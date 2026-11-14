#include <windows.h>
#include <tlhelp32.h>
#include <basetyps.h>
#include <uiautomation.h>
#include <oleauto.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <string>
#include <unordered_set>
#include <vector>
#include <optional>
#pragma comment(lib, "uiautomationcore.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

#include "AccountLoginAutomation.h"

#include <QProcess>
#include <QThread>
#include <QElapsedTimer>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QSettings>

#include "../Shared/ClientBindingRules.h"

AccountLoginAutomation::AccountLoginAutomation(QObject* parent) : QObject(parent)
{
    worker_ = std::thread([this]() { workerLoop(); });
}

AccountLoginAutomation::~AccountLoginAutomation()
{
    {
        std::lock_guard<std::mutex> lock(taskMutex_);
        stopping_ = true;
        if (latestTask_) latestTask_->cancelled = true;
        pendingTask_.reset();
    }
    taskChanged_.notify_one();
    if (worker_.joinable()) worker_.join();
}

void AccountLoginAutomation::submit(const std::shared_ptr<Task>& task)
{
    {
        std::lock_guard<std::mutex> lock(taskMutex_);
        if (stopping_) return;
        if (latestTask_) latestTask_->cancelled = true;
        task->id = ++nextTaskId_;
        latestTask_ = task;
        pendingTask_ = task;
    }
    taskChanged_.notify_one();
}

void AccountLoginAutomation::workerLoop()
{
    for (;;)
    {
        {
            std::unique_lock<std::mutex> lock(taskMutex_);
            taskChanged_.wait(lock, [this]() { return stopping_ || pendingTask_; });
            if (stopping_) return;
            workerTask_ = std::move(pendingTask_);
        }
        // A new login cannot launch processes until the previous task returns.
        if (!cancelled())
        {
            if (workerTask_->stopOnly)
            {
                QString ignored;
                stopProcesses(ignored, workerTask_->stopReason);
            }
            else
            {
                runLogin();
            }
        }
        workerTask_->completed = true;
        workerTask_.reset();
    }
}

void AccountLoginAutomation::postResult(const std::function<void()>& deliver)
{
    const auto task = workerTask_;
    QMetaObject::invokeMethod(this, [this, task, deliver]() {
        {
            std::lock_guard<std::mutex> lock(taskMutex_);
            if (stopping_ || latestTask_ != task || task->cancelled) return;
        }
        deliver();
    }, Qt::QueuedConnection);
}

void AccountLoginAutomation::reportProgress(const QString& text)
{
    reportLog(text);
    postResult([this, text]() { emit progress(text); });
}

void AccountLoginAutomation::reportLog(const QString& text)
{
    const QString tagged = QStringLiteral("[%1][上号任务 %2] %3")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")))
        .arg(workerTask_->id).arg(text);
    // Diagnostics describe actions that already happened, including cancelled
    // tasks. Filter stale UI/results, but never hide the reason for a process kill.
    QMetaObject::invokeMethod(this, [this, tagged]() { emit logMessage(tagged); }, Qt::QueuedConnection);
}

void AccountLoginAutomation::reportFinished(bool success, const QString& text)
{
    reportLog(QStringLiteral("上号阶段结束：%1，%2").arg(success ? QStringLiteral("成功") : QStringLiteral("失败"), text));
    postResult([this, success, text]() { emit finished(success, text); });
}

namespace
{
HWND FindWindowByLoginTitle()
{
    HWND result = nullptr;
    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        if (!IsWindowVisible(hwnd)) return TRUE;
        wchar_t title[256]{};
        GetWindowTextW(hwnd, title, 256);
        const std::wstring text(title);
        if (text.find(L"登录 Steam") != std::wstring::npos || text.find(L"Sign in to Steam") != std::wstring::npos)
        {
            *reinterpret_cast<HWND*>(param) = hwnd;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&result));
    return result;
}

bool SetValue(IUIAutomationElement* element, const QString& value)
{
    if (!element) return false;
    IUIAutomationValuePattern* pattern = nullptr;
    const HRESULT hr = element->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&pattern));
    if (FAILED(hr) || !pattern) return false;
    const std::wstring text = value.toStdWString();
    BSTR bstr = SysAllocString(text.c_str());
    const HRESULT result = bstr ? pattern->SetValue(bstr) : E_OUTOFMEMORY;
    if (bstr) SysFreeString(bstr);
    pattern->Release();
    return SUCCEEDED(result);
}

