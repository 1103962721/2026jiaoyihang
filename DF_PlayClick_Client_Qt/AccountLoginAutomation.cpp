#include <windows.h>
#include <basetyps.h>
#include <uiautomation.h>
#include <oleauto.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <string>
#pragma comment(lib, "uiautomationcore.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

#include "AccountLoginAutomation.h"

#include <QProcess>
#include <QThread>
#include <QElapsedTimer>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QSettings>

AccountLoginAutomation::AccountLoginAutomation(QObject* parent) : QObject(parent) {}

namespace
{
HWND FindWindowByLoginTitle()
{
    HWND result = nullptr;
    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
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
}

void AccountLoginAutomation::start(const QString& username, const QString& password)
{
    cancelled_ = false;
    username_ = username;
    password_ = password;
    QThread* thread = QThread::create([this]() {
        QString error;
        const auto fail = [this](const QString& text) {
            QString ignored;
            stopProcesses(ignored);
            emit finished(false, text);
        };
        emit progress(QStringLiteral("正在清理现有 Steam 和游戏进程"));
        if (!stopProcesses(error) || cancelled_)
        {
            fail(cancelled_ ? QStringLiteral("已取消") : error);
            return;
        }
        emit progress(QStringLiteral("正在启动 Steam，并持续等待登录窗口"));
        if (!launchSteam(error))
        {
            fail(error.isEmpty() ? QStringLiteral("无法发起 Steam 启动") : error);
            return;
        }
        if (!performSteamLogin(error))
        {
            fail(error);
            return;
        }
        if (cancelled_)
        {
            fail(QStringLiteral("已取消"));
            return;
        }
        if (username_.isEmpty() || password_.isEmpty())
        {
            fail(QStringLiteral("账号凭据为空"));
            return;
        }
        if (!launchGame(error))
        {
            fail(error);
            return;
        }
        emit progress(QStringLiteral("已发送启动游戏命令，等待游戏窗口"));
        if (!prepareAndStartLauncher(error))
        {
            fail(error);
            return;
        }
        if (!waitForGameWindow(60000))
        {
            fail(QStringLiteral("未检测到 Delta Force 游戏窗口"));
            return;
        }
        emit finished(true, QStringLiteral("游戏窗口已启动"));
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

bool AccountLoginAutomation::performSteamLogin(QString& errorText) const
{
    while (!cancelled())
    {
        HWND hwnd = FindWindowByLoginTitle();
        if (!hwnd) { QThread::msleep(300); continue; }
        const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IUIAutomation* automation = nullptr;
        IUIAutomationElement* root = nullptr;
        bool success = false;
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
                    if (isPassword && !password) password = item; else if (!isPassword && !user) user = item; else item->Release();
                }
            }
            IUIAutomationElement* button = FindNamed(automation, root, L"登录");
            if (!button) button = FindNamed(automation, root, L"Sign in");
            success = SetValue(user, username_) && SetValue(password, password_) && Invoke(button);
            if (user) user->Release(); if (password) password->Release(); if (button) button->Release();
            if (edits) edits->Release(); if (condition) condition->Release();
            root->Release(); automation->Release();
        }
        if (SUCCEEDED(init)) CoUninitialize();
        if (success) return true;
        QThread::msleep(300);
    }
    errorText = QStringLiteral("已取消");
    return false;
}

bool AccountLoginAutomation::prepareAndStartLauncher(QString& errorText) const
{
    if (!waitForProcess(QStringLiteral("df_launcher.exe"), 30000)) { errorText = QStringLiteral("未检测到 df_launcher"); return false; }
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* automation = nullptr; bool success = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&automation))))
    {
        HWND hwnd = FindWindowW(nullptr, L"Delta Force");
        if (hwnd)
        {
            IUIAutomationElement* root = nullptr;
            if (SUCCEEDED(automation->ElementFromHandle(hwnd, &root)))
            {
                VARIANT id{}; id.vt = VT_BSTR; id.bstrVal = SysAllocString(L"read-module");
                IUIAutomationCondition* condition = nullptr; IUIAutomationElement* check = nullptr;
                if (SUCCEEDED(automation->CreatePropertyCondition(UIA_AutomationIdPropertyId, id, &condition))) root->FindFirst(TreeScope_Descendants, condition, &check);
                if (check) { IUIAutomationTogglePattern* toggle = nullptr; if (SUCCEEDED(check->GetCurrentPatternAs(UIA_TogglePatternId, IID_PPV_ARGS(&toggle)))) { toggle->Toggle(); toggle->Release(); } check->Release(); }
                if (condition) condition->Release(); VariantClear(&id);
                IUIAutomationElement* start = FindNamed(automation, root, L"开始游戏"); if (!start) start = FindNamed(automation, root, L"Start Game");
                success = Invoke(start); if (start) start->Release(); root->Release();
            }
        }
        automation->Release();
    }
    if (SUCCEEDED(init)) CoUninitialize();
    if (!success) errorText = QStringLiteral("未找到 df_launcher 控件");
    return success;
}

void AccountLoginAutomation::cancel() { cancelled_ = true; }
void AccountLoginAutomation::cancelAndStopProcesses()
{
    cancelled_ = true;
    QString ignored;
    stopProcesses(ignored);
}
bool AccountLoginAutomation::cancelled() const { return cancelled_.load(); }

bool AccountLoginAutomation::stopProcesses(QString& errorText) const
{
    const QStringList names{QStringLiteral("steam.exe"), QStringLiteral("steamwebhelper.exe"),
        QStringLiteral("df_launcher.exe"), QStringLiteral("DeltaForceClient-Win64-Shipping.exe")};
    for (const QString& name : names)
    {
        QProcess::execute(QStringLiteral("taskkill"), {QStringLiteral("/F"), QStringLiteral("/IM"), name});
    }
    Q_UNUSED(errorText);
    return true;
}

bool AccountLoginAutomation::launchSteam(QString& errorText) const
{
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
    if (!QProcess::startDetached(QStringLiteral("cmd.exe"), {QStringLiteral("/C"), QStringLiteral("start"), QStringLiteral("steam://rungameid/2507950")}))
    {
        errorText = QStringLiteral("启动游戏命令失败");
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

bool AccountLoginAutomation::waitForGameWindow(int timeoutMs) const
{
    return waitForProcess(QStringLiteral("DeltaForceClient-Win64-Shipping.exe"), timeoutMs);
}