bool Invoke(IUIAutomationElement* element)
{
    if (!element) return false;
    IUIAutomationInvokePattern* pattern = nullptr;
    const HRESULT hr = element->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&pattern));
    if (FAILED(hr) || !pattern) return false;
    const HRESULT result = pattern->Invoke();
    pattern->Release();
    return SUCCEEDED(result);
}

IUIAutomationElement* FindNamed(IUIAutomation* automation, IUIAutomationElement* root, const wchar_t* name)
{
    VARIANT value{};
    value.vt = VT_BSTR;
    value.bstrVal = SysAllocString(name);
    IUIAutomationCondition* condition = nullptr;
    IUIAutomationElement* result = nullptr;
    if (SUCCEEDED(automation->CreatePropertyCondition(UIA_NamePropertyId, value, &condition)))
        root->FindFirst(TreeScope_Descendants, condition, &result);
    if (condition) condition->Release();
    VariantClear(&value);
    return result;
}

QStringList FindSteamExecutables()
{
    QStringList paths;
    const QStringList registryKeys{
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Valve\\Steam"),
        QStringLiteral("HKEY_LOCAL_MACHINE\\Software\\Valve\\Steam"),
        QStringLiteral("HKEY_LOCAL_MACHINE\\Software\\WOW6432Node\\Valve\\Steam")};
    const QStringList valueNames{QStringLiteral("InstallPath"), QStringLiteral("SteamPath")};
    for (const QString& key : registryKeys)
    {
        QSettings settings(key, QSettings::NativeFormat);
        for (const QString& valueName : valueNames)
        {
            const QString root = settings.value(valueName).toString().trimmed();
            if (root.isEmpty()) continue;
            const QString executable = QDir::fromNativeSeparators(root) + QStringLiteral("/steam.exe");
            if (QFileInfo::exists(executable) && !paths.contains(executable, Qt::CaseInsensitive))
                paths.push_back(executable);
        }
    }
    const QString pathExecutable = QStandardPaths::findExecutable(QStringLiteral("steam.exe"));
    if (!pathExecutable.isEmpty() && !paths.contains(pathExecutable, Qt::CaseInsensitive))
        paths.push_back(pathExecutable);
    return paths;
}

std::vector<HWND> FindSteamWindows()
{
    std::vector<HWND> windows;
    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        if (!IsWindowVisible(hwnd)) return TRUE;
        wchar_t title[256]{};
        GetWindowTextW(hwnd, title, 256);
        const std::wstring text(title);
        if (text.find(L"Steam") != std::wstring::npos
            || text.find(L"最终用户许可协议") != std::wstring::npos
            || text.find(L"End User License Agreement") != std::wstring::npos)
        {
            reinterpret_cast<std::vector<HWND>*>(param)->push_back(hwnd);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&windows));
    return windows;
}

std::vector<HWND> FindDeltaForceWindows()
{
    std::vector<HWND> windows;
    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        if (!IsWindowVisible(hwnd)) return TRUE;
        wchar_t title[256]{};
        GetWindowTextW(hwnd, title, 256);
        const std::wstring text(title);
        if (text.find(L"Delta Force") != std::wstring::npos
            || text.find(L"三角洲行动") != std::wstring::npos)
        {
            reinterpret_cast<std::vector<HWND>*>(param)->push_back(hwnd);
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&windows));
    return windows;
}

QString ProcessNameForWindow(HWND hwnd)
{
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == 0)
    {
        return {};
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process)
    {
        return {};
    }

    wchar_t path[32768]{};
    DWORD length = static_cast<DWORD>(std::size(path));
    QString name;
    if (QueryFullProcessImageNameW(process, 0, path, &length))
    {
        const wchar_t* separator = wcsrchr(path, L'\\');
        name = QString::fromWCharArray(separator ? separator + 1 : path);
    }
    CloseHandle(process);
    return name;
}

bool IsDeltaForceGameProcess(const QString& processName)
{
    return IsDeltaForceGameBodyProcessName(
        reinterpret_cast<const wchar_t*>(processName.utf16()));
}

bool HasDeltaForceGameWindow()
{
    bool found = false;
    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        if (!IsWindowVisible(hwnd))
        {
            return TRUE;
        }

        RECT client{};
        if (!GetClientRect(hwnd, &client) || client.right <= 0 || client.bottom <= 0)
        {
            return TRUE;
        }

        if (IsDeltaForceGameProcess(ProcessNameForWindow(hwnd)))
        {
            *reinterpret_cast<bool*>(param) = true;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&found));
    return found;
}

// nullopt means the snapshot failed, not that Steam exited.
std::optional<DWORD> SteamProcessId()
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return std::nullopt;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD pid = 0;
    BOOL more = Process32FirstW(snapshot, &entry);
    while (more)
    {
        if (_wcsicmp(entry.szExeFile, L"steam.exe") == 0)
        {
            pid = entry.th32ProcessID;
            break;
        }
        more = Process32NextW(snapshot, &entry);
    }
    const DWORD error = more ? ERROR_SUCCESS : GetLastError();
    CloseHandle(snapshot);
    if (pid == 0 && error != ERROR_NO_MORE_FILES) return std::nullopt;
    return pid;
}
}

void AccountLoginAutomation::start(const QString& username, const QString& password)
{
    auto task = std::make_shared<Task>();
    task->username = username;
    task->password = password;
    submit(task);
}

void AccountLoginAutomation::runLogin()
{
    QString error;
    const auto fail = [this](const QString& text) {
        // An ambiguous login/launcher failure must preserve Steam for diagnosis.
        // Explicit stop and the next account switch still own process cleanup.
        if (cancelled()) return;
        reportLog(QStringLiteral("上号失败，保留 Steam 和游戏进程以便排查"));
        reportFinished(false, text);
    };
    reportProgress(QStringLiteral("正在清理现有 Steam 和游戏进程"));
    if (!stopProcesses(error, QStringLiteral("上号开始清理旧进程")) || cancelled())
    {
        fail(cancelled() ? QStringLiteral("已取消") : error);
        return;
    }
    while (!cancelled())
    {
        reportProgress(QStringLiteral("正在启动 Steam，并持续等待登录窗口"));
        if (!launchSteam(error))
        {
            fail(error.isEmpty() ? QStringLiteral("无法发起 Steam 启动") : error);
            return;
        }

        bool steamExited = false;
        if (!performSteamLogin(error, steamExited))
        {
            if (cancelled()) return;
            if (!steamExited)
            {
                fail(error);
                return;
            }
            reportProgress(QStringLiteral("Steam 登录期间退出，稍后从启动 Steam 重新上号"));
        }
        else
        {
            if (workerTask_->username.isEmpty() || workerTask_->password.isEmpty())
            {
                fail(QStringLiteral("账号凭据为空"));
                return;
            }
            reportLog(QStringLiteral("登录窗口已稳定退出；准备发送 Steam 游戏启动 URI"));
            bool restartSteam = false;
            while (!cancelled())
            {
                if (!launchGame(error))
                {
                    fail(error);
                    return;
                }
                reportProgress(QStringLiteral("已发送 Steam 启动命令，等待 df_launcher"));
                bool stopRetrying = false;
                steamExited = false;
                if (prepareAndStartLauncher(error, stopRetrying, steamExited)) break;
                if (cancelled()) return;
                if (steamExited)
                {
                    reportProgress(QStringLiteral("等待 df_launcher 时 Steam 退出，稍后从启动 Steam 重新上号"));
                    restartSteam = true;
                    break;
                }
                if (stopRetrying)
                {
                    fail(error);
                    return;
                }
                reportProgress(QStringLiteral("未发现 df_launcher，重新发送 Steam 游戏启动命令"));
            }
            if (cancelled()) return;
            if (!restartSteam)
            {
                if (!waitForGameWindow(60000))
                {
                    if (!cancelled()) fail(QStringLiteral("未检测到 Delta Force 游戏窗口"));
                    return;
                }
                reportFinished(true, QStringLiteral("游戏窗口已启动"));
                return;
            }
        }
        QElapsedTimer retryDelay;
        retryDelay.start();
        while (!cancelled() && retryDelay.elapsed() < 1500) QThread::msleep(100);
    }
}

bool AccountLoginAutomation::performSteamLogin(QString& errorText, bool& steamExited)
{
    constexpr int SteamLoginWindowTimeoutMs = 30000;
    constexpr int SteamLoginConfirmationTimeoutMs = 180000;
    constexpr int SteamLoginStillWaitingLogMs = 30000;
    constexpr int StableWindowCloseMs = 5000;
    constexpr int SteamRecoveryGraceMs = 10000;
    constexpr int StableSteamProcessMs = 2000;
    QElapsedTimer timer;
    timer.start();
    steamExited = false;
    while (!cancelled())
    {
        if (timer.elapsed() >= SteamLoginWindowTimeoutMs)
        {
            const auto pid = SteamProcessId();
            steamExited = pid && *pid == 0;
            errorText = steamExited ? QStringLiteral("等待 Steam 登录窗口时进程已退出，准备重新启动 Steam")
                                    : QStringLiteral("30 秒内未能提交 Steam 登录；保留 Steam 进程");
            reportLog(errorText);
            return false;
        }
        HWND hwnd = FindWindowByLoginTitle();
        if (!hwnd)
        {
            QThread::msleep(300);
            continue;
        }
        const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IUIAutomation* automation = nullptr;
        IUIAutomationElement* root = nullptr;
        bool invoked = false;
        if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation)))
            && SUCCEEDED(automation->ElementFromHandle(hwnd, &root)))
        {
            VARIANT type{}; type.vt = VT_I4; type.lVal = UIA_EditControlTypeId;
            IUIAutomationCondition* condition = nullptr; IUIAutomationElementArray* edits = nullptr;
            if (SUCCEEDED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId, type, &condition)))
                root->FindAll(TreeScope_Descendants, condition, &edits);
            IUIAutomationElement* user = nullptr; IUIAutomationElement* password = nullptr;
            if (edits)
            {
                int length = 0; edits->get_Length(&length);
                for (int i = 0; i < length; ++i)
                {
                    IUIAutomationElement* item = nullptr; edits->GetElement(i, &item);
                    if (!item) continue;
                    VARIANT flag{}; item->GetCurrentPropertyValue(UIA_IsPasswordPropertyId, &flag);
                    const bool isPassword = flag.vt == VT_BOOL && flag.boolVal == VARIANT_TRUE; VariantClear(&flag);
                    if (isPassword && !password) password = item;
                    else if (!isPassword && !user) user = item;
                    else item->Release();
                }
            }
            IUIAutomationElement* button = FindNamed(automation, root, L"登录");
            if (!button) button = FindNamed(automation, root, L"Sign in");
            if (user && password && button && !cancelled())
            {
                reportLog(QStringLiteral("Steam 登录：开始填写并提交凭据"));
                const bool fieldsSet = SetValue(user, workerTask_->username) && SetValue(password, workerTask_->password);
                invoked = fieldsSet && !cancelled() && Invoke(button);
                reportLog(invoked ? QStringLiteral("Steam 登录：提交按钮调用成功，等待登录状态")
                                  : QStringLiteral("Steam 登录：提交按钮调用失败"));
            }
            if (user) user->Release(); if (password) password->Release(); if (button) button->Release();
            if (edits) edits->Release(); if (condition) condition->Release();
            root->Release(); automation->Release();
        }
        if (SUCCEEDED(init)) CoUninitialize();
        if (!invoked)
        {
            if (cancelled()) break;
            QThread::msleep(300);
            continue;
        }
        QElapsedTimer confirmation;
        confirmation.start();
        QElapsedTimer absent;
        bool wasAbsent = false;
        bool stillWaitingLogged = false;
        QElapsedTimer steamRecovery;
        bool steamWasMissing = false;
        QElapsedTimer steamStable;
        DWORD stableSteamPid = 0;
        while (!cancelled() && confirmation.elapsed() < SteamLoginConfirmationTimeoutMs)
        {
            if (!stillWaitingLogged && confirmation.elapsed() >= SteamLoginStillWaitingLogMs)
            {
                stillWaitingLogged = true;
                reportProgress(QStringLiteral("Steam 登录仍在进行，已等待 30 秒；继续等待登录窗口关闭"));
            }
            if (FindWindowByLoginTitle())
            {
                wasAbsent = false;
                stableSteamPid = 0;
                const auto pid = SteamProcessId();
                if (pid && *pid == 0)
                {
                    if (!steamWasMissing)
                    {
                        steamWasMissing = true;
                        steamRecovery.start();
                        reportLog(QStringLiteral("Steam 登录窗口仍在，但进程暂时消失；等待最多 10 秒观察恢复"));
                    }
                    if (steamRecovery.elapsed() >= SteamRecoveryGraceMs)
                    {
                        steamExited = true;
                        errorText = QStringLiteral("Steam 登录期间进程持续消失，准备从启动 Steam 重新上号");
                        reportLog(errorText);
                        return false;
                    }
                }
                else if (pid)
                {
                    steamWasMissing = false;
                }
                QThread::msleep(300);
                continue;
            }
            if (!wasAbsent)
            {
                wasAbsent = true;
                absent.start();
                reportLog(QStringLiteral("Steam 登录窗口已关闭，确认其稳定退出"));
            }
            if (absent.elapsed() >= StableWindowCloseMs)
            {
                const auto pid = SteamProcessId();
                if (pid && *pid != 0)
                {
                    if (stableSteamPid != *pid || steamWasMissing)
                    {
                        stableSteamPid = *pid;
                        steamStable.start();
                        if (steamWasMissing) reportLog(QStringLiteral("Steam 进程已恢复，PID=%1；确认稳定运行").arg(*pid));
                    }
                    steamWasMissing = false;
                    if (steamStable.elapsed() >= StableSteamProcessMs)
                    {
                        reportLog(QStringLiteral("Steam 进程稳定运行；窗口消失仅作为登录完成的近似信号"));
                        return true;
                    }
                }
                else if (pid)
                {
                    stableSteamPid = 0;
                    if (!steamWasMissing)
                    {
                        steamWasMissing = true;
                        steamRecovery.start();
                        reportLog(QStringLiteral("Steam 登录窗口已关闭，但进程暂时消失；等待最多 10 秒观察恢复"));
                    }
                    if (steamRecovery.elapsed() >= SteamRecoveryGraceMs)
                    {
                        steamExited = true;
                        errorText = QStringLiteral("Steam 登录后进程持续消失，准备从启动 Steam 重新上号");
                        reportLog(errorText);
                        return false;
                    }
                }
                else
                {
                    errorText = QStringLiteral("无法读取 Steam 进程状态；已保留现场，请检查日志");
                    reportLog(errorText);
                    return false;
                }
            }
            QThread::msleep(150);
        }
        if (cancelled())
        {
            errorText = QStringLiteral("已取消");
            return false;
        }
        errorText = QStringLiteral("提交登录后 180 秒内登录窗口仍未稳定关闭；未启动游戏，Steam 保持运行");
        reportLog(errorText);
        return false;
    }
    errorText = QStringLiteral("已取消");
    return false;
}



bool AccountLoginAutomation::acceptSteamAgreementIfPresent() const
{
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) return false;

    IUIAutomation* automation = nullptr;
    bool accepted = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation))))
    {
        for (const HWND hwnd : FindSteamWindows())
        {
            IUIAutomationElement* root = nullptr;
            if (FAILED(automation->ElementFromHandle(hwnd, &root)) || !root) continue;

            // 不依赖弹窗标题：Steam 的确认窗口标题和语言可能变化，只要窗口内出现可操作的接受按钮就处理。
            IUIAutomationElement* accept = FindNamed(automation, root, L"接受");
            if (!accept) accept = FindNamed(automation, root, L"Accept");
            if (!accept) accept = FindNamed(automation, root, L"I Agree");
            if (!accept) accept = FindNamed(automation, root, L"同意");
            if (accept)
            {
                accepted = (!cancelled() && Invoke(accept)) || accepted;
                accept->Release();
            }
            root->Release();
            if (accepted) break;
        }
        automation->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return accepted;
}

bool AccountLoginAutomation::clickSteamOpenGameIfPresent() const
{
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) return false;

    IUIAutomation* automation = nullptr;
    bool clicked = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation))))
    {
        for (const HWND hwnd : FindSteamWindows())
        {
            IUIAutomationElement* root = nullptr;
            if (FAILED(automation->ElementFromHandle(hwnd, &root)) || !root) continue;

            IUIAutomationElement* openGame = FindNamed(automation, root, L"打开游戏");
            if (!openGame) openGame = FindNamed(automation, root, L"Open Game");
            if (openGame)
            {
                clicked = !cancelled() && Invoke(openGame);
                openGame->Release();
            }
            root->Release();
            if (clicked) break;
        }
        automation->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return clicked;
}

bool AccountLoginAutomation::prepareAndStartLauncher(QString& errorText, bool& stopRetrying, bool& steamExited)
{
    QElapsedTimer timer;
    timer.start();
    constexpr int LauncherProcessTimeoutMs = 60000;
    constexpr int LauncherControlTimeoutMs = 120000;
    bool steamOpenGameClicked = false;
    bool steamAgreementAccepted = false;
    stopRetrying = false;
    steamExited = false;
    QElapsedTimer steamAbsentTimer;
    bool steamWasAbsent = false;
    DWORD lastSteamPid = 0;
    bool snapshotFailureReported = false;
    QElapsedTimer loginWindowTimer;
    bool loginWindowReturned = false;
    std::unordered_set<HWND> toggleRequested;
    bool foundCheckbox = false;
    bool confirmedChecked = false;
    bool foundStartButton = false;
    bool launcherSeen = false;
    QElapsedTimer launcherControlsTimer;
    QElapsedTimer launcherAbsentTimer;
    bool launcherWasAbsent = false;
    while (!cancelled() && (launcherSeen ? launcherControlsTimer.elapsed() < LauncherControlTimeoutMs
                                         : timer.elapsed() < LauncherProcessTimeoutMs))
    {
        if (HasDeltaForceGameWindow())
        {
            const auto steamPid = SteamProcessId();
            if (steamPid && *steamPid != 0) return true;
        }
        if (FindWindowByLoginTitle())
        {
            if (!loginWindowReturned)
            {
                loginWindowReturned = true;
                loginWindowTimer.start();
                reportLog(QStringLiteral("发送游戏启动命令后，Steam 登录窗口再次出现"));
            }
            if (loginWindowTimer.elapsed() >= 10000)
            {
                stopRetrying = true;
                errorText = QStringLiteral("Steam 登录窗口再次出现且持续 10 秒；已停止重复启动游戏");
                reportLog(errorText);
                return false;
            }
        }
        else
        {
            loginWindowReturned = false;
        }
        if (acceptSteamAgreementIfPresent() && !steamAgreementAccepted)
        {
            steamAgreementAccepted = true;
            reportLog(QStringLiteral("Steam 确认弹窗已调用接受按钮"));
        }
        if (!steamOpenGameClicked && clickSteamOpenGameIfPresent())
        {
            steamOpenGameClicked = true;
            reportLog(QStringLiteral("Steam 打开游戏弹窗已调用按钮"));
        }
        const bool launcherPresent = waitForProcess(QStringLiteral("df_launcher.exe"), 250);
        if (launcherPresent && !launcherSeen)
        {
            launcherSeen = true;
            launcherControlsTimer.start();
            reportLog(QStringLiteral("首次检测到 df_launcher.exe，开始等待勾选框和开始游戏按钮"));
        }
        if (launcherPresent) launcherWasAbsent = false;
        else if (launcherSeen && !launcherWasAbsent)
        {
            launcherWasAbsent = true;
            launcherAbsentTimer.start();
            reportLog(QStringLiteral("df_launcher.exe 在开始游戏前消失，等待最多 10 秒观察恢复"));
        }
        const auto steamPid = SteamProcessId();
        if (steamPid && *steamPid == 0)
        {
            if (!steamWasAbsent)
            {
                steamAbsentTimer.start();
                steamWasAbsent = true;
                reportLog(QStringLiteral("Steam 进程暂时消失，观察是否重启；此前 PID=%1").arg(lastSteamPid));
            }
            if (steamAbsentTimer.elapsed() >= 10000)
            {
                stopRetrying = true;
                steamExited = true;
                errorText = QStringLiteral("Steam 进程持续 10 秒未恢复；准备重新启动 Steam");
                reportLog(errorText);
                return false;
            }
        }
        else if (steamPid)
        {
            if (steamWasAbsent || lastSteamPid != *steamPid)
                reportLog(QStringLiteral("Steam 进程可见，PID=%1%2")
                    .arg(*steamPid).arg(steamWasAbsent ? QStringLiteral("，已恢复") : QString()));
            lastSteamPid = *steamPid;
            steamWasAbsent = false;
            snapshotFailureReported = false;
        }
        else if (!snapshotFailureReported)
        {
            reportLog(QStringLiteral("无法读取 Steam 进程快照，继续等待启动器"));
            snapshotFailureReported = true;
        }

        if (launcherWasAbsent && launcherAbsentTimer.elapsed() >= 10000
            && steamPid && *steamPid != 0)
        {
            errorText = QStringLiteral("df_launcher.exe 持续消失，Steam 仍在运行；重新发送游戏启动命令");
            reportLog(errorText);
            return false;
        }

        if (!launcherPresent) continue;
        const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IUIAutomation* automation = nullptr;
        bool started = false;
        if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation))))
        {
            for (const HWND hwnd : FindDeltaForceWindows())
            {
                IUIAutomationElement* root = nullptr;
                if (FAILED(automation->ElementFromHandle(hwnd, &root)) || !root) continue;
                VARIANT id{}; id.vt = VT_BSTR; id.bstrVal = SysAllocString(L"read-module");
                IUIAutomationCondition* condition = nullptr; IUIAutomationElement* check = nullptr;
                if (SUCCEEDED(automation->CreatePropertyCondition(UIA_AutomationIdPropertyId, id, &condition)))
                    root->FindFirst(TreeScope_Descendants, condition, &check);
                if (condition) condition->Release(); VariantClear(&id);
                IUIAutomationElement* start = FindNamed(automation, root, L"开始游戏");
                if (!start) start = FindNamed(automation, root, L"Start Game");
                if (check)
                {
                    foundCheckbox = true;
                    IUIAutomationTogglePattern* toggle = nullptr;
                    if (SUCCEEDED(check->GetCurrentPatternAs(UIA_TogglePatternId, IID_PPV_ARGS(&toggle))) && toggle)
                    {
                        ToggleState state = ToggleState_Indeterminate;
                        if (SUCCEEDED(toggle->get_CurrentToggleState(&state)))
                        {
                            if (state == ToggleState_On)
                            {
                                confirmedChecked = true;
                                toggleRequested.erase(hwnd);
                                if (start)
                                {
                                    foundStartButton = true;
                                    BOOL enabled = FALSE;
                                    if (SUCCEEDED(start->get_CurrentIsEnabled(&enabled)) && enabled && !cancelled())
                                    {
                                        const auto steamCheck = SteamProcessId();
                                        if (steamCheck && *steamCheck != 0)
                                        {
                                            started = Invoke(start);
                                            if (started) reportLog(QStringLiteral("df_launcher 勾选状态已确认，开始游戏按钮已调用"));
                                        }
                                    }
                                }
                            }
                            else if (state == ToggleState_Off && !cancelled()
                                && toggleRequested.insert(hwnd).second)
                            {
                                if (SUCCEEDED(toggle->Toggle()))
                                    reportLog(QStringLiteral("df_launcher 已请求勾选 read-module，等待状态变为已勾选"));
                                else
                                    toggleRequested.erase(hwnd);
                            }
                        }
                        toggle->Release();
                    }
                }
                if (start) start->Release(); if (check) check->Release(); root->Release();
                if (started) break;
            }
            automation->Release();
        }
        if (SUCCEEDED(init)) CoUninitialize();
        if (started) return true;
        QThread::msleep(250);
    }
    if (cancelled()) errorText = QStringLiteral("已取消");
    else if (!launcherSeen) errorText = QStringLiteral("60 秒内未检测到 df_launcher.exe；将重新发送 Steam 游戏启动命令");
    else if (!foundCheckbox) errorText = QStringLiteral("df_launcher 已运行，但 120 秒内未找到 read-module 勾选框");
    else if (!confirmedChecked)
    {
        stopRetrying = true;
        errorText = QStringLiteral("df_launcher 等待超时：勾选框未确认处于已勾选状态");
    }
    else if (!foundStartButton) errorText = QStringLiteral("df_launcher 等待超时：已勾选但未找到开始游戏按钮");
    else errorText = QStringLiteral("df_launcher 等待超时：已勾选但开始游戏按钮不可用或调用失败");
    if (launcherSeen) stopRetrying = true;
    if (!cancelled()) reportLog(errorText);
    return false;
}



void AccountLoginAutomation::cancel()
{
    {
        std::lock_guard<std::mutex> lock(taskMutex_);
        if (stopping_ || !latestTask_ || latestTask_->stopOnly) return;
        latestTask_->cancelled = true;
        // Cancelling an unrelated action must not kill an already logged-in game.
        if (latestTask_->completed) return;
        auto task = std::make_shared<Task>();
        task->stopOnly = true;
        task->stopReason = QStringLiteral("取消上号");
        task->id = ++nextTaskId_;
        latestTask_ = task;
        pendingTask_ = task;
    }
    taskChanged_.notify_one();
}
void AccountLoginAutomation::cancelAndStopProcesses(const QString& reason)
{
    auto task = std::make_shared<Task>();
    task->stopOnly = true;
    task->stopReason = reason.isEmpty() ? QStringLiteral("外部取消上号") : reason;
    submit(task);
}
bool AccountLoginAutomation::cancelled() const { return workerTask_->cancelled.load(); }

bool AccountLoginAutomation::stopProcesses(QString& errorText, const QString& reason)
{
    const QStringList names{QStringLiteral("steam.exe"), QStringLiteral("steamwebhelper.exe"),
        QStringLiteral("df_launcher.exe"), QStringLiteral("DeltaForceClient-Win64-Shipping.exe"),
        QStringLiteral("eltaForceClient-Win64-Shipping.exe")};
    for (const QString& name : names)
    {
        if (cancelled()) return false;
        const int result = QProcess::execute(QStringLiteral("taskkill"),
            {QStringLiteral("/F"), QStringLiteral("/IM"), name});
        reportLog(QStringLiteral("进程清理：原因=%1，目标=%2，taskkill返回=%3")
            .arg(reason.isEmpty() ? QStringLiteral("未标注") : reason, name).arg(result));
    }
    Q_UNUSED(errorText);
    return true;
}

bool AccountLoginAutomation::launchSteam(QString& errorText) const
{
    if (cancelled()) return false;
    bool launchRequested = false;
    for (const QString& executable : FindSteamExecutables())
    {
        launchRequested = QProcess::startDetached(executable, {});
        if (launchRequested) break;
    }

    // steam.exe 经常不在 PATH；Steam 注册的 URI 是可靠的第二种启动入口。
    if (!launchRequested)
    {
        const HINSTANCE result = ShellExecuteW(nullptr, L"open", L"steam://open/login", nullptr, nullptr, SW_SHOWNORMAL);
        launchRequested = reinterpret_cast<INT_PTR>(result) > 32;
    }

    // 部分机器未注册 URI，但安装路径仍可从常见位置启动。
    if (!launchRequested)
    {
        const QStringList candidates{
            QStringLiteral("C:/Program Files (x86)/Steam/steam.exe"),
            QStringLiteral("C:/Program Files/Steam/steam.exe"),
            QStringLiteral("D:/Steam/steam.exe")};
        for (const QString& candidate : candidates)
        {
            if (!QFileInfo::exists(candidate)) continue;
            launchRequested = QProcess::startDetached(candidate, {});
            if (launchRequested) break;
        }
    }

    if (!launchRequested)
    {
        errorText = QStringLiteral("未找到 Steam 启动入口");
        return false;
    }
    return true;
}

bool AccountLoginAutomation::launchGame(QString& errorText) const
{
    if (cancelled()) return false;
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", L"steam://rungameid/2507950",
        nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32)
    {
        errorText = QStringLiteral("系统未能打开 Steam 游戏启动 URI（ShellExecute 返回 %1）")
            .arg(reinterpret_cast<INT_PTR>(result));
        return false;
    }
    return true;
}

bool AccountLoginAutomation::waitForProcess(const QString& processName, int timeoutMs) const
{
    QElapsedTimer timer;
    timer.start();
    while (!cancelled() && timer.elapsed() < timeoutMs)
    {
        QProcess probe;
        probe.start(QStringLiteral("tasklist"), {QStringLiteral("/FI"), QStringLiteral("IMAGENAME eq %1").arg(processName)});
        probe.waitForFinished(1000);
        if (QString::fromLocal8Bit(probe.readAllStandardOutput()).contains(processName, Qt::CaseInsensitive)) return true;
        QThread::msleep(250);
    }
    return false;
}

bool AccountLoginAutomation::waitForGameWindow(int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!cancelled() && timer.elapsed() < timeoutMs)
    {
        if (HasDeltaForceGameWindow())
        {
            return true;
        }
        QThread::msleep(250);
    }
    return false;
}
