#include "ClientController.h"

#include "BulletOcrRules.h"
#include "OcrNumberParser.h"

#include <QApplication>
#include <QBuffer>
#include <QCursor>
#include <QByteArray>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QPixmap>
#include <QPointer>
#include <QRegularExpression>
#include <QRandomGenerator>
#include <QScreen>
#include <QStandardPaths>
#include <QStringList>
#include <QThread>

#include <cmath>
#include <thread>

#include "../Shared/ClientBindingRules.h"
#include "../Shared/ClientCountdownRules.h"
#include "../Shared/Protocol.h"

namespace
{
const UINT WM_DF_PLAYCLICK_DELAY_CHANGED = RegisterWindowMessageW(L"DF_PlayClick_ClickQueueDelayChanged");

QString NormalizeAmountOcrText(QString text)
{
    text.replace(QChar(0xFF0C), QChar(','));
    text.replace(QChar(0x3002), QChar('.'));
    text.replace(QChar(0xFF0E), QChar('.'));
    // 资金 OCR 结果可能包含中文说明，只保留金额解析需要的字符。
    text.remove(QRegularExpression(QStringLiteral("[^0-9,.KkMm]")));
    return text;
}

QString NormalizeAmountNumericToken(QString value)
{
    // 游戏资金是整数，OCR 中的逗号、句点以及多段分组点都只是千位分隔符。
    // 统一只保留数字，避免“11.328,573”被当成 11.328573，最终截断成 11。
    // 例如：12.133.12 -> 1213312，随后再由服务端格式化为 1,213,312。
    value.remove(QRegularExpression(QStringLiteral("[^0-9]")));
    return value;
}

QString NormalizeLoginOcrText(QString value)
{
    value.remove(QRegularExpression(QStringLiteral("[\\s\\p{P}]")));
    return value.toCaseFolded();
}

QString CaptureDesktopOcrRegion(HWND hwnd, const OcrRegionConfig& region, const QString& filePrefix, QString& errorText)
{
    errorText.clear();
    if (!hwnd || !IsWindow(hwnd))
    {
        errorText = QStringLiteral("游戏窗口无效");
        return {};
    }
    RECT clientRect{};
    if (!GetClientRect(hwnd, &clientRect))
    {
        errorText = QStringLiteral("读取游戏客户区失败");
        return {};
    }
    const QSize clientSize(clientRect.right - clientRect.left, clientRect.bottom - clientRect.top);
    const QRect scaledRegion = ScaleReferenceRect(
        QRect(region.x, region.y, region.width, region.height),
        QSize(region.referenceWidth, region.referenceHeight), clientSize);
    if (!IsRectInsideClient(scaledRegion, clientSize))
    {
        errorText = QStringLiteral("OCR 截图区域超出游戏客户区");
        return {};
    }
    POINT origin{0, 0};
    if (!ClientToScreen(hwnd, &origin))
    {
        errorText = QStringLiteral("客户区坐标转屏幕坐标失败");
        return {};
    }
    HDC screen = GetDC(nullptr);
    HDC memory = screen ? CreateCompatibleDC(screen) : nullptr;
    HBITMAP bitmap = (screen && memory)
        ? CreateCompatibleBitmap(screen, scaledRegion.width(), scaledRegion.height()) : nullptr;
    if (!screen || !memory || !bitmap)
    {
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        if (screen) ReleaseDC(nullptr, screen);
        errorText = QStringLiteral("创建桌面截图失败");
        return {};
    }
    HGDIOBJ previous = SelectObject(memory, bitmap);
    const BOOL copied = BitBlt(memory, 0, 0, scaledRegion.width(), scaledRegion.height(), screen,
        origin.x + scaledRegion.x(), origin.y + scaledRegion.y(), SRCCOPY | CAPTUREBLT);
    QImage image;
    if (copied)
    {
        image = QImage(scaledRegion.width(), scaledRegion.height(), QImage::Format_RGB32);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = scaledRegion.width();
        info.bmiHeader.biHeight = -scaledRegion.height();
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        if (GetDIBits(memory, bitmap, 0, static_cast<UINT>(scaledRegion.height()), image.bits(), &info, DIB_RGB_COLORS) == 0)
        {
            image = {};
        }
    }
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    if (image.isNull())
    {
        errorText = QStringLiteral("桌面截图失败");
        return {};
    }
    const QString imagePath = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
        .filePath(QStringLiteral("df_playclick_%1_%2.png").arg(filePrefix).arg(reinterpret_cast<quintptr>(hwnd)));
    if (!image.save(imagePath, "PNG"))
    {
        errorText = QStringLiteral("保存 OCR 截图失败");
        return {};
    }
    return imagePath;
}

QString NormalizeBanOcrText(QString text)
{
    text.remove(QRegularExpression(QStringLiteral("[\\s\\p{P}]")));
    return text;
}

bool IsBanOcrText(const QString& recognizedText)
{
    const QString text = NormalizeBanOcrText(recognizedText);
    const bool abnormalEnvironment = text.contains(QStringLiteral("游戏环境及行为数据异常"), Qt::CaseInsensitive)
        || (text.contains(QStringLiteral("游戏环境"), Qt::CaseInsensitive)
            && text.contains(QStringLiteral("行为数据异常"), Qt::CaseInsensitive));
    const bool accountFrozen = text.contains(QStringLiteral("账号进行冻结"), Qt::CaseInsensitive)
        || text.contains(QStringLiteral("账号冻结"), Qt::CaseInsensitive)
        || text.contains(QStringLiteral("冻结"), Qt::CaseInsensitive);
    return abnormalEnvironment && accountFrozen;
}
}

ClientController::ClientController(QObject* parent)
    : QObject(parent)
{
    unitPrice_ = config_.loadUnitPrice();
    bulletTotal_ = config_.loadBulletTotal();
    countdownTimer_.setInterval(1000);
    scheduledTriggerTimer_.setSingleShot(true);
    scheduledTriggerTimer_.setTimerType(Qt::PreciseTimer);
    serverTimeTimer_.setInterval(50);
    bulletOcrActionTimer_.setSingleShot(true);
    bulletOcrNextRoundTimer_.setSingleShot(true);
    connect(&countdownTimer_, &QTimer::timeout, this, &ClientController::tickCountdown);
    connect(&scheduledTriggerTimer_, &QTimer::timeout, this, &ClientController::fireScheduledTrigger);
    connect(&serverTimeTimer_, &QTimer::timeout, this, &ClientController::updateServerTimeDisplay);
    serverTimeTimer_.start();
    connect(&bulletOcrActionTimer_, &QTimer::timeout, this, [this]() {
        if (!bulletOcrRunning_)
        {
            return;
        }
        QString errorText;
        if (!input_.pressKey(VK_ESCAPE, errorText))
        {
            finishBulletOcrLoop(false, errorText);
            return;
        }
        scheduleNextBulletOcrRound(true);
    });
    connect(&bulletOcrNextRoundTimer_, &QTimer::timeout, this, &ClientController::runBulletOcrRound);
    connect(&connection_, &ClientConnection::connectedChanged, this, [this](bool connected) {
        if (connected)
        {
            // 等待服务端完成首次校时，避免把本机时间误显示为服务器时间。
            serverTimeSynchronized_ = false;
            serverClockOffsetMs_ = 0;
            // READY 1 已在连接层发送成功，此处同步本地状态供控制器查询。
            ready_ = true;
            emit readyChanged(true);
        }
        else if (!connection_.isReconnecting())
        {
            serverTimeSynchronized_ = false;
            serverClockOffsetMs_ = 0;
            clearLocalReadyState();
            teamReadyOcrRunning_ = false;
            lastTeamReadyState_ = -2;
            lastTeamReadyCurrent_ = -1;
            lastTeamReadyTotal_ = -1;
        }
        emit connectedChanged(connected);
    });
    connect(&connection_, &ClientConnection::reconnectingChanged, this, [this](bool reconnecting) {
        emit reconnectingChanged(reconnecting);
    });
    connect(&connection_, &ClientConnection::statusChanged, this, &ClientController::setStatus);
    connect(&connection_, &ClientConnection::connectionLogAppended, this, [this](const QString& text) {
        emit connectionLogAppended(text);
    });
    connect(&connection_, &ClientConnection::triggerReceived, this, &ClientController::startCountdown);
    connect(&connection_, &ClientConnection::scheduledTriggerReceived, this, &ClientController::startScheduledTrigger);
    connect(&connection_, &ClientConnection::serverTimeReceived, this, [this](qint64 serverTimeMs, qint64 clientOffsetMs) {
        Q_UNUSED(serverTimeMs);
        // clientOffsetMs 是服务端基于四时间戳计算出的“客户端时钟 - 服务端时钟”偏移，
        // 即使偏移恰好为 0 也必须保留，不能再用 0 作为未同步哨兵值。
        serverClockOffsetMs_ = clientOffsetMs;
        serverTimeSynchronized_ = true;
        updateServerTimeDisplay();
    });
    connect(&connection_, &ClientConnection::roleReceived, this, [this](ClientRole role) {
        currentRole_ = role;
        emit clientRoleChanged(currentRole_);
        setStatus(QStringLiteral("服务端已下发角色：%1")
            .arg(QString::fromStdWString(ClientRoleUtil::ToDisplayText(currentRole_))));
    });
    connect(&connection_, &ClientConnection::teamCodeRequestReceived, this, &ClientController::handleTeamCodeRequest);
    connect(&connection_, &ClientConnection::teamReadyRequestReceived, this, &ClientController::handleTeamReadyRequest);
    connect(&connection_, &ClientConnection::prepareRequestReceived, this, &ClientController::handlePrepareRequest);
    connect(&connection_, &ClientConnection::rebindWindowRequestReceived, this, &ClientController::rebindGameWindow);
    connect(&connection_, &ClientConnection::forceStopActionRequestReceived, this, &ClientController::forceStopCurrentAction);
    connect(&connection_, &ClientConnection::forceStopActionImmediate, this, [this]() {
        automationCancelRequested_.store(true);
    }, Qt::DirectConnection);
    connect(&connection_, &ClientConnection::ocrRequestReceived, this, &ClientController::handleOcrRequest);
    connect(&connection_, &ClientConnection::selfThunderRequestReceived, this, &ClientController::handleSelfThunderRequest);
    connect(&connection_, &ClientConnection::loadoutRequestReceived, this, &ClientController::handleLoadoutRequest);
    connect(&connection_, &ClientConnection::saveLoadoutRequestReceived, this, &ClientController::handleSaveLoadoutRequest);
    connect(&connection_, &ClientConnection::listingRequestReceived, this, &ClientController::handleListingRequest);
    connect(&connection_, &ClientConnection::mailRequestReceived, this, &ClientController::handleMailRequest);
    connect(&connection_, &ClientConnection::accountLoginRequestReceived, this, &ClientController::handleAccountLoginRequest);
    connect(&accountLoginAutomation_, &AccountLoginAutomation::progress, this, [this](const QString& text) {
        setStatus(text);
    });
    connect(&accountBanMonitor_, &AccountBanMonitor::bannedDetected, this, &ClientController::handleAccountBanned);
    connect(&accountBanMonitor_, &AccountBanMonitor::monitorStatus, this, [this](const QString& text) {
        setStatus(text);
    });
    connect(&accountBanMonitor_, &AccountBanMonitor::monitorStopped, this, [this]() {
        if (!accountBanMonitor_.isRunning())
        {
            accountBanMonitoringEnabled_ = false;
        }
    });
    connect(&accountLoginSuccessMonitor_, &AccountLoginSuccessMonitor::loginReady, this, &ClientController::handleLoginReady);
    connect(&accountLoginSuccessMonitor_, &AccountLoginSuccessMonitor::bannedDetected, this, &ClientController::handleAccountBanned);
    connect(&accountLoginSuccessMonitor_, &AccountLoginSuccessMonitor::ocrRequested, this, &ClientController::runLoginSuccessOcr, Qt::QueuedConnection);
    connect(&accountLoginSuccessMonitor_, &AccountLoginSuccessMonitor::residentFound, this, [this]() {
        if (!boundHwnd_) return; QString error;
        POINT p{386, 1192}; input_.clickReferencePoint(boundHwnd_, p, 2560, 1440, 0, error);
        QTimer::singleShot(1500, this, [this]() { setStatus(QStringLiteral("常驻点击完成，继续检测登录界面")); });
    });
    connect(&accountLoginSuccessMonitor_, &AccountLoginSuccessMonitor::tabFound, this, [this]() {
        QString error; input_.pressKey(VK_TAB, error);
    });
    connect(&accountLoginSuccessMonitor_, &AccountLoginSuccessMonitor::retrySpace, this, [this]() {
        // holdKey() waits for the complete hold interval. Run it away from the
        // Qt UI thread so the 3-second fallback cannot make the window hang.
        QPointer<ClientController> controller(this);
        std::thread([controller]() {
            Win32InputDispatcher dispatcher;
            QString error;
            if (!dispatcher.holdKey(VK_SPACE, 3000, error) && controller)
            {
                QMetaObject::invokeMethod(controller, [controller, error]() {
                    if (controller)
                    {
                        controller->setStatus(QStringLiteral("空格保底输入失败：%1").arg(error));
                    }
                }, Qt::QueuedConnection);
            }
        }).detach();
    });
    connect(&accountLoginSuccessMonitor_, &AccountLoginSuccessMonitor::monitorStatus, this, [this](const QString& text) { setStatus(text); });
    connect(&accountLoginAutomation_, &AccountLoginAutomation::finished, this, [this](bool success, const QString& text) {
        bool finalSuccess = success;
        QString resultText = text;
        if (success)
        {
            // 游戏进程和顶层窗口通常不是同时创建，给窗口初始化留出时间再绑定。
            QElapsedTimer bindTimer;
            bindTimer.start();
            bool bound = false;
            while (!bound && bindTimer.elapsed() < 15000)
            {
                bound = autoBindGameWindow(false);
                if (bound)
                {
                    break;
                }
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                QThread::msleep(250);
            }
            if (!bound)
            {
                finalSuccess = false;
                resultText = QStringLiteral("游戏窗口已出现，但自动绑定失败");
            }
            else
            {
                accountBanHandled_ = false;
                accountBanMonitoringEnabled_ = true;
                accountLoginReadyReported_ = false;
                accountLoginSuccessMonitor_.start(boundHwnd_, config_.loadUmiOcrPath(), config_.loadUmiOcrPort());
            }
        }
        if (!finalSuccess)
        {
            stopAccountBanMonitor();
        }
        connection_.sendAccountLoginResponse(accountLoginRequestId_, finalSuccess ? QStringLiteral("OK") : QStringLiteral("ERROR"), resultText);
        // 成功拉起游戏只表示自动化启动阶段完成。登录成功/封禁监测仍属于本次上号流程，
        // 必须保持 accountLoginRunning_，否则后续 OCR 请求会被直接丢弃。
        if (!finalSuccess)
        {
            accountLoginRunning_ = false;
        }
        finishAutomationAction();
        setStatus(resultText);
    });
    connect(&hotkey_, &HotkeyManager::hotkeyPressed, this, &ClientController::finishHotkeyPick);
    connect(&hotkey_, &HotkeyManager::f3Pressed, this, &ClientController::toggleBulletOcrLoop);
    qApp->installNativeEventFilter(this);
}

QString ClientController::lastServerIp() const
{
    return config_.loadLastIp();
}

QVector<TargetWindowInfo> ClientController::windows() const
{
    return windows_;
}

QVector<ClickTask> ClientController::tasks() const
{
    return tasks_;
}

int ClientController::boundWindowIndex() const
{
    return boundWindowIndex_;
}

int ClientController::selectedTaskIndex() const
{
    return selectedTaskIndex_;
}

bool ClientController::isConnected() const
{
    return connection_.isConnected();
}

bool ClientController::isReconnecting() const
{
    return connection_.isReconnecting();
}

bool ClientController::isReady() const
{
    return ready_;
}

HWND ClientController::boundHwnd() const
{
    return boundHwnd_;
}

int ClientController::clickQueueDelayMs() const
{
    return clickQueueDelayMs_;
}

double ClientController::unitPrice() const
{
    return unitPrice_;
}

int ClientController::bulletTotal() const
{
    return bulletTotal_;
}

bool ClientController::isBulletOcrRunning() const
{
    return bulletOcrRunning_;
}

ClientRole ClientController::clientRole() const
{
    return currentRole_;
}

bool ClientController::initialize(HWND mainWindowHandle)
{
    mainWindowHandle_ = mainWindowHandle;
    reloadSharedClickQueueDelay();
    if (!hotkey_.start(mainWindowHandle_))
    {
        setStatus(QStringLiteral("F3 热键注册失败，请关闭占用该热键的程序后重试"));
    }
    refreshWindows();
    const bool bound = autoBindGameWindow(false);
    return bound;
}

bool ClientController::nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result)
{
    Q_UNUSED(eventType);
    Q_UNUSED(result);

    const MSG* nativeMessage = static_cast<MSG*>(message);
    if (nativeMessage && nativeMessage->message == WM_DF_PLAYCLICK_DELAY_CHANGED)
    {
        reloadSharedClickQueueDelay();
    }
    return false;
}

void ClientController::refreshWindows()
{
    windows_ = binder_.enumerateWindows(mainWindowHandle_);
    syncBoundWindowFromSelectedTask();
    emit windowsChanged();
}

bool ClientController::autoBindGameWindow(bool showMessage)
{
    refreshWindows();
    const int index = binder_.findGameBodyIndex(windows_);
    if (index < 0)
    {
        setStatus(QStringLiteral("未找到三角洲行动游戏本体窗口"));
        if (showMessage)
        {
            emit messageRequested(QStringLiteral("提示"), QStringLiteral("请先运行游戏再启动软件"), false);
        }
        return false;
    }

    addTaskFromWindow(index, false);
    return true;
}

void ClientController::rebindGameWindow()
{
    refreshWindows();
    const int index = binder_.findGameBodyIndex(windows_);
    if (index < 0)
    {
        setStatus(QStringLiteral("重新绑定失败：未找到游戏窗口"));
        return;
    }
    bindWindow(index, true);
    setStatus(QStringLiteral("已重新绑定游戏窗口"));
}

void ClientController::forceStopCurrentAction()
{
    automationCancelRequested_ = true;
    if (accountLoginRunning_ || accountBanMonitoringEnabled_ || accountBanMonitor_.isRunning())
    {
        accountBanMonitoringEnabled_ = false;
        accountBanMonitor_.stop();
        accountLoginSuccessMonitor_.stop();
        accountLoginRunning_ = false;
        accountLoginAutomation_.cancelAndStopProcesses();
    }
    else
        accountLoginAutomation_.cancel();
    countdownTimer_.stop();
    scheduledTriggerTimer_.stop();
    countdown_ = 0;
    if (bulletOcrRunning_)
    {
        finishBulletOcrLoop(false, QStringLiteral("服务端强制停止当前动作"));
    }
    automationActionRunning_ = false;
    setStatus(QStringLiteral("已强制停止当前动作"));
}

void ClientController::bindWindow(int index, bool bringToFront)
{
    if (index < 0 || index >= windows_.size())
    {
        return;
    }

    const TargetWindowInfo& item = windows_[index];
    if (!binder_.isValid(item.hwnd))
    {
        emit messageRequested(QStringLiteral("提示"), QStringLiteral("窗口已失效，请刷新列表"), false);
        return;
    }

    int clientWidth = 0;
    int clientHeight = 0;
    if (!binder_.prepareForBinding(item.hwnd, clientWidth, clientHeight))
    {
        setStatus(QStringLiteral("绑定失败：窗口客户区仍为 0×0"));
        return;
    }

    boundHwnd_ = item.hwnd;
    boundWindowIndex_ = index;
    if (bringToFront)
    {
        SetForegroundWindow(boundHwnd_);
    }

    emit boundWindowChanged(QStringLiteral("已绑定：%1 · %2 · %3×%4")
        .arg(item.title, item.processName)
        .arg(clientWidth)
        .arg(clientHeight));
    emit windowsChanged();
    setStatus(QStringLiteral("已绑定目标窗口"));
}

void ClientController::addTaskFromWindow(int index, bool bringToFront)
{
    if (index < 0 || index >= windows_.size())
    {
        return;
    }

    const TargetWindowInfo& item = windows_[index];
    if (!binder_.isValid(item.hwnd))
    {
        emit messageRequested(QStringLiteral("提示"), QStringLiteral("窗口已失效，请刷新列表"), false);
        return;
    }

    int clientWidth = 0;
    int clientHeight = 0;
    if (!binder_.prepareForBinding(item.hwnd, clientWidth, clientHeight))
    {
        setStatus(QStringLiteral("添加任务失败：窗口客户区仍为 0×0"));
        return;
    }

    ClickTask task;
    task.hwnd = item.hwnd;
    task.title = item.title;
    task.processName = item.processName;
    task.hwndText = item.hwndText;
    task.clientWidth = clientWidth;
    task.clientHeight = clientHeight;
    task.x = clickX_;
    task.y = clickY_;
    POINT point{};
    if (TryGetAutoClickPoint(item.clientWidth, item.clientHeight, point))
    {
        task.x = point.x;
        task.y = point.y;
    }

    tasks_.append(task);
    emit tasksChanged();
    selectTask(tasks_.size() - 1);
    if (bringToFront)
    {
        SetForegroundWindow(item.hwnd);
    }
    setStatus(QStringLiteral("已添加点击任务"));
}

void ClientController::selectTask(int index)
{
    if (index < 0 || index >= tasks_.size())
    {
        selectedTaskIndex_ = -1;
        boundHwnd_ = nullptr;
        boundWindowIndex_ = -1;
        emit selectedTaskChanged(selectedTaskIndex_);
        emit boundWindowChanged(QStringLiteral("未选择点击任务"));
        emit windowsChanged();
        return;
    }

    selectedTaskIndex_ = index;
    syncBoundWindowFromSelectedTask();
    emit selectedTaskChanged(selectedTaskIndex_);
    emit clickPointChanged(clickX_, clickY_);
    emit windowsChanged();
}

void ClientController::removeSelectedTask()
{
    if (selectedTaskIndex_ < 0 || selectedTaskIndex_ >= tasks_.size())
    {
        return;
    }

    tasks_.removeAt(selectedTaskIndex_);
    emit tasksChanged();
    if (tasks_.isEmpty())
    {
        selectTask(-1);
    }
    else
    {
        selectTask(qMin(selectedTaskIndex_, tasks_.size() - 1));
    }
    setStatus(QStringLiteral("已删除点击任务"));
}

void ClientController::moveSelectedTaskUp()
{
    if (selectedTaskIndex_ <= 0 || selectedTaskIndex_ >= tasks_.size())
    {
        return;
    }

    tasks_.move(selectedTaskIndex_, selectedTaskIndex_ - 1);
    --selectedTaskIndex_;
    emit tasksChanged();
    selectTask(selectedTaskIndex_);
    setStatus(QStringLiteral("已上移点击任务"));
}

void ClientController::moveSelectedTaskDown()
{
    if (selectedTaskIndex_ < 0 || selectedTaskIndex_ >= tasks_.size() - 1)
    {
        return;
    }

    tasks_.move(selectedTaskIndex_, selectedTaskIndex_ + 1);
    ++selectedTaskIndex_;
    emit tasksChanged();
    selectTask(selectedTaskIndex_);
    setStatus(QStringLiteral("已下移点击任务"));
}

void ClientController::toggleSelectedTaskEnabled()
{
    if (selectedTaskIndex_ < 0 || selectedTaskIndex_ >= tasks_.size())
    {
        return;
    }

    tasks_[selectedTaskIndex_].enabled = !tasks_[selectedTaskIndex_].enabled;
    emit tasksChanged();
    selectTask(selectedTaskIndex_);
    setStatus(tasks_[selectedTaskIndex_].enabled ? QStringLiteral("已启用点击任务") : QStringLiteral("已停用点击任务"));
}

void ClientController::connectOrDisconnect(const QString& ip, int port)
{
    if (connection_.isConnected() || connection_.isReconnecting())
    {
        connection_.disconnectFromServer();
        return;
    }

    config_.saveLastIp(ip);
    connection_.connectToServer(ip, port);
}

void ClientController::toggleReady()
{
    if (!connection_.isConnected())
    {
        emit messageRequested(QStringLiteral("提示"), QStringLiteral("请先连接服务端"), false);
        return;
    }
    if (!hasEnabledTasks())
    {
        emit messageRequested(QStringLiteral("提示"), QStringLiteral("请先添加并启用至少一个点击任务"), false);
        return;
    }

    setReady(!ready_);
}

void ClientController::autoPickPoint(bool showMessage)
{
    if (!boundHwnd_ || !binder_.isValid(boundHwnd_) || selectedTaskIndex_ < 0 || selectedTaskIndex_ >= tasks_.size())
    {
        if (showMessage)
        {
            emit messageRequested(QStringLiteral("提示"), QStringLiteral("请先选择有效点击任务"), false);
        }
        return;
    }

    int width = 0;
    int height = 0;
    POINT point{};
    if (binder_.clientSize(boundHwnd_, width, height) && TryGetAutoClickPoint(width, height, point))
    {
        setClickPoint(point.x, point.y);
        tasks_[selectedTaskIndex_].clientWidth = width;
        tasks_[selectedTaskIndex_].clientHeight = height;
        emit tasksChanged();
        setStatus(QStringLiteral("已按选中任务窗口分辨率自动取点"));
        return;
    }

    setStatus(QStringLiteral("当前分辨率未配置自动坐标，请用热键取点"));
    if (showMessage)
    {
        emit messageRequested(QStringLiteral("提示"), QStringLiteral("当前选中任务分辨率未配置自动坐标，请用 Ctrl + Shift + P 手动取点"), false);
    }
}

void ClientController::startHotkeyPick()
{
    if (!boundHwnd_ || !binder_.isValid(boundHwnd_))
    {
        emit messageRequested(QStringLiteral("提示"), QStringLiteral("请先选择有效点击任务"), false);
        return;
    }

    if (!hotkey_.isActive() && !hotkey_.start(mainWindowHandle_))
    {
        emit messageRequested(QStringLiteral("错误"), QStringLiteral("注册 Ctrl + Shift + P / F3 热键失败"), true);
        return;
    }
    hotkeyPickPending_ = true;
    setStatus(QStringLiteral("请移动鼠标到点击位置，然后按 Ctrl + Shift + P"));
}

void ClientController::setClickPoint(int x, int y)
{
    clickX_ = x;
    clickY_ = y;
    if (selectedTaskIndex_ >= 0 && selectedTaskIndex_ < tasks_.size())
    {
        tasks_[selectedTaskIndex_].x = x;
        tasks_[selectedTaskIndex_].y = y;
        emit tasksChanged();
    }
    emit clickPointChanged(clickX_, clickY_);
}

void ClientController::setClickQueueDelayMs(int delayMs)
{
    config_.saveClickQueueDelayMs(delayMs);
    reloadSharedClickQueueDelay();
    broadcastClickQueueDelayChanged();
}

bool ClientController::hasEnabledTasks() const
{
    for (const ClickTask& task : tasks_)
    {
        if (task.enabled)
        {
            return true;
        }
    }
    return false;
}

void ClientController::setOcrParameters(double unitPrice, int bulletTotal)
{
    if (!std::isfinite(unitPrice) || unitPrice < 0.0 || bulletTotal < 1)
    {
        setStatus(QStringLiteral("OCR 参数无效：单价不能为负数，子弹总数必须至少为 1"));
        return;
    }

    unitPrice_ = unitPrice;
    bulletTotal_ = bulletTotal;
    config_.saveUnitPrice(unitPrice_);
    config_.saveBulletTotal(bulletTotal_);
    emit ocrParametersChanged(unitPrice_, bulletTotal_);
    setStatus(QStringLiteral("OCR 参数已保存到本机"));
}

void ClientController::toggleBulletOcrLoop()
{
    if (bulletOcrRunning_)
    {
        finishBulletOcrLoop(false, QStringLiteral("用户按 F3 停止 OCR 循环"));
        return;
    }
    if (automationActionRunning_)
    {
        setStatus(QStringLiteral("已有自动化流程正在执行，暂不能启动 F3 OCR"));
        return;
    }
    if (!boundHwnd_ || !binder_.isValid(boundHwnd_))
    {
        setStatus(QStringLiteral("F3 OCR 启动失败：未绑定有效游戏窗口"));
        return;
    }
    if (!std::isfinite(unitPrice_) || unitPrice_ <= 0.0 || bulletTotal_ <= 0)
    {
        setStatus(QStringLiteral("F3 OCR 启动失败：单价和子弹总数必须是正数"));
        return;
    }

    bulletOcrConfig_ = config_.loadBulletOcrConfig();
    if (!bulletOcrConfig_.region.enabled)
    {
        setStatus(QStringLiteral("F3 OCR 启动失败：截图区域配置无效"));
        return;
    }

    // F3 循环独占本机自动化输入，避免与服务端指令同时争用真实鼠标和键盘。
    automationActionRunning_ = true;
    bulletOcrRunning_ = true;
    bulletOcrRoundActive_ = false;
    bulletOcrWaitingForResult_ = false;
    bulletOcrRoundIndex_ = 1;
    emit bulletOcrRunningChanged(true);
    appendBulletOcrLog(QStringLiteral("F3 OCR 循环已启动"));
    setStatus(QStringLiteral("F3 OCR 循环运行中"));
    runBulletOcrRound();
}

void ClientController::runBulletOcrRound()
{
    if (!bulletOcrRunning_ || bulletOcrRoundActive_)
    {
        return;
    }
    if (!boundHwnd_ || !binder_.isValid(boundHwnd_))
    {
        finishBulletOcrLoop(false, QStringLiteral("绑定游戏窗口已失效"));
        return;
    }

    bulletOcrRoundActive_ = true;
    if (!bulletOcrWaitingForResult_)
    {
        appendBulletOcrLog(QStringLiteral("第 %1 轮：开始截图识别").arg(bulletOcrRoundIndex_));
    }

    QString errorText;
    const QString imagePath = captureOcrRegion(bulletOcrConfig_.region, QStringLiteral("bullet_price"), errorText);
    if (imagePath.isEmpty())
    {
        bulletOcrRoundActive_ = false;
        finishBulletOcrLoop(false, errorText);
        return;
    }

    const QString rawText = umiOcrRunner_.recognizeImage(
        imagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), errorText);
    QFile::remove(imagePath);
    bulletOcrRoundActive_ = false;
    if (!bulletOcrRunning_)
    {
        return;
    }

    if (rawText.isEmpty())
    {
        if (!errorText.isEmpty()
            && !errorText.contains(QStringLiteral("No text found"), Qt::CaseInsensitive)
            && !errorText.contains(QStringLiteral("未找到")))
        {
            finishBulletOcrLoop(false, errorText);
            return;
        }
        bulletOcrWaitingForResult_ = true;
        scheduleNextBulletOcrRound();
        return;
    }

    const OcrPriceParseResult parsed = ParseOcrPrice(rawText);
    if (!parsed.ok)
    {
        bulletOcrWaitingForResult_ = true;
        scheduleNextBulletOcrRound();
        return;
    }

    bulletOcrWaitingForResult_ = false;
    appendBulletOcrLog(parsed.debugText);
    const BulletOcrEvaluation evaluation = EvaluateBulletOcr(parsed.price, bulletTotal_, unitPrice_);
    if (!evaluation.valid)
    {
        finishBulletOcrLoop(false, evaluation.errorText);
        return;
    }

    emit bulletOcrResultChanged(rawText, parsed.price, evaluation.averagePrice, evaluation.meetsUnitPrice);
    appendBulletOcrLog(QStringLiteral("计算：%1 / %2 = %3，目标单价 <= %4")
        .arg(parsed.price, 0, 'f', 2)
        .arg(bulletTotal_)
        .arg(evaluation.averagePrice, 0, 'f', 4)
        .arg(unitPrice_, 0, 'f', 4));
    if (evaluation.meetsUnitPrice)
    {
        finishBulletOcrLoop(true, QStringLiteral("识别价格满足条件，OCR 循环停止"));
        return;
    }

    const int jitterX = QRandomGenerator::global()->bounded(-3, 4);
    const int jitterY = QRandomGenerator::global()->bounded(-3, 4);
    POINT retryPoint{ bulletOcrConfig_.retryX + jitterX, bulletOcrConfig_.retryY + jitterY };
    if (!input_.clickReferencePoint(boundHwnd_, retryPoint,
            bulletOcrConfig_.retryReferenceWidth, bulletOcrConfig_.retryReferenceHeight, 0, errorText))
    {
        finishBulletOcrLoop(false, errorText);
        return;
    }
    appendBulletOcrLog(QStringLiteral("价格高于设定值：已点击重试，等待 %1 ms 后发送 ESC")
        .arg(bulletOcrConfig_.escDelayMs));
    bulletOcrActionTimer_.start(bulletOcrConfig_.escDelayMs);
}

void ClientController::scheduleNextBulletOcrRound(bool completedDecision)
{
    if (!bulletOcrRunning_)
    {
        return;
    }

    // OCR 未识别时只重试当前轮；只有有效数字完成判断后才推进逻辑轮次。
    if (completedDecision)
    {
        bulletOcrWaitingForResult_ = false;
        ++bulletOcrRoundIndex_;
    }
    bulletOcrNextRoundTimer_.start(bulletOcrConfig_.nextRoundDelayMs);
}

void ClientController::finishBulletOcrLoop(bool success, const QString& reason)
{
    if (!bulletOcrRunning_)
    {
        return;
    }

    bulletOcrActionTimer_.stop();
    bulletOcrNextRoundTimer_.stop();
    bulletOcrRunning_ = false;
    bulletOcrRoundActive_ = false;
    bulletOcrWaitingForResult_ = false;
    automationActionRunning_ = false;
    appendBulletOcrLog(QStringLiteral("F3 OCR：%1").arg(reason));
    emit bulletOcrRunningChanged(false);
    setStatus(success ? QStringLiteral("OCR 成功：%1").arg(reason) : reason);
}

void ClientController::appendBulletOcrLog(const QString& text)
{
    emit bulletOcrLogAppended(text);
}
void ClientController::setReady(bool ready)
{
    if (ready_ == ready)
    {
        return;
    }

    if (!connection_.sendReady(ready))
    {
        emit messageRequested(QStringLiteral("提示"), QStringLiteral("连接已失效，请重新连接服务端"), false);
        return;
    }

    ready_ = ready;
    emit readyChanged(ready_);
    setStatus(ready_ ? QStringLiteral("已准备") : QStringLiteral("未准备"));
}

void ClientController::clearLocalReadyState()
{
    if (!ready_)
    {
        return;
    }

    // 连接已经失效时只清理本地状态，避免再通过坏 socket 发送 READY 0。
    ready_ = false;
    emit readyChanged(ready_);
    setStatus(QStringLiteral("未准备"));
}

void ClientController::setStatus(const QString& text)
{
    emit statusChanged(text);
}

void ClientController::startCountdown(int seconds, const QString& bossPrice)
{
    // 服务端同一轮可能因自动状态切换重复投递 TRIGGER；客户端已有动作或倒计时
    // 时直接丢弃重复指令，不能重置倒计时并重新进入整套流程。
    if (automationActionRunning_ || countdownTimer_.isActive() || scheduledTriggerTimer_.isActive())
    {
        emit connectionLogAppended(QStringLiteral("忽略重复 TRIGGER：当前已有自动化流程或倒计时正在执行"));
        return;
    }
    // 新一轮服务端指令开始时清除上一轮强制停止状态。
    automationCancelRequested_.store(false);
    scheduledTriggerTimer_.stop();
    countdownTimer_.stop();
    countdown_ = seconds;
    triggerBossPrice_ = bossPrice.trimmed();
    if (ShouldClickImmediatelyForCountdown(seconds))
    {
        emit countdownChanged(QStringLiteral("倒计时：点击"));
        clickEnabledTasks();
        return;
    }

    emit countdownChanged(QStringLiteral("倒计时：%1").arg(countdown_));
    countdownTimer_.start();
}

void ClientController::startScheduledTrigger(int seconds, qint64 targetClientMs, const QString& bossPrice)
{
    if (automationActionRunning_ || countdownTimer_.isActive() || scheduledTriggerTimer_.isActive())
    {
        emit connectionLogAppended(QStringLiteral("忽略重复定时 TRIGGER：当前已有自动化流程或倒计时正在执行"));
        return;
    }
    // 新一轮定时指令开始时清除上一轮强制停止状态，否则到点会被直接跳过。
    automationCancelRequested_.store(false);
    countdownTimer_.stop();
    scheduledTriggerTimer_.stop();
    scheduledTriggerSeconds_ = seconds;
    scheduledTriggerTargetMs_ = targetClientMs;
    triggerBossPrice_ = bossPrice.trimmed();
    const qint64 remainingMs = targetClientMs - QDateTime::currentMSecsSinceEpoch();
    if (remainingMs <= 0)
    {
        setStatus(QStringLiteral("定时出发目标已到期，立即执行"));
        fireScheduledTrigger();
        return;
    }
    setStatus(QStringLiteral("定时出发：目标时间 %1").arg(
        QDateTime::fromMSecsSinceEpoch(targetClientMs).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))));
    scheduledTriggerTimer_.start(static_cast<int>(qMin<qint64>(remainingMs, INT_MAX)));
}

void ClientController::fireScheduledTrigger()
{
    const qint64 remainingMs = scheduledTriggerTargetMs_ - QDateTime::currentMSecsSinceEpoch();
    if (remainingMs > 2)
    {
        scheduledTriggerTimer_.start(static_cast<int>(qMin<qint64>(remainingMs, INT_MAX)));
        return;
    }
    if (automationCancelRequested_.load())
    {
        return;
    }
    emit countdownChanged(QStringLiteral("定时出发：点击"));
    clickEnabledTasks();
}

void ClientController::updateServerTimeDisplay()
{
    if (!serverTimeSynchronized_)
    {
        emit serverTimeChanged(QStringLiteral("未同步"));
        return;
    }
    // 服务端计算的偏移定义为“客户端时间 - 服务器时间”，
    // 因此从客户端本机时间换算服务器时间时必须减去该偏移。
    const qint64 serverNowMs = QDateTime::currentMSecsSinceEpoch() - serverClockOffsetMs_;
    emit serverTimeChanged(QDateTime::fromMSecsSinceEpoch(serverNowMs).toString(
        QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")));
}

void ClientController::tickCountdown()
{
    --countdown_;
    if (ShouldClickImmediatelyForCountdown(countdown_))
    {
        countdownTimer_.stop();
        emit countdownChanged(QStringLiteral("倒计时：点击"));
        clickEnabledTasks();
        return;
    }

    emit countdownChanged(QStringLiteral("倒计时：%1").arg(countdown_));
}

void ClientController::clickEnabledTasks()
{
    if (!boundHwnd_ || !binder_.isValid(boundHwnd_))
    {
        emit messageRequested(QStringLiteral("提示"), QStringLiteral("绑定游戏窗口已失效，请重启客户端后重试"), false);
        return;
    }

    const QPoint referencePoint = DispatchReferencePoint(currentRole_);
    POINT point{ referencePoint.x(), referencePoint.y() };
    QString errorText;
    const QString actionName = currentRole_ == ClientRole::Boss
        ? QStringLiteral("老板上架") : QStringLiteral("领取交易收益");
    // 先占用自动化锁，再执行首个固定点击，避免重复 TRIGGER 在入口处重入。
    if (!beginAutomationAction(actionName))
    {
        setStatus(QStringLiteral("%1点击完成，已有自动化流程正在执行").arg(actionName));
        return;
    }
    struct ActionGuard
    {
        ClientController* controller = nullptr;
        ~ActionGuard() { if (controller) controller->finishAutomationAction(); }
    } guard{ this };

    const auto sendAutomaticListingResponse = [this](bool success, const QString& text) {
        if (!hasLastListingRequest_ || lastListingRequestId_ <= 0)
        {
            emit connectionLogAppended(QStringLiteral("自动上架回报跳过：没有可关联的上架请求，结果=%1，%2")
                .arg(success ? QStringLiteral("成功") : QStringLiteral("失败"), text));
            return;
        }
        const bool sent = connection_.sendListingResponse(lastListingRequestId_, success, text);
        emit connectionLogAppended(QStringLiteral("自动上架回报：requestId=%1，结果=%2，发送=%3，%4")
            .arg(lastListingRequestId_)
            .arg(success ? QStringLiteral("成功") : QStringLiteral("失败"))
            .arg(sent ? QStringLiteral("成功") : QStringLiteral("失败"), text));
    };
    const auto reportStableAmount = [this, &errorText](POINT movePoint, int moveReferenceWidth, int moveReferenceHeight,
        int x, int y, int width, int height, const QString& prefix, bool reportToServer) {
        QRegularExpression amountPattern(QStringLiteral(R"((\d[\d,.]*\d|\d)[\s]*([KkMm])?)"));
        // 资金面板可能因鼠标已在附近而没有重新弹出，最多向右偏移 3、6 像素重试。
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            QString previous;
            int stableCount = 0;
            QElapsedTimer timer;
            POINT retryPoint = movePoint;
            retryPoint.x += attempt * 3;
            if (!input_.moveReferencePoint(boundHwnd_, retryPoint, moveReferenceWidth, moveReferenceHeight, errorText))
                return false;
            if (!waitForAutomation(1000, errorText))
                return false;
            timer.start();
            // 每次重新移动后最多识别 5 秒；失败才进入下一偏移点。
            while (timer.elapsed() < 5000)
            {
            OcrRegionConfig region;
            region.enabled = true;
            region.x = x; region.y = y; region.width = width; region.height = height;
            region.referenceWidth = 2560; region.referenceHeight = 1440;
            QString ocrError;
            const QString imagePath = captureOcrRegion(region, prefix, ocrError);
            QString text;
            if (!imagePath.isEmpty())
            {
                text = umiOcrRunner_.recognizeImage(
                    imagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), ocrError).simplified();
                QFile::remove(imagePath);
            }
            emit connectionLogAppended(QStringLiteral("金额OCR[%1]：原始文本“%2”").arg(prefix, text));
            const QString normalizedOcrText = NormalizeAmountOcrText(text);
            QRegularExpressionMatch match;
            double largestValue = -1.0;
            auto iterator = amountPattern.globalMatch(normalizedOcrText);
            while (iterator.hasNext())
            {
                const auto candidate = iterator.next();
                const QString candidateValue = NormalizeAmountNumericToken(candidate.captured(1));
                bool candidateOk = false;
                const double numericValue = candidateValue.toDouble(&candidateOk);
                if (!candidateOk)
                    continue;
                if (candidate.captured(2).isEmpty() && candidateValue.size() <= 1)
                    continue;
                if (!match.hasMatch()
                    || (!candidate.captured(2).isEmpty() && match.captured(2).isEmpty())
                    || (candidate.captured(2).isEmpty() == match.captured(2).isEmpty()
                        && numericValue > largestValue))
                {
                    match = candidate;
                    largestValue = numericValue;
                }
            }
            if (match.hasMatch())
            {
                const QString normalized = NormalizeAmountNumericToken(match.captured(1));
                bool ok = false;
                double value = normalized.toDouble(&ok);
                if (ok)
                {
                    const QString suffix = match.captured(2).toUpper();
                    if (suffix == QStringLiteral("K")) value *= 1000.0;
                    else if (suffix == QStringLiteral("M")) value *= 1000000.0;
                    const QString sample = QString::number(static_cast<qint64>(value));
                    stableCount = sample == previous ? stableCount + 1 : 1;
                    previous = sample;
                    emit connectionLogAppended(QStringLiteral("金额OCR[%1]：识别到 %2，连续稳定 %3/3")
                        .arg(prefix, sample).arg(stableCount));
                    if (stableCount >= 3)
                    {
                        if (connection_.sendAmountReport(sample.toLongLong()))
                        {
                            emit connectionLogAppended(QStringLiteral("金额OCR[%1]：已回报确认金额 %2（%3）")
                                .arg(prefix, sample, reportToServer ? QStringLiteral("老板") : QStringLiteral("币号")));
                            return true;
                        }
                        emit connectionLogAppended(QStringLiteral("金额OCR[%1]：金额 %2 回报服务端失败")
                            .arg(prefix, sample));
                        return false;
                    }
                }
            }
            else
            {
                emit connectionLogAppended(QStringLiteral("金额OCR[%1]：未识别到金额，OCR文本“%2”错误“%3”")
                    .arg(prefix, text, ocrError));
            }
                if (!waitForAutomation(100, errorText)) return false;
            }
        }
        if (errorText.isEmpty())
            errorText = QStringLiteral("资金面板在原坐标及右移 3/6 像素后仍未识别到稳定金额");
        return false;
    };
    // 出发点不接受用户取点，只按角色使用 2560x1440 固定坐标并适配当前客户区。
    if (!input_.clickReferencePoint(boundHwnd_, point, 2560, 1440, clickQueueDelayMs_, errorText))
    {
        setStatus(errorText);
        emit messageRequested(QStringLiteral("提示"), errorText, false);
        return;
    }

    const QString roleText = QString::fromStdWString(ClientRoleUtil::ToDisplayText(currentRole_));
    setStatus(QStringLiteral("%1固定坐标点击完成").arg(roleText));

    if (currentRole_ == ClientRole::Boss)
    {
        if (!executeBossListingSequence(triggerBossPrice_, errorText))
        {
            // 执行失败时主动清除旧的老板准备状态，避免服务端继续使用上一轮状态。
            connection_.sendTeamReady(0, 0, 1);
            setStatus(QStringLiteral("老板上架流程失败：%1").arg(errorText));
            return;
        }
        setStatus(QStringLiteral("老板执行完成，已提交上架"));
        return;
    }

    bool noRevenue = false;
    if (!executeCoinRevenueClaimSequence(noRevenue, errorText))
    {
        sendAutomaticListingResponse(false, QStringLiteral("币号交易收益领取失败：%1").arg(errorText));
        setStatus(QStringLiteral("币号交易收益领取失败：%1").arg(errorText));
        return;
    }
    // 币号资金区域包含货币名称和说明文字，金额解析器只提取数字及分隔符，忽略中文。
    int coinClientWidth = 0;
    int coinClientHeight = 0;
    if (!binder_.clientSize(boundHwnd_, coinClientWidth, coinClientHeight)
        || coinClientWidth <= 0 || coinClientHeight <= 0)
    {
        sendAutomaticListingResponse(false, QStringLiteral("币号资金识别失败：无法获取游戏窗口客户区尺寸"));
        setStatus(QStringLiteral("币号金额识别失败：无法获取游戏窗口客户区尺寸"));
        return;
    }
    const bool isCoinWindow1K = coinClientWidth <= 1920 && coinClientHeight <= 1080;
    const POINT coinAmountMovePoint = isCoinWindow1K ? POINT{ 1541, 49 } : POINT{ 1985, 68 };
    const int coinMoveReferenceWidth = isCoinWindow1K ? 1920 : 2560;
    const int coinMoveReferenceHeight = isCoinWindow1K ? 1080 : 1440;
    emit connectionLogAppended(QStringLiteral("币号资金识别：窗口客户区 %1x%2，使用鼠标坐标 (%3,%4)" )
        .arg(coinClientWidth).arg(coinClientHeight)
        .arg(coinAmountMovePoint.x).arg(coinAmountMovePoint.y));
    if (!reportStableAmount(coinAmountMovePoint, coinMoveReferenceWidth, coinMoveReferenceHeight,
        1887, 320, 450, 95,
        QStringLiteral("coin_amount"), false))
    {
        sendAutomaticListingResponse(false, QStringLiteral("币号金额识别失败：%1").arg(errorText));
        setStatus(QStringLiteral("币号金额识别失败：%1").arg(errorText));
        return;
    }
    // 执行完成后复用同一套上架函数，形成“执行 -> 上架 -> 回报”的闭环。
    // 上架配置由最近一次服务端下发保存；不再依赖新的服务端按钮事件。
    if (hasLastListingRequest_ && lastListingPurchaseCount_ > 0
        && !lastListingPrice_.trimmed().isEmpty())
    {
        QString listingOcrText;
        QString listingUsageResult;
        if (!executeListingSequence(lastListingHasExchange_, lastListingPurchaseCount_, lastListingLoadoutMode_,
            lastListingPrice_, lastListingText_, listingOcrText, listingUsageResult, errorText))
        {
            sendAutomaticListingResponse(false, QStringLiteral("自动上架失败：%1").arg(errorText));
            setStatus(QStringLiteral("币号执行完成，但自动上架失败：%1").arg(errorText));
            return;
        }
        // 自动重新上架仍必须回报 ListingResponse，服务端据此清除“执行中”并显示成功。
        const bool usageOk = lastListingLoadoutMode_
            || listingUsageResult.startsWith(QStringLiteral("使用量核对成功"));
        const QString responseText = lastListingLoadoutMode_
            ? QStringLiteral("配装上架流程完成")
            : (usageOk
                ? QStringLiteral("OCR识别到上架，%1").arg(listingUsageResult)
                : listingUsageResult);
        sendAutomaticListingResponse(usageOk, responseText);
        setStatus(QStringLiteral("币号执行完成，已自动重新上架"));
        return;
    }
    sendAutomaticListingResponse(false, QStringLiteral("币号执行完成，但没有可复用的上架配置"));
    setStatus(noRevenue
        ? QStringLiteral("币号执行完成，当前没有交易收益")
        : QStringLiteral("币号执行完成，交易收益已领取"));
}

void ClientController::reloadSharedClickQueueDelay()
{
    const int delayMs = config_.loadClickQueueDelayMs();
    if (clickQueueDelayMs_ == delayMs)
    {
        return;
    }

    clickQueueDelayMs_ = delayMs;
    emit clickQueueDelayChanged(clickQueueDelayMs_);
}

void ClientController::broadcastClickQueueDelayChanged()
{
    PostMessageW(HWND_BROADCAST, WM_DF_PLAYCLICK_DELAY_CHANGED, 0, 0);
}

void ClientController::finishHotkeyPick()
{
    if (!hotkeyPickPending_)
    {
        return;
    }
    hotkeyPickPending_ = false;
    const QPoint cursorPos = QCursor::pos();
    POINT point{ cursorPos.x(), cursorPos.y() };
    if (!binder_.screenToClientPoint(boundHwnd_, point) || !binder_.pointInsideClient(boundHwnd_, point))
    {
        emit messageRequested(QStringLiteral("提示"), QStringLiteral("鼠标位置不在选中任务窗口客户区内"), false);
        setStatus(QStringLiteral("热键取点失败"));
        return;
    }

    setClickPoint(point.x, point.y);
    setStatus(QStringLiteral("已通过热键取点"));
}

void ClientController::syncBoundWindowFromSelectedTask()
{
    boundWindowIndex_ = -1;
    if (selectedTaskIndex_ < 0 || selectedTaskIndex_ >= tasks_.size())
    {
        boundHwnd_ = nullptr;
        return;
    }

    const ClickTask& task = tasks_[selectedTaskIndex_];
    boundHwnd_ = task.hwnd;
    clickX_ = task.x;
    clickY_ = task.y;
    for (int index = 0; index < windows_.size(); ++index)
    {
        if (windows_[index].hwnd == boundHwnd_)
        {
            boundWindowIndex_ = index;
            break;
        }
    }

    emit boundWindowChanged(QStringLiteral("当前任务：%1 · %2 · %3×%4")
        .arg(task.title, task.processName)
        .arg(task.clientWidth)
        .arg(task.clientHeight));
}
void ClientController::handleTeamCodeRequest()
{
    if (!beginAutomationAction(QStringLiteral("获取组队码")))
    {
        sendTeamCodeResult(QStringLiteral("ERROR"), QStringLiteral("已有自动化流程正在执行"));
        return;
    }

    const auto finish = [this]() {
        finishAutomationAction();
    };

    QString errorText;
    if (!boundHwnd_ || !binder_.isValid(boundHwnd_))
    {
        sendTeamCodeResult(QStringLiteral("ERROR"), QStringLiteral("获取组队码失败：未绑定有效窗口"));
        finish();
        return;
    }

    SetForegroundWindow(boundHwnd_);
    constexpr int ReferenceWidth = 2560;
    constexpr int ReferenceHeight = 1440;
    const POINT first{ 2345, 1393 };
    const POINT second{ 458, 776 };
    const POINT third{ 1606, 820 };

    // 组队码流程的坐标按 2560x1440 客户区比例缩放，避免不同窗口尺寸下偏移。
    if (!input_.clickReferencePoint(boundHwnd_, first, ReferenceWidth, ReferenceHeight, 1000, errorText)
        || !input_.clickReferencePoint(boundHwnd_, second, ReferenceWidth, ReferenceHeight, 2000, errorText)
        || !input_.clickReferencePoint(boundHwnd_, third, ReferenceWidth, ReferenceHeight, 1000, errorText)
        || !input_.pressKey(VK_ESCAPE, errorText))
    {
        sendTeamCodeResult(QStringLiteral("ERROR"), QStringLiteral("获取组队码失败：%1").arg(errorText));
        finish();
        return;
    }

    QThread::msleep(100);
    if (!input_.pressKey(VK_ESCAPE, errorText))
    {
        sendTeamCodeResult(QStringLiteral("ERROR"), QStringLiteral("获取组队码失败：%1").arg(errorText));
        finish();
        return;
    }

    QThread::msleep(80);
    const QString clipboardText = input_.readClipboardText(errorText).trimmed();
    const std::string payload = clipboardText.toStdString();
    if (Protocol::IsValidTeamCodePayload(payload))
    {
        sendTeamCodeResult(QString::fromStdString(payload), QStringLiteral("已返回组队码"));
    }
    else
    {
        sendTeamCodeResult(QStringLiteral("INVALID"), QStringLiteral("剪切板组队码无效"));
    }
    finish();
}

bool ClientController::beginAutomationAction(const QString& actionName)
{
    if (automationActionRunning_)
    {
        return false;
    }
    automationCancelRequested_ = false;
    automationActionRunning_ = true;
    setStatus(QStringLiteral("正在执行：%1").arg(actionName));
    return true;
}

void ClientController::finishAutomationAction()
{
    automationActionRunning_ = false;
}

void ClientController::sendTeamCodeResult(const QString& payload, const QString& statusText)
{
    connection_.sendTeamCodeResponse(payload);
    setStatus(statusText);
}

void ClientController::handleTeamReadyRequest()
{
    if (!beginAutomationAction(QStringLiteral("识别队伍状态")))
    {
        sendTeamReadyResult(TeamReadyResult{});
        return;
    }

    struct ActionGuard
    {
        ClientController* controller = nullptr;
        ~ActionGuard()
        {
            if (controller)
            {
                controller->finishAutomationAction();
                controller->teamReadyOcrRunning_ = false;
            }
        }
    } guard{ this };

    if (teamReadyOcrRunning_ || !connection_.isConnected())
    {
        sendTeamReadyResult(TeamReadyResult{});
        return;
    }

    teamReadyOcrRunning_ = true;
    TeamReadyResult result;
    QString errorText;
    const OcrRegionConfig region = config_.loadTeamReadyRegion();
    if (!region.enabled)
    {
        sendTeamReadyResult(result);
        setStatus(QStringLiteral("队伍状态OCR区域未配置"));
        return;
    }

    // 页面切换后就绪数字可能延迟出现，持续重新截图和 OCR，最多等待 5 秒。
    constexpr int ReadyTimeoutMs = 5000;
    constexpr int ReadyPollIntervalMs = 150;
    QElapsedTimer readyTimer;
    readyTimer.start();
    while (readyTimer.elapsed() < ReadyTimeoutMs)
    {
        QString attemptError;
        const QString imagePath = captureOcrRegion(region, QStringLiteral("team_ready"), attemptError);
        if (!imagePath.isEmpty())
        {
            const QString ocrText = umiOcrRunner_.recognizeImage(
                imagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), attemptError);
            QFile::remove(imagePath);
            if (!ocrText.isEmpty())
            {
                const TeamReadyResult attempt = teamReadyParser_.parse(ocrText);
                if (attempt.state >= 0)
                {
                    result = attempt;
                    errorText.clear();
                    break;
                }
            }
        }
        if (!attemptError.isEmpty())
            errorText = attemptError;
        QThread::msleep(ReadyPollIntervalMs);
    }

    if (result.state < 0)
    {
        if (!errorText.isEmpty())
        {
            setStatus(QStringLiteral("队伍状态OCR失败：%1").arg(errorText));
        }
        else
        {
            setStatus(QStringLiteral("队伍状态OCR未识别到有效结果"));
        }
    }
    else
    {
        setStatus(QStringLiteral("已返回队伍准备状态：%1").arg(result.displayText));
    }
    sendTeamReadyResult(result);
}

void ClientController::handlePrepareRequest(int requestId)
{
    if (currentRole_ != ClientRole::Boss)
    {
        connection_.sendPrepareResponse(requestId, QStringLiteral("ERROR"), QStringLiteral("执行前准备请求仅适用于老板客户端"));
        return;
    }
    if (!beginAutomationAction(QStringLiteral("执行前准备检查")))
    {
        connection_.sendPrepareResponse(requestId, QStringLiteral("ERROR"), QStringLiteral("已有自动化流程正在执行"));
        return;
    }
    struct Guard { ClientController* c; ~Guard() { c->finishAutomationAction(); } } guard{ this };
    QString errorText;
    const bool ok = executeBossPrepareCheck(requestId, errorText);
    if (!ok)
    {
        connection_.sendPrepareResponse(requestId, QStringLiteral("ERROR"), errorText.isEmpty()
            ? QStringLiteral("老板准备检查失败") : errorText);
        setStatus(QStringLiteral("老板执行前准备失败：%1").arg(errorText));
    }
    else
    {
        connection_.sendPrepareResponse(requestId, QStringLiteral("OK"), QStringLiteral("准备数字和金额基线已确认"));
        setStatus(QStringLiteral("老板执行前准备成功"));
    }
}

bool ClientController::executeBossPrepareCheck(int requestId, QString& errorText)
{
    Q_UNUSED(requestId);
    if (!boundHwnd_ || !binder_.isValid(boundHwnd_))
    {
        errorText = QStringLiteral("未绑定有效游戏窗口");
        return false;
    }
    constexpr int ReferenceWidth = 2560;
    constexpr int ReferenceHeight = 1440;
    const auto ocrContainsDigit = [this, &errorText](int x, int y, int width, int height, const QString& prefix) {
        QElapsedTimer timer; timer.start();
        while (timer.elapsed() < 5000)
        {
            if (automationCancelRequested_.load()) { errorText = QStringLiteral("用户或服务端已强制停止当前动作"); return false; }
            OcrRegionConfig region; region.enabled = true; region.x = x; region.y = y;
            region.width = width; region.height = height; region.referenceWidth = 2560; region.referenceHeight = 1440;
            QString ocrError;
            const QString image = captureOcrRegion(region, prefix, ocrError);
            QString text;
            if (!image.isEmpty()) { text = umiOcrRunner_.recognizeImage(image, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), ocrError); QFile::remove(image); }
            if (text.contains(QRegularExpression(QStringLiteral("\\d")))) return true;
            if (!ocrError.isEmpty() && !ocrError.contains(QStringLiteral("No text found in image"), Qt::CaseInsensitive)) errorText = ocrError;
            if (!waitForAutomation(150, errorText)) return false;
        }
        errorText = QStringLiteral("5 秒内未识别到老板准备数字");
        return false;
    };

    // 价格提示出现时，先回报本轮失败，再 ESC，等待 1500ms 后重新检查。
    QElapsedTimer promptTimer; promptTimer.start();
    while (promptTimer.elapsed() < 5000)
    {
        OcrRegionConfig region; region.enabled = true; region.x = 538; region.y = 313; region.width = 275; region.height = 60;
        region.referenceWidth = ReferenceWidth; region.referenceHeight = ReferenceHeight;
        QString ocrError; const QString image = captureOcrRegion(region, QStringLiteral("prepare_prompt"), ocrError);
        QString text;
        if (!image.isEmpty()) { text = umiOcrRunner_.recognizeImage(image, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), ocrError); QFile::remove(image); }
        if (text.contains(QStringLiteral("价格变动提醒"), Qt::CaseInsensitive)
            || text.contains(QStringLiteral("确认实现方案"), Qt::CaseInsensitive))
        {
            connection_.sendPrepareResponse(requestId, QStringLiteral("RETRY"), QStringLiteral("检测到价格提示，正在 ESC 后重试"));
            if (!input_.pressKey(VK_ESCAPE, errorText) || !waitForAutomation(1500, errorText)) return false;
            promptTimer.restart();
            continue;
        }
        if (!ocrError.isEmpty() && !ocrError.contains(QStringLiteral("No text found in image"), Qt::CaseInsensitive)) { errorText = ocrError; return false; }
        break;
    }
    if (!ocrContainsDigit(2048, 1123, 400, 80, QStringLiteral("prepare_ready"))) return false;

    int bossClientWidth = 0;
    int bossClientHeight = 0;
    if (!binder_.clientSize(boundHwnd_, bossClientWidth, bossClientHeight)
        || bossClientWidth <= 0 || bossClientHeight <= 0)
    {
        errorText = QStringLiteral("读取老板游戏窗口客户区失败");
        return false;
    }
    const bool bossWindow1K = bossClientWidth <= 1920 && bossClientHeight <= 1080;
    const POINT bossAmountMovePoint = bossWindow1K ? POINT{ 1639, 54 } : POINT{ 2128, 67 };
    const int bossReferenceWidth = bossWindow1K ? 1920 : ReferenceWidth;
    const int bossReferenceHeight = bossWindow1K ? 1080 : ReferenceHeight;
    QRegularExpression amountPattern(QStringLiteral(R"((\d[\d,.]*\d|\d)[\s]*([KkMm])?)"));
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        QString previous; int stable = 0; QElapsedTimer amountTimer;
        POINT retryPoint = bossAmountMovePoint;
        retryPoint.x += attempt * 3;
        if (!input_.moveReferencePoint(boundHwnd_, retryPoint,
            bossReferenceWidth, bossReferenceHeight, errorText)
            || !waitForAutomation(1000, errorText)) return false;
        amountTimer.start();
        while (amountTimer.elapsed() < 5000)
        {
        OcrRegionConfig region; region.enabled = true; region.x = 2015; region.y = 325; region.width = 450; region.height = 95;
        region.referenceWidth = ReferenceWidth; region.referenceHeight = ReferenceHeight;
        QString ocrError; const QString image = captureOcrRegion(region, QStringLiteral("prepare_amount"), ocrError);
        QString text;
        if (!image.isEmpty()) { text = umiOcrRunner_.recognizeImage(image, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), ocrError); QFile::remove(image); }
        auto matches = amountPattern.globalMatch(NormalizeAmountOcrText(text));
        QString sample;
        while (matches.hasNext()) { const auto m = matches.next(); QString n = NormalizeAmountNumericToken(m.captured(1)); if (n.size() > 1 || !m.captured(2).isEmpty()) { bool ok=false; double v=n.toDouble(&ok); if (ok) { const QString s=m.captured(2).toUpper(); if(s==QStringLiteral("K"))v*=1000; if(s==QStringLiteral("M"))v*=1000000; sample=QString::number(static_cast<qint64>(v)); } } }
        if (!sample.isEmpty()) { stable = sample == previous ? stable + 1 : 1; previous = sample; if (stable >= 3) { if (!connection_.sendAmountReport(sample.toLongLong())) { errorText = QStringLiteral("老板金额基线回报失败"); return false; } return true; } }
        if (!ocrError.isEmpty() && !ocrError.contains(QStringLiteral("No text found in image"), Qt::CaseInsensitive)) errorText = ocrError;
            if (!waitForAutomation(100, errorText)) return false;
        }
    }
    errorText = QStringLiteral("5 秒内未识别到老板金额基线（已重试右移 3/6 像素）");
    return false;
}

void ClientController::handleOcrRequest(int requestId, int x, int y, int width, int height, int referenceWidth, int referenceHeight)
{
    if (!beginAutomationAction(QStringLiteral("OCR识别")))
    {
        connection_.sendOcrResponse(requestId, false, QStringLiteral("已有自动化流程正在执行"));
        return;
    }

    struct ActionGuard
    {
        ClientController* controller = nullptr;
        ~ActionGuard()
        {
            if (controller)
            {
                controller->finishAutomationAction();
            }
        }
    } guard{ this };

    OcrRegionConfig region;
    region.enabled = true;
    region.x = x;
    region.y = y;
    region.width = width;
    region.height = height;
    region.referenceWidth = referenceWidth;
    region.referenceHeight = referenceHeight;

    QString errorText;
    const QString imagePath = captureOcrRegion(region, QStringLiteral("generic"), errorText);
    if (imagePath.isEmpty())
    {
        connection_.sendOcrResponse(requestId, false, errorText);
        setStatus(QStringLiteral("OCR截图失败：%1").arg(errorText));
        return;
    }

    const QString ocrText = umiOcrRunner_.recognizeImage(imagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), errorText);
    QFile::remove(imagePath);
    if (ocrText.isEmpty() && !errorText.isEmpty())
    {
        connection_.sendOcrResponse(requestId, false, errorText);
        setStatus(QStringLiteral("OCR识别失败：%1").arg(errorText));
        return;
    }

    connection_.sendOcrResponse(requestId, true, ocrText);
    setStatus(QStringLiteral("已返回OCR识别结果"));
}

void ClientController::handleLoadoutRequest(int requestId, const QString& loadoutType)
{
    if (!beginAutomationAction(QStringLiteral("配装")))
    {
        connection_.sendLoadoutResponse(requestId, false, QStringLiteral("已有自动化流程正在执行"));
        return;
    }

    struct ActionGuard
    {
        ClientController* controller = nullptr;
        ~ActionGuard()
        {
            if (controller)
            {
                controller->finishAutomationAction();
            }
        }
    } guard{ this };

    QString errorText;
    if (!executeLoadoutSequence(loadoutType, errorText))
    {
        connection_.sendLoadoutResponse(requestId, false, errorText);
        setStatus(QStringLiteral("配装失败：%1").arg(errorText));
        return;
    }

    connection_.sendLoadoutResponse(requestId, true, QStringLiteral("配装完成"));
    setStatus(QStringLiteral("配装流程已完成"));
}

void ClientController::handleSaveLoadoutRequest(int requestId, const QString& weapon, const QString& backpack,
    const QString& egg, const QString& bulletName, int perClick, int bulletTotal)
{
    if (!beginAutomationAction(QStringLiteral("保存配装")))
    {
        connection_.sendSaveLoadoutResponse(requestId, false, QStringLiteral("已有自动化流程正在执行"));
        return;
    }

    struct ActionGuard
    {
        ClientController* controller = nullptr;
        ~ActionGuard()
        {
            if (controller)
            {
                controller->finishAutomationAction();
            }
        }
    } guard{ this };

    QString errorText;
    if (!executeSaveLoadoutSequence(weapon, backpack, egg, bulletName, perClick, bulletTotal, errorText))
    {
        connection_.sendSaveLoadoutResponse(requestId, false, errorText);
        setStatus(QStringLiteral("保存配装失败：%1").arg(errorText));
        return;
    }
    connection_.sendSaveLoadoutResponse(requestId, true, QStringLiteral("保存配装完成"));
    setStatus(QStringLiteral("保存配装流程已完成"));
}

void ClientController::handleListingRequest(int requestId, bool hasExchange, int purchaseCount, bool loadoutMode, const QString& listingPrice, const QString& listingText)
{
    lastListingPrice_ = listingPrice;
    lastListingText_ = listingText;
    lastListingHasExchange_ = hasExchange;
    lastListingPurchaseCount_ = purchaseCount;
    lastListingLoadoutMode_ = loadoutMode;
    lastListingRequestId_ = requestId;
    hasLastListingRequest_ = true;
    if (currentRole_ != ClientRole::Coin)
    {
        connection_.sendListingResponse(requestId, false, QStringLiteral("老板客户端不执行上架"));
        setStatus(QStringLiteral("已拒绝上架任务：当前客户端为老板"));
        return;
    }

    if (!beginAutomationAction(QStringLiteral("上架")))
    {
        connection_.sendListingResponse(requestId, false, QStringLiteral("已有自动化流程正在执行"));
        return;
    }

    struct ActionGuard
    {
        ClientController* controller = nullptr;
        ~ActionGuard()
        {
            if (controller)
            {
                controller->finishAutomationAction();
            }
        }
    } guard{ this };

    QString ocrText;
    QString usageResult;
    QString errorText;
    if (!executeListingSequence(hasExchange, purchaseCount, loadoutMode, listingPrice, listingText, ocrText, usageResult, errorText))
    {
        connection_.sendListingResponse(requestId, false, errorText);
        setStatus(QStringLiteral("上架失败：%1").arg(errorText));
        return;
    }

    if (loadoutMode)
    {
        connection_.sendListingResponse(requestId, true, QStringLiteral("配装上架流程完成"));
        setStatus(QStringLiteral("配装上架流程完成"));
        return;
    }

    const bool listingSucceeded = ocrText.contains(QStringLiteral("上架"), Qt::CaseInsensitive);
    if (!listingSucceeded)
    {
        const QString resultText = ocrText.trimmed().isEmpty()
            ? QStringLiteral("OCR未识别到上架")
            : QStringLiteral("OCR未识别到上架，原文：%1").arg(ocrText.trimmed());
        connection_.sendListingResponse(requestId, false, resultText);
        setStatus(QStringLiteral("上架失败：OCR未识别到“上架”"));
        return;
    }

    if (!usageResult.startsWith(QStringLiteral("使用量核对成功")))
    {
        connection_.sendListingResponse(requestId, false, usageResult);
        setStatus(usageResult);
        return;
    }

    connection_.sendListingResponse(requestId, true, QStringLiteral("OCR识别到上架，%1").arg(usageResult));
    setStatus(QStringLiteral("上架成功，使用量核对成功"));
}

bool ClientController::waitForAutomation(int milliseconds, QString& errorText)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < milliseconds)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (automationCancelRequested_.load())
        {
            errorText = QStringLiteral("用户或服务端已强制停止当前动作");
            return false;
        }
        QThread::msleep(25);
    }
    return true;
}

bool ClientController::waitForListingQuantityReady(QString& errorText, const QString& prefix,
    int timeoutMs, bool* performedDismount)
{
    constexpr int ReferenceWidth = 2560;
    constexpr int ReferenceHeight = 1440;
    constexpr int PollIntervalMs = 150;
    constexpr int PostDismountPollIntervalMs = 300;
    if (performedDismount)
        *performedDismount = false;
    const auto log = [this](const QString& text) { emit connectionLogAppended(text); };
    const auto click = [this, &errorText](POINT point) {
        return input_.clickReferencePoint(boundHwnd_, point, 2560, 1440, 0, errorText);
    };
    const auto readText = [this, &errorText](int x, int y, int width, int height,
        const QString& filePrefix, QString& text) {
        OcrRegionConfig region;
        region.enabled = true;
        region.x = x; region.y = y; region.width = width; region.height = height;
        region.referenceWidth = 2560; region.referenceHeight = 1440;
        QString ocrError;
        const QString imagePath = captureOcrRegion(region, filePrefix, ocrError);
        if (imagePath.isEmpty()) { errorText = ocrError; return false; }
        text = umiOcrRunner_.recognizeImage(imagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), ocrError);
        QFile::remove(imagePath);
        if (!ocrError.isEmpty() && !ocrError.contains(QStringLiteral("No text found in image"), Qt::CaseInsensitive))
        { errorText = ocrError; return false; }
        return true;
    };
    const QRegularExpression quantityPattern(QStringLiteral(R"((\d+)\s*/\s*(\d+))"));
    QElapsedTimer timer;
    timer.start();
    int attempt = 0;
    while (timer.elapsed() < timeoutMs)
    {
        if (automationCancelRequested_.load()) { errorText = QStringLiteral("用户或服务端已强制停止当前动作"); return false; }
        QString text;
        if (readText(108, 185, 189, 45, QStringLiteral("%1_%2").arg(prefix).arg(++attempt), text))
        {
            const auto match = quantityPattern.match(text.simplified());
            if (match.hasMatch() && match.captured(1).toInt() == 0)
            {
                log(QStringLiteral("%1：识别到数量 %2，继续领取").arg(prefix, text.simplified()));
                // 数量归零后等待界面稳定，再点击领取按钮。
                if (!waitForAutomation(1500, errorText))
                    return false;
                return true;
            }
        }
        if (!waitForAutomation(PollIntervalMs, errorText)) return false;
    }

    // 数量长时间未归零时，依次查找并点击两个“下架”按钮，然后重新确认 X/N。
    // OCR 没找到或 X 仍不为 0 时继续重试，不能直接当作成功返回。
    const auto findAndClickKeyword = [this, &errorText, ReferenceWidth, ReferenceHeight, PollIntervalMs](
        int x, int y, int width, int height, const QString& filePrefix, const QString& keyword) {
        OcrRegionConfig region;
        region.enabled = true;
        region.x = x;
        region.y = y;
        region.width = width;
        region.height = height;
        region.referenceWidth = ReferenceWidth;
        region.referenceHeight = ReferenceHeight;
        QElapsedTimer timer;
        timer.start();
        int attempt = 0;
        while (timer.elapsed() < 5000)
        {
            if (automationCancelRequested_.load())
            {
                errorText = QStringLiteral("用户或服务端已强制停止当前动作");
                return false;
            }
            QString ocrError;
            const QString imagePath = captureOcrRegion(
                region, QStringLiteral("%1_%2").arg(filePrefix).arg(++attempt), ocrError);
            if (!imagePath.isEmpty())
            {
                QRect bounds;
                const bool found = umiOcrRunner_.findTextBounds(
                    imagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), keyword, bounds, ocrError);
                QFile::remove(imagePath);
                if (found)
                {
                    int clientWidth = 0;
                    int clientHeight = 0;
                    if (!binder_.clientSize(boundHwnd_, clientWidth, clientHeight)
                        || clientWidth <= 0 || clientHeight <= 0)
                    {
                        errorText = QStringLiteral("读取游戏窗口客户区失败");
                        return false;
                    }
                    const QRect scaledRegion = ScaleReferenceRect(
                        QRect(x, y, width, height), QSize(ReferenceWidth, ReferenceHeight),
                        QSize(clientWidth, clientHeight));
                    const QPoint relativeCenter = bounds.center();
                    if (!QRect(QPoint(0, 0), scaledRegion.size()).contains(relativeCenter))
                    {
                        errorText = QStringLiteral("OCR识别到的“%1”坐标超出截图区域").arg(keyword);
                        return false;
                    }
                    return input_.clickClientPoint(boundHwnd_, POINT{
                        scaledRegion.x() + relativeCenter.x(),
                        scaledRegion.y() + relativeCenter.y()
                    }, 0, errorText);
                }
            }
            if (!waitForAutomation(PollIntervalMs, errorText))
                return false;
        }
        errorText = QStringLiteral("5 秒内未找到“%1”").arg(keyword);
        return false;
    };

    for (;;)
    {
        if (automationCancelRequested_.load())
        {
            errorText = QStringLiteral("用户或服务端已强制停止当前动作");
            return false;
        }
        QString quantityText;
        if (readText(108, 185, 189, 45, QStringLiteral("%1_retry_quantity").arg(prefix), quantityText))
        {
            const auto match = quantityPattern.match(quantityText.simplified());
            if (match.hasMatch() && match.captured(1).toInt() == 0)
            {
                if (!waitForAutomation(1500, errorText))
                    return false;
                return true;
            }
        }
        if (!findAndClickKeyword(895, 358, 210, 40,
            QStringLiteral("%1_dismount_first").arg(prefix), QStringLiteral("下架"))
            || !waitForAutomation(1500, errorText)
            || !findAndClickKeyword(1296, 899, 400, 75,
                QStringLiteral("%1_dismount_second").arg(prefix), QStringLiteral("下架"))
            || !waitForAutomation(1500, errorText))
        {
            return false;
        }
        log(QStringLiteral("%1：已点击两处下架，重新检查 X/N").arg(prefix));
        if (performedDismount)
            *performedDismount = true;
        // 下架后的边界固定为 300ms：每 300ms 重试识别 X/N。
        // 只有明确识别到合法数量且 X>0 才进入下一轮下架；X=0 才结束。
        for (;;)
        {
            if (!waitForAutomation(PostDismountPollIntervalMs, errorText))
                return false;
            QString retryText;
            if (!readText(108, 185, 189, 45,
                QStringLiteral("%1_after_dismount").arg(prefix), retryText))
                continue;
            const auto retryMatch = quantityPattern.match(retryText.simplified());
            if (!retryMatch.hasMatch())
                continue;
            log(QStringLiteral("%1：下架后数量 %2").arg(prefix, retryText.simplified()));
            if (retryMatch.captured(1).toInt() == 0)
            {
                if (!waitForAutomation(1500, errorText)) return false;
                return true;
            }
            break;
        }
        log(QStringLiteral("%1：下架后数量仍不为 0，300ms 检查完成，继续下一轮下架").arg(prefix));
    }
}

bool ClientController::executeCoinRevenueClaimSequence(bool& noRevenue, QString& errorText)
{
    noRevenue = false;
    if (!boundHwnd_ || !binder_.isValid(boundHwnd_))
    {
        errorText = QStringLiteral("未绑定有效游戏窗口");
        return false;
    }

    constexpr int ReferenceWidth = 2560;
    constexpr int ReferenceHeight = 1440;
    constexpr int PollIntervalMs = 150;
    constexpr int TimeoutMs = 5000;
    const POINT revenueClaimButton{ 1224, 205 };

    const auto captureText = [this, &errorText](int x, int y, int width, int height,
        const QString& prefix, QString& text) {
        OcrRegionConfig region;
        region.enabled = true;
        region.x = x;
        region.y = y;
        region.width = width;
        region.height = height;
        region.referenceWidth = 2560;
        region.referenceHeight = 1440;
        QString ocrError;
        const QString imagePath = captureOcrRegion(region, prefix, ocrError);
        if (imagePath.isEmpty())
        {
            errorText = ocrError;
            return false;
        }
        text = umiOcrRunner_.recognizeImage(
            imagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), ocrError);
        QFile::remove(imagePath);
        // 空白区域被 UmiOCR 报告为 No text，属于本流程的正常“尚未出现”状态。
        if (!ocrError.isEmpty() && !ocrError.contains(QStringLiteral("No text found in image"), Qt::CaseInsensitive))
        {
            errorText = ocrError;
            return false;
        }
        return true;
    };

    if (!waitForAutomation(2000, errorText))
        return false;

    const auto click = [this, &errorText, ReferenceWidth, ReferenceHeight](POINT point) {
        return input_.clickReferencePoint(boundHwnd_, point, 2560, 1440, 0, errorText);
    };
    if (!waitForListingQuantityReady(errorText, QStringLiteral("coin_revenue_quantity")))
        return false;
    if (!click(revenueClaimButton) || !waitForAutomation(2000, errorText))
        return false;

    QElapsedTimer completeTimer;
    completeTimer.start();
    bool completeFound = false;
    int pollIndex = 0;
    while (completeTimer.elapsed() < TimeoutMs)
    {
        if (automationCancelRequested_.load())
        {
            errorText = QStringLiteral("用户或服务端已强制停止当前动作");
            return false;
        }

        QString recognizedText;
        if (!captureText(1153, 152, 285, 375,
            QStringLiteral("coin_revenue_complete_%1").arg(++pollIndex), recognizedText))
        {
            errorText.clear();
        }
        if (recognizedText.simplified().contains(QStringLiteral("领取完成"), Qt::CaseInsensitive))
        {
            completeFound = true;
            break;
        }
        if (!waitForAutomation(PollIntervalMs, errorText))
            return false;
    }

    if (!completeFound)
    {
        errorText = QStringLiteral("5 秒内未找到“领取完成”");
        return false;
    }

    // 领取完成后立即按空格，随后等待界面标记再返回，避免依赖固定等待时间。
    if (!input_.pressKey(VK_SPACE, errorText))
        return false;

    const auto waitForTextThenEscape = [this, &errorText](int x, int y, int width, int height,
        const QString& prefix, const QString& keyword) {
        constexpr int PollIntervalMs = 150;
        constexpr int TimeoutMs = 5000;
        QElapsedTimer timer;
        timer.start();
        int attempt = 0;
        while (timer.elapsed() < TimeoutMs)
        {
            if (automationCancelRequested_.load())
            {
                errorText = QStringLiteral("用户或服务端已强制停止当前动作");
                return false;
            }
            OcrRegionConfig region;
            region.enabled = true;
            region.x = x;
            region.y = y;
            region.width = width;
            region.height = height;
            region.referenceWidth = 2560;
            region.referenceHeight = 1440;
            QString ocrError;
            const QString imagePath = captureOcrRegion(
                region, QStringLiteral("%1_%2").arg(prefix).arg(++attempt), ocrError);
            QString recognizedText;
            if (!imagePath.isEmpty())
            {
                recognizedText = umiOcrRunner_.recognizeImage(
                    imagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), ocrError);
                QFile::remove(imagePath);
            }
            const bool emptyOcr = ocrError.contains(QStringLiteral("No text found in image"),
                Qt::CaseInsensitive);
            if (!ocrError.isEmpty() && !emptyOcr)
            {
                errorText = ocrError;
                return false;
            }
            if (recognizedText.simplified().contains(keyword, Qt::CaseInsensitive))
                return input_.pressKey(VK_ESCAPE, errorText);
            if (!waitForAutomation(PollIntervalMs, errorText))
                return false;
        }
        errorText = QStringLiteral("5 秒内未找到“%1”").arg(keyword);
        return false;
    };

    if (!waitForTextThenEscape(190, 46, 120, 50,
        QStringLiteral("coin_start_game"), QStringLiteral("开始游戏")))
    {
        return false;
    }
    if (!waitForTextThenEscape(2049, 1233, 400, 75,
        QStringLiteral("coin_confirm_loadout"), QStringLiteral("确认配装")))
    {
        return false;
    }
    if (!waitForAutomation(1000, errorText))
        return false;
    return true;
}

bool ClientController::executeBossListingSequence(const QString& listingPrice, QString& errorText)
{
    if (!boundHwnd_ || !binder_.isValid(boundHwnd_))
    {
        errorText = QStringLiteral("未绑定有效游戏窗口");
        return false;
    }
    if (listingPrice.trimmed().isEmpty())
    {
        errorText = QStringLiteral("服务端未下发老板上架价格");
        return false;
    }

    constexpr int ReferenceWidth = 2560;
    constexpr int ReferenceHeight = 1440;
    constexpr int PollIntervalMs = 150;
    constexpr int TimeoutMs = 5000;
    const auto click = [this, &errorText, ReferenceWidth, ReferenceHeight](POINT point) {
        return input_.clickReferencePoint(boundHwnd_, point, ReferenceWidth, ReferenceHeight, 0, errorText);
    };
    const auto findAndClick = [this, &errorText, ReferenceWidth, ReferenceHeight,
        PollIntervalMs, TimeoutMs](int x, int y, int width, int height,
        const QString& keyword, const QString& prefix) {
        OcrRegionConfig region;
        region.enabled = true;
        region.x = x;
        region.y = y;
        region.width = width;
        region.height = height;
        region.referenceWidth = ReferenceWidth;
        region.referenceHeight = ReferenceHeight;

        QElapsedTimer timer;
        timer.start();
        int attempt = 0;
        QString lastError;
        while (timer.elapsed() < TimeoutMs)
        {
            if (automationCancelRequested_.load())
            {
                errorText = QStringLiteral("用户或服务端已强制停止当前动作");
                return false;
            }

            QString ocrError;
            const QString imagePath = captureOcrRegion(
                region, QStringLiteral("%1_%2").arg(prefix).arg(++attempt), ocrError);
            if (!imagePath.isEmpty())
            {
                QRect bounds;
                const bool found = umiOcrRunner_.findTextBounds(
                    imagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), keyword, bounds, ocrError);
                QFile::remove(imagePath);
                if (found)
                {
                    int clientWidth = 0;
                    int clientHeight = 0;
                    if (!binder_.clientSize(boundHwnd_, clientWidth, clientHeight)
                        || clientWidth <= 0 || clientHeight <= 0)
                    {
                        errorText = QStringLiteral("读取游戏窗口客户区失败");
                        return false;
                    }
                    const QRect scaledRegion = ScaleReferenceRect(
                        QRect(x, y, width, height), QSize(ReferenceWidth, ReferenceHeight),
                        QSize(clientWidth, clientHeight));
                    const QPoint relativeCenter = bounds.center();
                    if (!QRect(QPoint(0, 0), scaledRegion.size()).contains(relativeCenter))
                    {
                        errorText = QStringLiteral("OCR识别到的“%1”坐标超出截图区域").arg(keyword);
                        return false;
                    }
                    const POINT clickPoint{
                        scaledRegion.x() + relativeCenter.x(),
                        scaledRegion.y() + relativeCenter.y()
                    };
                    return input_.clickClientPoint(boundHwnd_, clickPoint, 0, errorText);
                }
            }
            lastError = ocrError;
            if (!waitForAutomation(PollIntervalMs, errorText))
                return false;
        }
        errorText = lastError.isEmpty()
            ? QStringLiteral("5 秒内未识别到“%1”").arg(keyword)
            : QStringLiteral("5 秒内未识别到“%1”：%2").arg(keyword, lastError);
        return false;
    };
    const auto captureText = [this, &errorText](int x, int y, int width, int height,
        const QString& prefix, QString& text) {
        OcrRegionConfig region;
        region.enabled = true;
        region.x = x;
        region.y = y;
        region.width = width;
        region.height = height;
        region.referenceWidth = 2560;
        region.referenceHeight = 1440;
        QString ocrError;
        const QString imagePath = captureOcrRegion(region, prefix, ocrError);
        if (imagePath.isEmpty())
        {
            errorText = ocrError;
            return false;
        }
        text = umiOcrRunner_.recognizeImage(
            imagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), ocrError);
        QFile::remove(imagePath);
        if (!ocrError.isEmpty()
            && !ocrError.contains(QStringLiteral("No text found in image"), Qt::CaseInsensitive))
        {
            errorText = ocrError;
            return false;
        }
        return true;
    };

    // 首次准备状态确认后，读取老板当前金额作为本轮收益统计的初始基准。
    // 金额支持无单位、K、M，并要求连续三次 OCR 结果一致后才回报。
    const auto reportBossAmountBeforeReady = [this, &errorText]() {
        const QRegularExpression amountPattern(
            QStringLiteral(R"((\d[\d,.]*\d|\d)[\s]*([KkMm])?)"));
        QString previous;
        int stableCount = 0;
        QElapsedTimer timer;
        // 移动到金额区域后只等待一次，随后每 100ms 重新截图识别。
        int bossClientWidth = 0;
        int bossClientHeight = 0;
        if (!binder_.clientSize(boundHwnd_, bossClientWidth, bossClientHeight)
            || bossClientWidth <= 0 || bossClientHeight <= 0)
        {
            errorText = QStringLiteral("读取老板游戏窗口客户区失败");
            return false;
        }
        const bool bossWindow1K = bossClientWidth <= 1920 && bossClientHeight <= 1080;
        const POINT bossAmountMovePoint = bossWindow1K ? POINT{ 1639, 54 } : POINT{ 2128, 67 };
        const int bossReferenceWidth = bossWindow1K ? 1920 : 2560;
        const int bossReferenceHeight = bossWindow1K ? 1080 : 1440;
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            previous.clear();
            stableCount = 0;
            POINT retryPoint = bossAmountMovePoint;
            retryPoint.x += attempt * 3;
            if (!input_.moveReferencePoint(boundHwnd_, retryPoint,
                bossReferenceWidth, bossReferenceHeight, errorText))
                return false;
            if (!waitForAutomation(1000, errorText))
                return false;
            timer.start();
            while (timer.elapsed() < 5000)
            {
            OcrRegionConfig region;
            region.enabled = true;
            // 老板资金区域同样包含中文说明，金额解析器只提取数字及分隔符，忽略中文。
            region.x = 2015; region.y = 325; region.width = 450; region.height = 95;
            region.referenceWidth = 2560; region.referenceHeight = 1440;
            QString ocrError;
            const QString imagePath = captureOcrRegion(
                region, QStringLiteral("boss_return_amount"), ocrError);
            QString text;
            if (!imagePath.isEmpty())
            {
                text = umiOcrRunner_.recognizeImage(
                    imagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), ocrError).simplified();
                QFile::remove(imagePath);
            }
            emit connectionLogAppended(QStringLiteral("金额OCR[boss_return_amount]：原始文本“%1”").arg(text));
            const QString normalizedOcrText = NormalizeAmountOcrText(text);
            QRegularExpressionMatch match;
            double largestValue = -1.0;
            auto iterator = amountPattern.globalMatch(normalizedOcrText);
            while (iterator.hasNext())
            {
                const auto candidate = iterator.next();
                const QString candidateValue = NormalizeAmountNumericToken(candidate.captured(1));
                bool candidateOk = false;
                const double numericValue = candidateValue.toDouble(&candidateOk);
                if (!candidateOk)
                    continue;
                if (candidate.captured(2).isEmpty() && candidateValue.size() <= 1)
                    continue;
                if (!match.hasMatch()
                    || (!candidate.captured(2).isEmpty() && match.captured(2).isEmpty())
                    || (candidate.captured(2).isEmpty() == match.captured(2).isEmpty()
                        && numericValue > largestValue))
                {
                    match = candidate;
                    largestValue = numericValue;
                }
            }
            if (match.hasMatch())
            {
                const QString valueText = NormalizeAmountNumericToken(match.captured(1));
                bool ok = false;
                double value = valueText.toDouble(&ok);
                if (ok)
                {
                    const QString suffix = match.captured(2).toUpper();
                    if (suffix == QStringLiteral("K")) value *= 1000.0;
                    else if (suffix == QStringLiteral("M")) value *= 1000000.0;
                    const QString sample = QString::number(static_cast<qint64>(value));
                    stableCount = sample == previous ? stableCount + 1 : 1;
                    previous = sample;
                    emit connectionLogAppended(QStringLiteral(
                        "金额OCR[boss_return_amount]：%1，稳定 %2/3").arg(sample).arg(stableCount));
                    if (stableCount >= 3)
                    {
                        if (connection_.sendAmountReport(sample.toLongLong()))
                        {
                            emit connectionLogAppended(QStringLiteral(
                                "金额OCR[boss_return_amount]：已回报老板金额 %1").arg(sample));
                            return true;
                        }
                        errorText = QStringLiteral("初始老板金额回报服务端失败");
                        return false;
                    }
                }
            }
                if (!waitForAutomation(100, errorText))
                    return false;
            }
        }
        if (errorText.isEmpty())
            errorText = QStringLiteral("5 秒内未识别到老板金额基线（已重试右移 3/6 像素）");
        return false;
    };

    // 执行入口点击后，先留出 1 秒让价格提示/确认方案弹窗稳定，再进行 OCR 清理。
    if (!waitForAutomation(1000, errorText))
        return false;
    // 清理可能残留的价格提示/确认方案弹窗，再进行初始化数字检测。
    {
        OcrRegionConfig promptRegion;
        promptRegion.enabled = true;
        promptRegion.x = 538;
        promptRegion.y = 313;
        promptRegion.width = 275;
        promptRegion.height = 60;
        promptRegion.referenceWidth = ReferenceWidth;
        promptRegion.referenceHeight = ReferenceHeight;
        QElapsedTimer promptTimer;
        promptTimer.start();
        int promptAttempt = 0;
        while (promptTimer.elapsed() < 5000)
        {
            if (automationCancelRequested_.load())
            {
                errorText = QStringLiteral("用户或服务端已强制停止当前动作");
                return false;
            }
            QString ocrError;
            const QString imagePath = captureOcrRegion(
                promptRegion, QStringLiteral("boss_execute_prompt_%1").arg(++promptAttempt), ocrError);
            QString recognized;
            if (!imagePath.isEmpty())
            {
                recognized = umiOcrRunner_.recognizeImage(
                    imagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), ocrError);
                QFile::remove(imagePath);
            }
            const bool emptyOcr = ocrError.contains(QStringLiteral("No text found in image"),
                Qt::CaseInsensitive);
            if (!ocrError.isEmpty() && !emptyOcr)
            {
                errorText = ocrError;
                return false;
            }
            const bool promptFound = recognized.contains(QStringLiteral("价格变动提醒"), Qt::CaseInsensitive)
                || recognized.contains(QStringLiteral("确认实现方案"), Qt::CaseInsensitive);
            if (!promptFound)
            {
                emit connectionLogAppended(QStringLiteral("老板执行：第 %1 次检查未发现价格提示").arg(promptAttempt));
                break;
            }
            emit connectionLogAppended(QStringLiteral("老板执行：第 %1 次检测到价格提示，发送 ESC").arg(promptAttempt));
            if (!input_.pressKey(VK_ESCAPE, errorText)
                || !waitForAutomation(1500, errorText))
                return false;
            emit connectionLogAppended(QStringLiteral("老板执行：ESC 已发送，准备复检价格提示"));
        }
        emit connectionLogAppended(QStringLiteral("老板执行：价格提示清理阶段结束"));
    }

    // 首次执行准备数字 OCR 前，先读取老板当前金额作为本轮初始基准。
    // 首轮初始化检测：执行流程开始前先确认老板当前界面是否已有准备数字。
    // 该结果只作为本轮配对的初始化依据，服务端已将本轮客户端标记为“执行中”，
    // 因此不会提前触发下一轮自动执行；流程末尾仍会再次确认并回报准备成功。
    {
        OcrRegionConfig initialReadyRegion;
        initialReadyRegion.enabled = true;
        initialReadyRegion.x = 2048;
        initialReadyRegion.y = 1123;
        initialReadyRegion.width = 400;
        initialReadyRegion.height = 80;
        initialReadyRegion.referenceWidth = ReferenceWidth;
        initialReadyRegion.referenceHeight = ReferenceHeight;
        QElapsedTimer initialReadyTimer;
        initialReadyTimer.start();
        int initialReadyAttempt = 0;
        bool initialReadyFound = false;
        while (initialReadyTimer.elapsed() < 5000)
        {
            if (automationCancelRequested_.load())
            {
                errorText = QStringLiteral("用户或服务端已强制停止当前动作");
                return false;
            }
            QString ocrError;
            const QString imagePath = captureOcrRegion(
                initialReadyRegion,
                QStringLiteral("boss_initial_ready_%1").arg(++initialReadyAttempt), ocrError);
            if (!imagePath.isEmpty())
            {
                const QString recognized = umiOcrRunner_.recognizeImage(
                    imagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), ocrError);
                QFile::remove(imagePath);
                if (recognized.contains(QRegularExpression(QStringLiteral("\\d"))))
                {
                    initialReadyFound = true;
                    break;
                }
            }
            if (!ocrError.isEmpty()
                && !ocrError.contains(QStringLiteral("No text found in image"), Qt::CaseInsensitive))
            {
                errorText = ocrError;
                return false;
            }
            if (!waitForAutomation(150, errorText))
                return false;
        }
        if (!initialReadyFound)
        {
            errorText = QStringLiteral("5 秒内未识别到老板初始准备数字");
            return false;
        }
    }

    QString currentBossPrice = listingPrice.trimmed();
    const auto enterAndVerifyBossPrice = [&](const POINT& panelPoint, const POINT& inputPoint) {
        bool expectedOk = false;
        const double expectedPrice = currentBossPrice.toDouble(&expectedOk);
        if (!expectedOk)
        {
            errorText = QStringLiteral("老板上架价格格式无效");
            return false;
        }
        OcrRegionConfig priceRegion;
        priceRegion.enabled = true;
        priceRegion.x = 1548; priceRegion.y = 837;
        priceRegion.width = 400; priceRegion.height = 50;
        priceRegion.referenceWidth = ReferenceWidth;
        priceRegion.referenceHeight = ReferenceHeight;
        const QRegularExpression numberPattern(QStringLiteral(R"(\d+(?:[,.]\d+)?)"));
        int attempt = 0;
        for (;;)
        {
            if (automationCancelRequested_.load())
            {
                errorText = QStringLiteral("用户或服务端已强制停止当前动作");
                return false;
            }
            if (!click(panelPoint) || !waitForAutomation(300, errorText))
                return false;
            OcrRegionConfig panelQuantityRegion;
            panelQuantityRegion.enabled = true;
            panelQuantityRegion.x = 1957;
            panelQuantityRegion.y = 678;
            panelQuantityRegion.width = 80;
            panelQuantityRegion.height = 32;
            panelQuantityRegion.referenceWidth = ReferenceWidth;
            panelQuantityRegion.referenceHeight = ReferenceHeight;
            const QRegularExpression panelQuantityPattern(QStringLiteral(R"((\d+)\s*/\s*(\d+))"));
            for (int panelAttempt = 1; panelAttempt <= 3; ++panelAttempt)
            {
                QString panelOcrError;
                const QString panelImage = captureOcrRegion(
                    panelQuantityRegion,
                    QStringLiteral("boss_price_panel_quantity_%1").arg(panelAttempt), panelOcrError);
                QString panelText;
                if (!panelImage.isEmpty())
                {
                    panelText = umiOcrRunner_.recognizeImage(
                        panelImage, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), panelOcrError);
                    QFile::remove(panelImage);
                }
                const QRegularExpressionMatch panelMatch = panelQuantityPattern.match(panelText);
                if (!panelMatch.hasMatch() || panelMatch.captured(1).toInt() != 1 || panelAttempt == 3)
                    break;
                if (!click(panelPoint) || !waitForAutomation(300, errorText))
                    return false;
            }
            if (!click(inputPoint) || !waitForAutomation(500, errorText)
                || !click(inputPoint)
                || !input_.pressKeyRepeated(VK_BACK, 5, 20, errorText)
                || !waitForAutomation(500, errorText)
                || !input_.pasteText(boundHwnd_, currentBossPrice, errorText)
                || !waitForAutomation(1500, errorText))
                return false;
            QString ocrError;
            const QString imagePath = captureOcrRegion(
                priceRegion, QStringLiteral("boss_price_verify_%1").arg(++attempt), ocrError);
            QString recognized;
            if (!imagePath.isEmpty())
            {
                recognized = umiOcrRunner_.recognizeImage(
                    imagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), ocrError);
                QFile::remove(imagePath);
            }
            const QRegularExpressionMatch match = numberPattern.match(recognized);
            if (match.hasMatch())
            {
                QString normalized = match.captured(0);
                normalized.remove(',');
                bool actualOk = false;
                const double actualPrice = normalized.toDouble(&actualOk);
                if (actualOk && qAbs(actualPrice - expectedPrice) < 0.0001)
                    return true;
            }
            if (!waitForAutomation(150, errorText))
                return false;
        }
    };

    SetForegroundWindow(boundHwnd_);
    const auto clearBossPricePrompt = [&]() {
        OcrRegionConfig promptRegion;
        promptRegion.enabled = true;
        promptRegion.x = 538;
        promptRegion.y = 313;
        promptRegion.width = 275;
        promptRegion.height = 60;
        promptRegion.referenceWidth = ReferenceWidth;
        promptRegion.referenceHeight = ReferenceHeight;
        QElapsedTimer promptTimer;
        promptTimer.start();
        int promptAttempt = 0;
        bool foundAny = false;
        while (promptTimer.elapsed() < TimeoutMs)
        {
            if (automationCancelRequested_.load())
            {
                errorText = QStringLiteral("用户或服务端已强制停止当前动作");
                return false;
            }
            QString ocrError;
            const QString imagePath = captureOcrRegion(
                promptRegion, QStringLiteral("boss_warehouse_prompt_%1").arg(++promptAttempt), ocrError);
            QString recognized;
            if (!imagePath.isEmpty())
            {
                recognized = umiOcrRunner_.recognizeImage(
                    imagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), ocrError);
                QFile::remove(imagePath);
            }
            const bool emptyOcr = ocrError.contains(QStringLiteral("No text found in image"), Qt::CaseInsensitive);
            if (!ocrError.isEmpty() && !emptyOcr)
            {
                errorText = ocrError;
                return false;
            }
            const bool promptFound = recognized.contains(QStringLiteral("价格变动提醒"), Qt::CaseInsensitive)
                || recognized.contains(QStringLiteral("确认实现方案"), Qt::CaseInsensitive);
            emit connectionLogAppended(QStringLiteral("老板仓库回调：第 %1 次检查价格提示，结果=%2")
                .arg(promptAttempt)
                .arg(promptFound ? QStringLiteral("发现") : QStringLiteral("未发现")));
            if (promptFound)
            {
                foundAny = true;
                if (!input_.pressKey(VK_ESCAPE, errorText)
                    || !waitForAutomation(1500, errorText))
                    return false;
            }
            else if (foundAny)
            {
                return true;
            }
            else if (!waitForAutomation(100, errorText))
            {
                return false;
            }
        }
        return true;
    };

    bool warehouseFound = findAndClick(173, 44, 1217, 60, QStringLiteral("仓库"), QStringLiteral("boss_warehouse"));
    if (!warehouseFound)
    {
        emit connectionLogAppended(QStringLiteral("老板执行：仓库识别失败，开始回调清理价格提示"));
        if (!clearBossPricePrompt())
            return false;
        emit connectionLogAppended(QStringLiteral("老板执行：价格提示回调结束，重新识别仓库"));
        warehouseFound = findAndClick(173, 44, 1217, 60, QStringLiteral("仓库"), QStringLiteral("boss_warehouse_retry"));
    }
    if (!warehouseFound
        || !findAndClick(394, 1380, 80, 45, QStringLiteral("转移全部"), QStringLiteral("boss_transfer_all"))
        || !findAndClick(173, 44, 1217, 60, QStringLiteral("交易行"), QStringLiteral("boss_market"))
        || !findAndClick(195, 125, 495, 46, QStringLiteral("出售"), QStringLiteral("boss_sell")))
    {
        return false;
    }

    const POINT priceArea{ 1693, 293 };
    if (!click(priceArea) || !waitForAutomation(2000, errorText))
        return false;

    QElapsedTimer listingItemTimer;
    listingItemTimer.start();
    bool listingItemFound = false;
    int attempt = 0;
    QString lastItemError;
    while (listingItemTimer.elapsed() < TimeoutMs)
    {
        if (!click(priceArea) || !waitForAutomation(300, errorText))
            return false;

        OcrRegionConfig region;
        region.enabled = true;
        region.x = 540;
        region.y = 309;
        region.width = 190;
        region.height = 55;
        region.referenceWidth = ReferenceWidth;
        region.referenceHeight = ReferenceHeight;
        QString ocrError;
        const QString imagePath = captureOcrRegion(
            region, QStringLiteral("boss_listing_item_%1").arg(++attempt), ocrError);
        if (!imagePath.isEmpty())
        {
            QRect bounds;
            listingItemFound = umiOcrRunner_.findTextBounds(
                imagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(),
                QStringLiteral("上架物品"), bounds, ocrError);
            QFile::remove(imagePath);
        }
        if (listingItemFound)
            break;
        lastItemError = ocrError;
        if (!waitForAutomation(PollIntervalMs, errorText))
            return false;
    }
    if (!listingItemFound)
    {
        errorText = lastItemError.isEmpty()
            ? QStringLiteral("5 秒内未识别到“上架物品”")
            : QStringLiteral("5 秒内未识别到“上架物品”：%1").arg(lastItemError);
        return false;
    }

    const POINT pricePanelClick{ 1939, 745 };
    const POINT priceInput{ 1827, 858 };
    const POINT coinDispatchPoint{ 1756, 1000 };
    const POINT returnEntry{ 495, 1400 };
    const POINT finalSelection{ 234, 563 };
    // 老板价格可能需要在下架后按“服务端价格 - 20”重新上架并再次确认数量。
    bool performedDismount = false;
    bool itemSoldAfterDismount = false;
    for (;;)
    {
        // 下架后返回价格输入前，重新定位“上架物品”，避免直接操作旧的价格面板。
        if (performedDismount)
        {
            const POINT relistPriceArea{ 1693, 282 };
            bool relistedItemFound = false;
            QString relistItemError;
            if (!click(relistPriceArea) || !waitForAutomation(1500, errorText))
                return false;
            QElapsedTimer relistTimer;
            relistTimer.start();
            int relistAttempt = 0;
            while (relistTimer.elapsed() < TimeoutMs)
            {
                if (automationCancelRequested_.load())
                {
                    errorText = QStringLiteral("用户或服务端已强制停止当前动作");
                    return false;
                }
                if (!click(relistPriceArea))
                    return false;
                OcrRegionConfig relistRegion;
                relistRegion.enabled = true;
                relistRegion.x = 540;
                relistRegion.y = 309;
                relistRegion.width = 190;
                relistRegion.height = 55;
                relistRegion.referenceWidth = ReferenceWidth;
                relistRegion.referenceHeight = ReferenceHeight;
                QString ocrError;
                const QString imagePath = captureOcrRegion(
                    relistRegion, QStringLiteral("boss_relist_item_%1").arg(relistAttempt), ocrError);
                if (!imagePath.isEmpty())
                {
                    QRect bounds;
                    relistedItemFound = umiOcrRunner_.findTextBounds(
                        imagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(),
                        QStringLiteral("上架物品"), bounds, ocrError);
                    QFile::remove(imagePath);
                }
                if (relistedItemFound)
                    break;
                relistItemError = ocrError;
                if (!waitForAutomation(PollIntervalMs, errorText))
                    return false;
            }
            if (!relistedItemFound)
            {
                // 下架后物品可能在重新上架前已被买走；此时本轮视为成功，跳过重上架。
                itemSoldAfterDismount = true;
                emit connectionLogAppended(QStringLiteral(
                    "老板执行：下架后 5 秒内未找到上架物品，视为货物已被买走，跳过减价重上架"));
                Q_UNUSED(relistItemError);
                break;
            }
        }
        if (itemSoldAfterDismount)
            break;
        if (!enterAndVerifyBossPrice(pricePanelClick, priceInput)
            || !click(coinDispatchPoint)
            || !waitForAutomation(1500, errorText))
            return false;
        performedDismount = false;
        if (!waitForListingQuantityReady(errorText, QStringLiteral("boss_revenue_quantity"), 15000,
            &performedDismount))
            return false;
        if (!performedDismount)
            break;
        bool priceOk = false;
        const double discountedPrice = currentBossPrice.toDouble(&priceOk) - 20.0;
        if (!priceOk || discountedPrice <= 0.0)
        {
            errorText = QStringLiteral("下架后老板价格减 20 无效：%1").arg(currentBossPrice);
            return false;
        }
        currentBossPrice = QString::number(discountedPrice, 'f', 2);
    }
    const POINT revenueClaimButton{ 1224, 205 };
    if (!click(revenueClaimButton) || !waitForAutomation(2000, errorText))
        return false;

    QElapsedTimer completeTimer;
    completeTimer.start();
    bool completeFound = false;
    int completeAttempt = 0;
    while (completeTimer.elapsed() < TimeoutMs)
    {
        if (automationCancelRequested_.load())
        {
            errorText = QStringLiteral("用户或服务端已强制停止当前动作");
            return false;
        }
        QString recognizedText;
        if (!captureText(1153, 152, 285, 375,
            QStringLiteral("boss_revenue_complete_%1").arg(++completeAttempt), recognizedText))
        {
            errorText.clear();
        }
        if (recognizedText.simplified().contains(QStringLiteral("领取完成"), Qt::CaseInsensitive))
        {
            completeFound = true;
            break;
        }
        if (!waitForAutomation(PollIntervalMs, errorText))
            return false;
    }
    if (!completeFound)
    {
        errorText = QStringLiteral("5 秒内未找到“领取完成”");
        return false;
    }
    if (!waitForAutomation(1500, errorText)
        || !input_.pressKey(VK_SPACE, errorText)
        || !waitForAutomation(1500, errorText))
    {
        return false;
    }

    if (!input_.pressKey(VK_ESCAPE, errorText)
        || !waitForAutomation(1500, errorText)
        || !click(returnEntry)
        || !waitForAutomation(1500, errorText)
        || !click(finalSelection))
    {
        return false;
    }

    // 老板领取完成返回后，先识别金额；金额确认成功后再识别右下角准备数字。
    if (!reportBossAmountBeforeReady())
    {
        errorText = QStringLiteral("返回准备状态前未识别到稳定老板金额");
        return false;
    }

    // 老板上架流程结束后，确认右下角是否出现数字；出现数字才报告“老板准备就绪”。
    OcrRegionConfig readyRegion;
    readyRegion.enabled = true;
    readyRegion.x = 2048;
    readyRegion.y = 1123;
    readyRegion.width = 400;
    readyRegion.height = 80;
    readyRegion.referenceWidth = ReferenceWidth;
    readyRegion.referenceHeight = ReferenceHeight;
    QString readyOcrError;
    bool ready = false;
    QElapsedTimer readyTimer;
    readyTimer.start();
    while (readyTimer.elapsed() < 5000)
    {
        if (automationCancelRequested_.load())
        {
            errorText = QStringLiteral("用户或服务端已强制停止当前动作");
            return false;
        }
        QString attemptError;
        const QString readyImagePath = captureOcrRegion(
            readyRegion, QStringLiteral("boss_ready_check_%1").arg(readyTimer.elapsed()), attemptError);
        if (!readyImagePath.isEmpty())
        {
            const QString readyText = umiOcrRunner_.recognizeImage(
                readyImagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), attemptError);
            QFile::remove(readyImagePath);
            if (readyText.contains(QRegularExpression(QStringLiteral("\\d"))))
            {
                ready = true;
                break;
            }
        }
        if (!attemptError.isEmpty())
            readyOcrError = attemptError;
        if (!waitForAutomation(150, errorText))
            return false;
    }
    connection_.sendTeamReady(ready ? 1 : 0, ready ? 1 : 0, 1);
    return true;
}

void ClientController::handleMailRequest(int requestId)
{
    if (!beginAutomationAction(QStringLiteral("领取邮件")))
    {
        connection_.sendMailResponse(requestId, false, QStringLiteral("已有自动化流程正在执行"));
        return;
    }
    struct ActionGuard
    {
        ClientController* controller = nullptr;
        ~ActionGuard() { if (controller) controller->finishAutomationAction(); }
    } guard{ this };

    QString errorText;
    if (!executeMailSequence(errorText))
    {
        connection_.sendMailResponse(requestId, false, errorText);
        setStatus(QStringLiteral("领取失败：%1").arg(errorText));
        return;
    }
    connection_.sendMailResponse(requestId, true, QStringLiteral("领取完成"));
    setStatus(QStringLiteral("领取邮件完成"));
}

void ClientController::handleAccountLoginRequest(int requestId, const QString& username, const QString& password)
{
    if (currentRole_ != ClientRole::Coin)
    {
        connection_.sendAccountLoginResponse(requestId, QStringLiteral("ERROR"), QStringLiteral("仅币号客户端支持上号"));
        return;
    }
    if (accountBanMonitoringEnabled_ || accountBanMonitor_.isRunning())
    {
        connection_.sendAccountLoginResponse(requestId, QStringLiteral("ERROR"), QStringLiteral("已有账号正在进行封禁监测"));
        return;
    }
    if (!beginAutomationAction(QStringLiteral("上号")))
    {
        connection_.sendAccountLoginResponse(requestId, QStringLiteral("ERROR"), QStringLiteral("已有自动化流程正在执行"));
        return;
    }
    accountLoginRequestId_ = requestId;
    accountLoginRunning_ = true;
    accountBanHandled_ = false;
    connection_.sendAccountLoginResponse(requestId, QStringLiteral("STARTED"), QStringLiteral("已开始上号"));
    accountLoginAutomation_.start(username, password);
}

void ClientController::handleAccountBanned(const QString& recognizedText)
{
    Q_UNUSED(recognizedText);
    if (!accountBanMonitoringEnabled_ || accountBanHandled_)
    {
        return;
    }

    accountBanHandled_ = true;
    accountBanMonitoringEnabled_ = false;
    accountBanMonitor_.stop();
    accountLoginSuccessMonitor_.stop();
    loginOcrRunning_ = false;
    automationCancelRequested_.store(true);
    accountLoginAutomation_.cancelAndStopProcesses();
    countdownTimer_.stop();
    scheduledTriggerTimer_.stop();
    countdown_ = 0;
    if (bulletOcrRunning_)
    {
        finishBulletOcrLoop(false, QStringLiteral("检测到账号封号/冻结"));
    }
    accountLoginRunning_ = false;
    automationActionRunning_ = false;
    connection_.sendAccountLoginResponse(accountLoginRequestId_, QStringLiteral("BANNED"), QStringLiteral("检测到账号封号/冻结"));
    setStatus(QStringLiteral("检测到账号封号/冻结，已结束相关进程，等待服务端切换账号"));
}

void ClientController::handleLoginReady()
{
    if (accountLoginReadyReported_ || !accountLoginRunning_ || !boundHwnd_) return;
    accountLoginReadyReported_ = true;
    // 登录成功后停止“常驻/TAB/行前备战”判断，继续保留同一个监测线程做封禁 OCR。
    accountLoginSuccessMonitor_.setStage(3);
    loginOcrRunning_ = false;
    rebindGameWindow();
    QTimer::singleShot(500, this, [this]() {
        connection_.sendAccountLoginResponse(accountLoginRequestId_, QStringLiteral("READY"), QStringLiteral("行前备战，登录成功"));
        accountLoginRunning_ = false;
        setStatus(QStringLiteral("已识别行前备战，登录成功"));
    });
}

void ClientController::runLoginSuccessOcr(int stage)
{
    if ((!accountLoginRunning_ && stage != 3) || (!accountBanMonitoringEnabled_ && stage == 3)
        || !boundHwnd_ || !binder_.isValid(boundHwnd_)) return;
    if (loginOcrRunning_.exchange(true)) return;
    OcrRegionConfig region;
    // 常驻、Tab 和封禁都读取完整客户区，避免固定小区域随窗口布局或缩放变化而偏移。
    if (stage == 0) { region = {true, 0, 0, 2560, 1440, 2560, 1440}; }
    else if (stage == 1) { region = {true, 0, 0, 2560, 1440, 2560, 1440}; }
    else if (stage == 2) { region = {true, 1946, 1262, 500, 56, 2560, 1440}; }
    else { region = {true, 0, 0, 2560, 1440, 2560, 1440}; }
    const QString executable = config_.loadUmiOcrPath();
    const int port = config_.loadUmiOcrPort();
    const QString keyword = stage == 0 ? QStringLiteral("常驻") : stage == 1 ? QStringLiteral("Tab/战术部门/研发部门/医疗部门") : QStringLiteral("行前备战");
    const HWND gameWindow = boundHwnd_;
    QPointer<ClientController> controller(this);
    std::thread([controller, gameWindow, region, stage, executable, port, keyword]() {
        const qint64 startedAt = QDateTime::currentMSecsSinceEpoch();
        AccountBanMonitor::loginOcrInFlight = true;
        UmiOcrRunner runner;
        QString error;
        const QString imagePath = CaptureDesktopOcrRegion(gameWindow, region,
            QStringLiteral("login_success_%1").arg(stage), error);
        QString recognizedText;
        QRect bounds;
        bool found = false;
        if (!imagePath.isEmpty())
        {
            if (stage == 3)
            {
                found = !(recognizedText = runner.recognizeImage(imagePath, executable, port, error)).trimmed().isEmpty();
            }
            else if (stage == 1)
            {
                // Tab 阶段一次 OCR 同时接受 Tab、战术部门、研发部门、医疗部门，
                // 避免为多个候选词重复请求 OCR。
                recognizedText = runner.recognizeImage(imagePath, executable, port, error);
                const QString normalizedRecognizedText = NormalizeLoginOcrText(recognizedText);
                const QStringList tabKeywords{
                    QStringLiteral("Tab"),
                    QStringLiteral("战术部门"),
                    QStringLiteral("研发部门"),
                    QStringLiteral("医疗部门")
                };
                for (const QString& candidate : tabKeywords)
                {
                    if (normalizedRecognizedText.contains(NormalizeLoginOcrText(candidate)))
                    {
                        found = true;
                        break;
                    }
                }
                if (!error.isEmpty())
                {
                    found = false;
                }
            }
            else
            {
                found = runner.findTextBounds(imagePath, executable, port, keyword, bounds, error, false, &recognizedText);
            }
            QFile::remove(imagePath);
        }
        AccountBanMonitor::loginOcrInFlight = false;
        const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - startedAt;
        if (controller)
        {
            const QString stageName = stage == 0 ? QStringLiteral("常驻")
                : stage == 1 ? QStringLiteral("TAB开始游戏")
                : stage == 2 ? QStringLiteral("行前备战") : QStringLiteral("封禁");
            QMetaObject::invokeMethod(controller, [controller, stageName, found, recognizedText, error, elapsed]() {
                if (controller)
                {
                    emit controller->connectionLogAppended(QStringLiteral("上号OCR[%1]：%2，耗时%3ms，文本：%4，错误：%5")
                        .arg(stageName, found ? QStringLiteral("找到") : QStringLiteral("未找到"))
                        .arg(elapsed)
                        .arg(recognizedText.trimmed().isEmpty() ? QStringLiteral("<空>") : recognizedText.simplified(),
                             error.isEmpty() ? QStringLiteral("<无>") : error));
                }
            }, Qt::QueuedConnection);
        }
        if (stage == 3) {
            const QString normalized = recognizedText;
            if (IsBanOcrText(normalized)) {
                QMetaObject::invokeMethod(controller, [controller, normalized]() {
                    if (controller) controller->handleAccountBanned(normalized);
                }, Qt::QueuedConnection);
            }
        }
        if (!controller) return;
        QMetaObject::invokeMethod(controller, [controller, stage, found, recognizedText]() {
            if (controller) controller->finishLoginSuccessOcr(stage, found, recognizedText);
        }, Qt::QueuedConnection);
    }).detach();
}

void ClientController::finishLoginSuccessOcr(int stage, bool found, const QString& recognizedText)
{
    Q_UNUSED(recognizedText);
    loginOcrRunning_ = false;
    if (!accountLoginRunning_ || !boundHwnd_) return;
    if (stage == 3)
    {
        // 封禁 OCR 未命中目标文案（包括空文本）时，返回常驻阶段继续轮换。
        accountLoginSuccessMonitor_.setStage(0);
        return;
    }
    if (!found)
    {
        // 进入游戏或行前备战未出现时，回到常驻检测，避免卡在中间阶段。
        // Tab 阶段由监测线程连续保留 10 秒，不能因单轮 OCR 未命中立即回退。
        if (stage != 0 && stage != 1)
        {
            accountLoginSuccessMonitor_.setStage(0);
        }
        return;
    }
    QString error;
    if (stage == 0)
    {
        POINT p{386, 1192};
        input_.clickReferencePoint(boundHwnd_, p, 2560, 1440, 0, error);
        accountLoginSuccessMonitor_.setStage(1);
    }
    else if (stage == 1)
    {
        input_.pressKey(VK_TAB, error);
        accountLoginSuccessMonitor_.setStage(2);
    }
    else
    {
        handleLoginReady();
    }
}

void ClientController::stopAccountBanMonitor()
{
    accountBanMonitoringEnabled_ = false;
    accountBanMonitor_.stop();
    accountLoginSuccessMonitor_.stop();
}

bool ClientController::executeMailSequence(QString& errorText)
{
    if (!boundHwnd_ || !binder_.isValid(boundHwnd_))
    {
        errorText = QStringLiteral("未绑定有效游戏窗口");
        return false;
    }

    constexpr int ReferenceWidth = 2560;
    constexpr int ReferenceHeight = 1440;
    constexpr int PollIntervalMs = 150;
    constexpr int TimeoutMs = 5000;
    const POINT revenueClaimButton{ 1224, 205 };
    const auto click = [this, &errorText, ReferenceWidth, ReferenceHeight](POINT point) {
        return input_.clickReferencePoint(boundHwnd_, point, 2560, 1440, 0, errorText);
    };
    const auto findKeyword = [this, &errorText, ReferenceWidth, ReferenceHeight](int x, int y, int width, int height, const QString& prefix,
        const QString& keyword, QRect& bounds) {
        OcrRegionConfig region;
        region.enabled = true;
        region.x = x; region.y = y; region.width = width; region.height = height;
        region.referenceWidth = ReferenceWidth; region.referenceHeight = ReferenceHeight;
        const QString imagePath = captureOcrRegion(region, prefix, errorText);
        if (imagePath.isEmpty()) return false;
        const bool found = umiOcrRunner_.findTextBounds(imagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), keyword, bounds, errorText);
        QFile::remove(imagePath);
        if (found)
        {
            int clientWidth = 0, clientHeight = 0;
            if (!binder_.clientSize(boundHwnd_, clientWidth, clientHeight))
            {
                errorText = QStringLiteral("读取游戏窗口客户区失败");
                return false;
            }
            const QRect scaledRegion = ScaleReferenceRect(QRect(x, y, width, height), QSize(ReferenceWidth, ReferenceHeight), QSize(clientWidth, clientHeight));
            bounds.translate(scaledRegion.topLeft());
        }
        return found;
    };
    const auto captureText = [this, &errorText](int x, int y, int width, int height,
        const QString& prefix, QString& text) {
        OcrRegionConfig region;
        region.enabled = true;
        region.x = x;
        region.y = y;
        region.width = width;
        region.height = height;
        region.referenceWidth = 2560;
        region.referenceHeight = 1440;
        QString ocrError;
        const QString imagePath = captureOcrRegion(region, prefix, ocrError);
        if (imagePath.isEmpty())
        {
            errorText = ocrError;
            return false;
        }
        text = umiOcrRunner_.recognizeImage(
            imagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), ocrError);
        QFile::remove(imagePath);
        // 收益提示尚未出现时截图可能为空，UmiOCR 的 No text 在轮询阶段属于正常结果。
        if (!ocrError.isEmpty() && !ocrError.contains(QStringLiteral("No text found in image"), Qt::CaseInsensitive))
        {
            errorText = ocrError;
            return false;
        }
        return true;
    };
    const auto clickFound = [this, &errorText](const QRect& bounds) {
        const QPoint point = bounds.center();
        return input_.clickClientPoint(boundHwnd_, POINT{ point.x(), point.y() }, 0, errorText);
    };
    const auto findAndClick = [&findKeyword, &clickFound, &errorText, this](
        int x, int y, int width, int height, const QString& prefix,
        const QString& keyword, int maximumAttempts) {
        QRect bounds;
        for (int attempt = 1; attempt <= maximumAttempts; ++attempt)
        {
            if (automationCancelRequested_.load())
            {
                errorText = QStringLiteral("用户或服务端已强制停止当前动作");
                return false;
            }
            errorText.clear();
            if (findKeyword(x, y, width, height,
                QStringLiteral("%1_%2").arg(prefix).arg(attempt), keyword, bounds))
            {
                return clickFound(bounds);
            }
        }
        errorText = QStringLiteral("%1 次未找到“%2”").arg(maximumAttempts).arg(keyword);
        return false;
    };

    SetForegroundWindow(boundHwnd_);
    if (!findAndClick(173, 44, 1217, 60,
        QStringLiteral("mail_market"), QStringLiteral("交易行"), 3)
        || !waitForAutomation(1500, errorText))
    {
        return false;
    }
    if (!findAndClick(195, 125, 495, 46,
        QStringLiteral("mail_sell"), QStringLiteral("出售"), 3)
        || !waitForAutomation(1500, errorText))
    {
        return false;
    }

    if (!waitForListingQuantityReady(errorText, QStringLiteral("mail_revenue_quantity")))
        return false;
    if (!click(revenueClaimButton) || !waitForAutomation(2000, errorText))
        return false;

    QElapsedTimer completeTimer;
    completeTimer.start();
    bool completeFound = false;
    int pollIndex = 0;
    while (completeTimer.elapsed() < TimeoutMs)
    {
        if (automationCancelRequested_.load())
        {
            errorText = QStringLiteral("用户或服务端已强制停止当前动作");
            return false;
        }
        QString recognizedText;
        if (!captureText(1153, 152, 285, 375,
            QStringLiteral("mail_complete_%1").arg(++pollIndex), recognizedText))
        {
            errorText.clear();
        }
        if (recognizedText.simplified().contains(QStringLiteral("领取完成"), Qt::CaseInsensitive))
        {
            completeFound = true;
            break;
        }
        if (!waitForAutomation(PollIntervalMs, errorText))
            return false;
    }
    if (!completeFound)
    {
        errorText = QStringLiteral("5 秒内未找到“领取完成”");
        return false;
    }
    if (!waitForAutomation(1500, errorText)
        || !input_.pressKey(VK_SPACE, errorText)
        || !waitForAutomation(1500, errorText))
    {
        return false;
    }
    // 邮件领取完成后连续返回两层界面，两个 ESC 之间保留 1.5 秒。
    if (!input_.pressKey(VK_ESCAPE, errorText)
        || !waitForAutomation(1500, errorText)
        || !input_.pressKey(VK_ESCAPE, errorText)
        || !waitForAutomation(1500, errorText))
    {
        return false;
    }
    return true;
}

void ClientController::handleSelfThunderRequest(int requestId)
{
    if (!beginAutomationAction(QStringLiteral("自雷")))
    {
        connection_.sendSelfThunderResponse(requestId, false, QStringLiteral("已有自动化流程正在执行"));
        return;
    }

    struct ActionGuard
    {
        ClientController* controller = nullptr;
        ~ActionGuard()
        {
            if (controller)
            {
                controller->finishAutomationAction();
            }
        }
    } guard{ this };

    QString errorText;
    if (!executeSelfThunderSequence(errorText))
    {
        connection_.sendSelfThunderResponse(requestId, false, errorText);
        setStatus(QStringLiteral("自雷失败：%1").arg(errorText));
        return;
    }

    connection_.sendSelfThunderResponse(requestId, true, QStringLiteral("自雷完成"));
    setStatus(QStringLiteral("自雷流程已完成"));
}

bool ClientController::executeSelfThunderSequence(QString& errorText)
{
    if (!boundHwnd_ || !binder_.isValid(boundHwnd_))
    {
        errorText = QStringLiteral("未绑定有效游戏窗口");
        return false;
    }

    constexpr int ReferenceWidth = 2560;
    constexpr int ReferenceHeight = 1440;
    const POINT center{ ReferenceWidth / 2, ReferenceHeight / 2 };
    const POINT bottom{ ReferenceWidth / 2, ReferenceHeight - 1 };
    const POINT leaveButton{ 1273, 847 };
    const POINT confirmButton{ 1494, 895 };

    SetForegroundWindow(boundHwnd_);
    // 进入对局后先等待角色可操作，避免过早拖拽被游戏加载/保护阶段吞掉。
    QThread::msleep(8000);
    // 自雷流程所有坐标都按 2560x1440 客户区缩放，适配不同游戏窗口尺寸。
    if (!input_.moveReferencePointBezier(boundHwnd_, center, bottom, ReferenceWidth, ReferenceHeight, 1400, errorText))
    {
        return false;
    }

    if (!input_.pressKey(0x47, errorText))
    {
        return false;
    }
    QThread::msleep(2500);
    if (!input_.pressKey(0x47, errorText))
    {
        return false;
    }

    QThread::msleep(1000);
    if (!input_.pressKey(VK_ESCAPE, errorText))
    {
        return false;
    }

    QThread::msleep(5000);
    if (!input_.clickReferencePoint(boundHwnd_, leaveButton, ReferenceWidth, ReferenceHeight, 500, errorText))
    {
        return false;
    }

    QThread::msleep(5000);
    if (!input_.clickReferencePoint(boundHwnd_, confirmButton, ReferenceWidth, ReferenceHeight, 500, errorText))
    {
        return false;
    }

    QThread::msleep(15000);
    if (!input_.pressKey(VK_SPACE, errorText))
    {
        return false;
    }
    QThread::msleep(2000);
    if (!input_.pressKey(VK_SPACE, errorText))
    {
        return false;
    }
    QThread::msleep(2000);
    return input_.pressKey(VK_TAB, errorText);
}

bool ClientController::executeLoadoutSequence(const QString& loadoutType, QString& errorText)
{
    if (!boundHwnd_ || !binder_.isValid(boundHwnd_))
    {
        errorText = QStringLiteral("未绑定有效游戏窗口");
        return false;
    }

    constexpr int ReferenceWidth = 2560;
    constexpr int ReferenceHeight = 1440;
    const POINT openLoadout{ 2063, 1290 };
    const POINT normalOption{ 220, 569 };
    const POINT secretOption{ 216, 666 };
    const POINT firstConfirm{ 2263, 1165 };
    const POINT secondConfirm{ 2260, 1278 };
    const POINT finalConfirm{ 1282, 936 };
    const POINT typeOption = loadoutType == QStringLiteral("SECRET") ? secretOption : normalOption;

    SetForegroundWindow(boundHwnd_);
    // 配装流程坐标按 2560x1440 客户区参考缩放，普通/机密由服务端备注类型决定。
    if (!input_.clickReferencePoint(boundHwnd_, openLoadout, ReferenceWidth, ReferenceHeight, 500, errorText))
    {
        return false;
    }
    QThread::msleep(1000);
    if (!input_.pressKey('L', errorText))
    {
        return false;
    }
    QThread::msleep(1000);
    if (!input_.clickReferencePoint(boundHwnd_, typeOption, ReferenceWidth, ReferenceHeight, 500, errorText))
    {
        return false;
    }
    QThread::msleep(1000);
    if (!input_.clickReferencePoint(boundHwnd_, firstConfirm, ReferenceWidth, ReferenceHeight, 500, errorText))
    {
        return false;
    }
    QThread::msleep(1000);
    if (!input_.clickReferencePoint(boundHwnd_, secondConfirm, ReferenceWidth, ReferenceHeight, 500, errorText))
    {
        return false;
    }
    QThread::msleep(1000);
    return input_.clickReferencePoint(boundHwnd_, finalConfirm, ReferenceWidth, ReferenceHeight, 500, errorText);
}

bool ClientController::executeSaveLoadoutSequence(const QString& weapon, const QString& backpack, const QString& egg,
    const QString& bulletName, int perClick, int bulletTotal, QString& errorText)
{
    if (!boundHwnd_ || !binder_.isValid(boundHwnd_))
    {
        errorText = QStringLiteral("未绑定有效游戏窗口");
        return false;
    }
    if (weapon.trimmed().isEmpty() || backpack.trimmed().isEmpty() || egg.trimmed().isEmpty() || bulletName.trimmed().isEmpty())
    {
        errorText = QStringLiteral("配装文本参数不能为空");
        return false;
    }
    if (perClick <= 0 || bulletTotal <= 0 || bulletTotal % perClick != 0)
    {
        errorText = QStringLiteral("子弹总数必须能被单次数量整除");
        return false;
    }

    constexpr int ReferenceWidth = 2560;
    constexpr int ReferenceHeight = 1440;
    const auto click = [this, &errorText, ReferenceWidth, ReferenceHeight](POINT point) {
        return input_.clickReferencePoint(boundHwnd_, point, ReferenceWidth, ReferenceHeight, 0, errorText);
    };
    const auto rightClick = [this, &errorText, ReferenceWidth, ReferenceHeight](POINT point) {
        return input_.rightClickReferencePoint(boundHwnd_, point, ReferenceWidth, ReferenceHeight, 0, errorText);
    };
    const auto waitStep = [this, &errorText]() { return waitForAutomation(1500, errorText); };
    const auto stopIfRequested = [&]() {
        if (!automationCancelRequested_.load())
            return false;
        errorText = QStringLiteral("用户或服务端已强制停止当前动作");
        return true;
    };

    const auto clickFoundText = [&](const OcrRegionConfig& region, const QRect& bounds) {
        int clientWidth = 0;
        int clientHeight = 0;
        if (!binder_.clientSize(boundHwnd_, clientWidth, clientHeight)
            || clientWidth <= 0 || clientHeight <= 0)
        {
            errorText = QStringLiteral("读取游戏窗口客户区失败");
            return false;
        }
        const QRect scaledRegion = ScaleReferenceRect(
            QRect(region.x, region.y, region.width, region.height),
            QSize(region.referenceWidth, region.referenceHeight),
            QSize(clientWidth, clientHeight));
        const QPoint relativeCenter = bounds.center();
        if (!QRect(QPoint(0, 0), scaledRegion.size()).contains(relativeCenter))
        {
            errorText = QStringLiteral("OCR识别到的配装文本坐标超出截图区域");
            return false;
        }
        const POINT clickPoint{
            scaledRegion.x() + relativeCenter.x(),
            scaledRegion.y() + relativeCenter.y()
        };
        return input_.clickClientPoint(boundHwnd_, clickPoint, 0, errorText);
    };

    OcrRegionConfig listRegion;
    listRegion.enabled = true;
    listRegion.x = 119;
    listRegion.y = 246;
    listRegion.width = 460;
    listRegion.height = 1152;
    listRegion.referenceWidth = ReferenceWidth;
    listRegion.referenceHeight = ReferenceHeight;

    const auto findWithScroll = [&](const QString& keyword, const QString& filePrefix, QRect& bounds) {
        QElapsedTimer timer;
        timer.start();
        int attempt = 0;
        while (timer.elapsed() < 20000)
        {
            if (stopIfRequested())
                return false;
            QString ocrError;
            const QString imagePath = captureOcrRegion(listRegion,
                QStringLiteral("%1_%2").arg(filePrefix).arg(++attempt), ocrError);
            bool found = false;
            if (!imagePath.isEmpty())
            {
                found = umiOcrRunner_.findTextBounds(imagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), keyword, bounds, ocrError, true);
                QFile::remove(imagePath);
            }
            if (found)
                return true;
            if (!input_.scrollReferencePoint(boundHwnd_, POINT{ 342, 700 }, ReferenceWidth, ReferenceHeight, -WHEEL_DELTA * 5, errorText))
                return false;
            if (!waitForAutomation(150, errorText))
                return false;
        }
        errorText = QStringLiteral("20 秒内未找到“%1”").arg(keyword);
        return false;
    };

    SetForegroundWindow(boundHwnd_);
    OcrRegionConfig preparationRegion;
    preparationRegion.enabled = true;
    preparationRegion.x = 1946;
    preparationRegion.y = 1262;
    preparationRegion.width = 500;
    preparationRegion.height = 56;
    preparationRegion.referenceWidth = ReferenceWidth;
    preparationRegion.referenceHeight = ReferenceHeight;
    QString preparationOcrError;
    const QString preparationImagePath = captureOcrRegion(
        preparationRegion, QStringLiteral("save_loadout_preparation_state"), preparationOcrError);
    if (preparationImagePath.isEmpty())
    {
        errorText = preparationOcrError;
        return false;
    }
    const QString preparationText = umiOcrRunner_.recognizeImage(
        preparationImagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), preparationOcrError);
    QFile::remove(preparationImagePath);
    if (!preparationOcrError.isEmpty())
    {
        errorText = preparationOcrError;
        return false;
    }
    const QString normalizedPreparationText = preparationText.simplified();
    const bool isPreBattlePreparation = normalizedPreparationText.contains(QStringLiteral("行前备战"));
    const bool isLoadoutOrDeparture = normalizedPreparationText.contains(QStringLiteral("配装"))
        || normalizedPreparationText.contains(QStringLiteral("出发"));
    if (!isPreBattlePreparation && !isLoadoutOrDeparture)
    {
        errorText = QStringLiteral("未识别到“行前备战”“配装”或“出发”：%1").arg(normalizedPreparationText);
        return false;
    }

    if (stopIfRequested() || !click(POINT{ 2041, 1286 }) || !waitStep())
        return false;

    if (isPreBattlePreparation)
    {
        OcrRegionConfig startActionRegion;
        startActionRegion.enabled = true;
        startActionRegion.x = 2069;
        startActionRegion.y = 828;
        startActionRegion.width = 270;
        startActionRegion.height = 70;
        startActionRegion.referenceWidth = ReferenceWidth;
        startActionRegion.referenceHeight = ReferenceHeight;
        QElapsedTimer startActionTimer;
        startActionTimer.start();
        bool startActionFound = false;
        int startActionAttempt = 0;
        while (startActionTimer.elapsed() < 5000)
        {
            if (stopIfRequested())
                return false;
            QString ocrError;
            const QString imagePath = captureOcrRegion(startActionRegion,
                QStringLiteral("save_loadout_start_action_%1").arg(++startActionAttempt), ocrError);
            if (!imagePath.isEmpty())
            {
                QRect bounds;
                startActionFound = umiOcrRunner_.findTextBounds(
                    imagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(),
                    QStringLiteral("开始行动"), bounds, ocrError);
                QFile::remove(imagePath);
            }
            if (startActionFound)
                break;
            if (!waitForAutomation(150, errorText))
                return false;
        }

        if (!startActionFound
            && (stopIfRequested() || !click(POINT{ 1095, 352 }) || !waitStep()))
            return false;
    }

    if (isPreBattlePreparation && (stopIfRequested() || !click(POINT{ 2211, 856 }) || !waitStep()))
    {
        return false;
    }

    const POINT points[] = {
        { 501, 1400 }, { 199, 563 }, { 1228, 311 }
    };
    for (const POINT point : points)
    {
        if (stopIfRequested() || !click(point) || !waitStep())
            return false;
    }

    QRect bounds;
    if (!findWithScroll(weapon, QStringLiteral("save_loadout_weapon"), bounds)
        || stopIfRequested() || !waitStep() || !clickFoundText(listRegion, bounds)
        || !waitStep() || !click(POINT{ 2232, 1254 }) || !waitStep())
        return false;
    if (!click(POINT{ 1281, 1103 }) || !waitStep())
        return false;
    if (!findWithScroll(backpack, QStringLiteral("save_loadout_backpack"), bounds)
        || stopIfRequested() || !waitStep() || !clickFoundText(listRegion, bounds)
        || !waitStep() || !click(POINT{ 2232, 1254 }) || !waitStep())
        return false;
    if (!click(POINT{ 1302, 1232 }) || !waitStep())
        return false;
    if (!findWithScroll(egg, QStringLiteral("save_loadout_egg"), bounds)
        || stopIfRequested() || !waitStep() || !clickFoundText(listRegion, bounds)
        || !waitStep() || !click(POINT{ 2232, 1254 }) || !waitStep())
        return false;
    if (!click(POINT{ 625, 1268 }) || !waitStep())
        return false;

    // 进入弹药选择页后，先处理可能挡住子弹列表的“弹药穿透机制”提示。
    // 首次识别不到或 OCR 为空时直接继续；识别到后向下滚动 4 格并按空格，
    // 等待界面刷新后再次确认提示已经消失。
    {
        OcrRegionConfig penetrationRegion;
        penetrationRegion.enabled = true;
        penetrationRegion.x = 605;
        penetrationRegion.y = 219;
        penetrationRegion.width = 773;
        penetrationRegion.height = 104;
        penetrationRegion.referenceWidth = ReferenceWidth;
        penetrationRegion.referenceHeight = ReferenceHeight;

        QElapsedTimer penetrationTimer;
        penetrationTimer.start();
        int penetrationAttempt = 0;
        while (penetrationTimer.elapsed() < 20000)
        {
            if (stopIfRequested())
                return false;

            QString ocrError;
            const QString imagePath = captureOcrRegion(
                penetrationRegion,
                QStringLiteral("save_loadout_penetration_%1").arg(++penetrationAttempt),
                ocrError);
            bool foundPenetration = false;
            if (!imagePath.isEmpty())
            {
                QRect bounds;
                foundPenetration = umiOcrRunner_.findTextBounds(
                    imagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(),
                    QStringLiteral("弹药穿透机制"), bounds, ocrError, true);
                QFile::remove(imagePath);
            }

            const bool emptyOcr = ocrError.contains(QStringLiteral("No text found in image"), Qt::CaseInsensitive)
                || (imagePath.isEmpty() && ocrError.isEmpty());
            const bool normalNoMatch = ocrError.startsWith(QStringLiteral("OCR未找到"))
                || ocrError.startsWith(QStringLiteral("OCR未识别到文字"));
            if (!foundPenetration && !ocrError.isEmpty() && !emptyOcr && !normalNoMatch)
            {
                errorText = ocrError;
                return false;
            }
            if (!foundPenetration)
                break;

            for (int scrollIndex = 0; scrollIndex < 4; ++scrollIndex)
            {
                if (!input_.scrollReferencePoint(boundHwnd_, POINT{ 342, 700 },
                    ReferenceWidth, ReferenceHeight, -WHEEL_DELTA * 2, errorText)
                    || !waitForAutomation(500, errorText))
                    return false;
            }
            if (!input_.pressKey(VK_SPACE, errorText)
                || !waitForAutomation(1000, errorText))
                return false;
        }

        if (penetrationTimer.elapsed() >= 20000)
        {
            errorText = QStringLiteral("20 秒内未能关闭“弹药穿透机制”提示");
            return false;
        }
    }

    OcrRegionConfig bulletRegion;
    bulletRegion.enabled = true;
    bulletRegion.x = 1487;
    bulletRegion.y = 203;
    bulletRegion.width = 940;
    bulletRegion.height = 872;
    bulletRegion.referenceWidth = ReferenceWidth;
    bulletRegion.referenceHeight = ReferenceHeight;
    const int clickCount = bulletTotal / perClick;
    QElapsedTimer bulletFindTimer;
    bulletFindTimer.start();
    QRect bulletBounds;
    bool bulletFound = false;
    int bulletAttempt = 0;
    while (bulletFindTimer.elapsed() < 20000)
    {
        if (stopIfRequested())
            return false;
        QString ocrError;
        const QString imagePath = captureOcrRegion(bulletRegion,
            QStringLiteral("save_loadout_bullet_%1").arg(++bulletAttempt), ocrError);
        if (!imagePath.isEmpty())
        {
            bulletFound = umiOcrRunner_.findTextBounds(imagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), bulletName, bulletBounds, ocrError, true);
            QFile::remove(imagePath);
        }
        if (bulletFound)
        {
            break;
        }
        if (!input_.scrollReferencePoint(boundHwnd_, POINT{ 1930, 700 }, ReferenceWidth, ReferenceHeight,
            -WHEEL_DELTA * 5, errorText))
            return false;
        if (!waitForAutomation(150, errorText))
            return false;
    }
    if (!bulletFound)
    {
        errorText = QStringLiteral("20 秒内未找到子弹名称“%1”").arg(bulletName);
        return false;
    }

    int clientWidth = 0;
    int clientHeight = 0;
    if (!binder_.clientSize(boundHwnd_, clientWidth, clientHeight) || clientWidth <= 0 || clientHeight <= 0)
    {
        errorText = QStringLiteral("读取游戏窗口客户区失败");
        return false;
    }
    const QRect scaledRegion = ScaleReferenceRect(QRect(1487, 203, 940, 872), QSize(ReferenceWidth, ReferenceHeight), QSize(clientWidth, clientHeight));
    const POINT bulletPoint{ scaledRegion.x() + bulletBounds.center().x(), scaledRegion.y() + bulletBounds.center().y() };
    const auto randomXOffset = []() {
        const int magnitude = QRandomGenerator::global()->bounded(5, 11);
        return QRandomGenerator::global()->bounded(2) == 0 ? -magnitude : magnitude;
    };
    for (int index = 0; index < clickCount; ++index)
    {
        if (stopIfRequested())
            return false;
        const POINT clickPoint{ bulletPoint.x + randomXOffset(), bulletPoint.y };
        if (!input_.clickClientPoint(boundHwnd_, clickPoint, 0, errorText)
            || !waitForAutomation(300, errorText))
            return false;
    }

    if (!click(POINT{ 2229, 1268 }) || !waitStep()
        || !rightClick(POINT{ 1340, 332 }) || !waitStep()
        || !click(POINT{ 2254, 1264 }) || !waitStep()
        || !click(POINT{ 1284, 1002 }) || !waitStep()
        || !click(POINT{ 1383, 931 }) || !waitStep())
        return false;
    if (!input_.pressKey(VK_ESCAPE, errorText) || !waitStep()
        || !input_.pressKey(VK_ESCAPE, errorText) || !waitStep())
        return false;

    // 两次返回后检查是否仍停留在“配装方案”页面；若仍在该页面，
    // 再补一次 ESC 返回上一级。4 秒未找到时按正常完成继续。
    OcrRegionConfig loadoutPlanRegion;
    loadoutPlanRegion.enabled = true;
    loadoutPlanRegion.x = 483;
    loadoutPlanRegion.y = 1380;
    loadoutPlanRegion.width = 80;
    loadoutPlanRegion.height = 45;
    loadoutPlanRegion.referenceWidth = ReferenceWidth;
    loadoutPlanRegion.referenceHeight = ReferenceHeight;
    QElapsedTimer loadoutPlanTimer;
    loadoutPlanTimer.start();
    bool loadoutPlanFound = false;
    int loadoutPlanAttempt = 0;
    while (loadoutPlanTimer.elapsed() < 4000)
    {
        if (stopIfRequested())
            return false;
        QString ocrError;
        const QString imagePath = captureOcrRegion(
            loadoutPlanRegion,
            QStringLiteral("save_loadout_finish_plan_%1").arg(++loadoutPlanAttempt),
            ocrError);
        if (!imagePath.isEmpty())
        {
            QRect bounds;
            loadoutPlanFound = umiOcrRunner_.findTextBounds(
                imagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(),
                QStringLiteral("配装方案"), bounds, ocrError);
            QFile::remove(imagePath);
        }
        if (loadoutPlanFound)
            break;
        if (!waitForAutomation(150, errorText))
            return false;
    }
    if (loadoutPlanFound
        && (!input_.pressKey(VK_ESCAPE, errorText) || !waitStep()))
        return false;
    return true;
}

bool ClientController::executeListingSequence(bool hasExchange, int purchaseCount, bool loadoutMode, const QString& listingPrice, const QString& listingText,
    QString& ocrText, QString& usageResult, QString& errorText)
{
    Q_UNUSED(hasExchange);
    if (!boundHwnd_ || !binder_.isValid(boundHwnd_))
    {
        errorText = QStringLiteral("未绑定有效游戏窗口");
        return false;
    }
    if (listingPrice.trimmed().isEmpty())
    {
        errorText = QStringLiteral("上架价格为空");
        return false;
    }
    if (purchaseCount <= 0 || purchaseCount > 99)
    {
        errorText = QStringLiteral("购买次数必须在 1 到 99 之间");
        return false;
    }

    constexpr int ReferenceWidth = 2560;
    constexpr int ReferenceHeight = 1440;
    const POINT textInput{ 183, 232 };
    const POINT selectResult{ 786, 304 };
    const POINT noExchangeEntry{ 2323, 1111 };
    const POINT exchangeEntry{ 2324, 1032 };
    const POINT noExchangeOption{ 2200, 1223 };
    const POINT exchangeOption{ 2187, 1150 };
    const POINT priceArea{ 1695, 283 };
    const POINT pricePanelClick{ 1939, 745 };
    const POINT priceInput{ 1827, 858 };
    const POINT captchaConfirm{ 662, 1033 };

    Win32InputSequenceLock inputSequenceLock;
    if (!inputSequenceLock.acquire(errorText, 120000))
        return false;
    const auto click = [this, &errorText, ReferenceWidth, ReferenceHeight](POINT point) {
        return input_.clickReferencePoint(boundHwnd_, point, ReferenceWidth, ReferenceHeight, 0, errorText);
    };
    const auto stopIfRequested = [&]() {
        if (!automationCancelRequested_.load()) return false;
        errorText = QStringLiteral("用户或服务端已强制停止当前动作");
        return true;
    };

    const auto enterAndVerifyPrice = [&](const POINT& panelPoint, const POINT& inputPoint,
        const QString& price, const QString& filePrefix) {
        bool expectedOk = false;
        const double expectedPrice = price.trimmed().toDouble(&expectedOk);
        if (!expectedOk)
        {
            errorText = QStringLiteral("上架价格格式无效");
            return false;
        }
        OcrRegionConfig priceRegion;
        priceRegion.enabled = true;
        priceRegion.x = 1548; priceRegion.y = 837;
        priceRegion.width = 400; priceRegion.height = 50;
        priceRegion.referenceWidth = ReferenceWidth;
        priceRegion.referenceHeight = ReferenceHeight;
        const QRegularExpression numberPattern(QStringLiteral(R"(\d+(?:[,.]\d+)?)"));
        int attempt = 0;
        for (;;)
        {
            if (stopIfRequested()) return false;
            if (!click(panelPoint) || !waitForAutomation(1500, errorText)
                || !click(inputPoint) || !waitForAutomation(500, errorText)
                || !click(inputPoint)
                || !input_.pressKeyRepeated(VK_BACK, 5, 20, errorText)
                || !waitForAutomation(500, errorText)
                || !input_.pasteText(boundHwnd_, price.trimmed(), errorText)
                || !waitForAutomation(1500, errorText)) return false;
            QString ocrError;
            const QString imagePath = captureOcrRegion(
                priceRegion, QStringLiteral("%1_%2").arg(filePrefix).arg(++attempt), ocrError);
            QString recognized;
            if (!imagePath.isEmpty())
            {
                recognized = umiOcrRunner_.recognizeImage(
                    imagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), ocrError);
                QFile::remove(imagePath);
            }
            const QRegularExpressionMatch match = numberPattern.match(recognized);
            if (match.hasMatch())
            {
                QString normalized = match.captured(0);
                normalized.remove(',');
                bool actualOk = false;
                const double actualPrice = normalized.toDouble(&actualOk);
                if (actualOk && qAbs(actualPrice - expectedPrice) < 0.0001) return true;
            }
            if (!waitForAutomation(150, errorText)) return false;
        }
    };

    // 文本定位统一最多尝试 3 次，每次识别前等待 1.5 秒；成功后点击识别到的位置。
    const auto findAndClickText = [this, &errorText, &stopIfRequested, ReferenceWidth, ReferenceHeight](
        int x, int y, int width, int height, const QString& filePrefix, const QString& keyword) {
        OcrRegionConfig region;
        region.enabled = true;
        region.x = x;
        region.y = y;
        region.width = width;
        region.height = height;
        region.referenceWidth = ReferenceWidth;
        region.referenceHeight = ReferenceHeight;

        QString lastError;
        for (int attempt = 1; attempt <= 3; ++attempt)
        {
            if (attempt > 1 && !waitForAutomation(1500, errorText))
                return false;
            if (stopIfRequested())
                return false;

            QString ocrError;
            const QString imagePath = captureOcrRegion(
                region, QStringLiteral("%1_%2").arg(filePrefix).arg(attempt), ocrError);
            if (imagePath.isEmpty())
            {
                lastError = ocrError;
                continue;
            }

            QRect bounds;
            const bool found = umiOcrRunner_.findTextBounds(
                imagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), keyword, bounds, ocrError);
            QFile::remove(imagePath);
            if (found)
            {
                int clientWidth = 0;
                int clientHeight = 0;
                if (!binder_.clientSize(boundHwnd_, clientWidth, clientHeight)
                    || clientWidth <= 0 || clientHeight <= 0)
                {
                    errorText = QStringLiteral("读取游戏窗口客户区失败");
                    return false;
                }
                const QRect scaledRegion = ScaleReferenceRect(
                    QRect(x, y, width, height), QSize(ReferenceWidth, ReferenceHeight),
                    QSize(clientWidth, clientHeight));
                const QPoint relativeCenter = bounds.center();
                if (!QRect(QPoint(0, 0), scaledRegion.size()).contains(relativeCenter))
                {
                    errorText = QStringLiteral("OCR识别到的“%1”坐标超出截图区域").arg(keyword);
                    return false;
                }
                const POINT clickPoint{
                    scaledRegion.x() + relativeCenter.x(),
                    scaledRegion.y() + relativeCenter.y()
                };
                return input_.clickClientPoint(boundHwnd_, clickPoint, 0, errorText);
            }
            lastError = ocrError;
        }

        errorText = lastError.isEmpty()
            ? QStringLiteral("连续 3 次未识别到“%1”").arg(keyword)
            : QStringLiteral("连续 3 次未识别到“%1”：%2").arg(keyword, lastError);
        return false;
    };

    const auto findAndClickWithin = [this, &errorText, &stopIfRequested, ReferenceWidth, ReferenceHeight](
        int x, int y, int width, int height, const QString& filePrefix, const QString& keyword,
        int timeoutMs = 5000, int delayBeforeClickMs = 0) {
        OcrRegionConfig region;
        region.enabled = true;
        region.x = x;
        region.y = y;
        region.width = width;
        region.height = height;
        region.referenceWidth = ReferenceWidth;
        region.referenceHeight = ReferenceHeight;

        QElapsedTimer timer;
        timer.start();
        int attempt = 0;
        QString lastError;
        while (timer.elapsed() < timeoutMs)
        {
            if (stopIfRequested())
                return false;
            QString ocrError;
            const QString imagePath = captureOcrRegion(
                region, QStringLiteral("%1_%2").arg(filePrefix).arg(++attempt), ocrError);
            if (!imagePath.isEmpty())
            {
                QRect bounds;
                const bool found = umiOcrRunner_.findTextBounds(
                    imagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), keyword, bounds, ocrError);
                QFile::remove(imagePath);
                if (found)
                {
                    int clientWidth = 0;
                    int clientHeight = 0;
                    if (!binder_.clientSize(boundHwnd_, clientWidth, clientHeight)
                        || clientWidth <= 0 || clientHeight <= 0)
                    {
                        errorText = QStringLiteral("读取游戏窗口客户区失败");
                        return false;
                    }
                    const QRect scaledRegion = ScaleReferenceRect(
                        QRect(x, y, width, height), QSize(ReferenceWidth, ReferenceHeight),
                        QSize(clientWidth, clientHeight));
                    const QPoint relativeCenter = bounds.center();
                    if (!QRect(QPoint(0, 0), scaledRegion.size()).contains(relativeCenter))
                    {
                        errorText = QStringLiteral("OCR识别到的“%1”坐标超出截图区域").arg(keyword);
                        return false;
                    }
                    if (delayBeforeClickMs > 0
                        && !waitForAutomation(delayBeforeClickMs, errorText))
                        return false;
                    return input_.clickClientPoint(boundHwnd_, POINT{
                        scaledRegion.x() + relativeCenter.x(),
                        scaledRegion.y() + relativeCenter.y()
                    }, 0, errorText);
                }
            }
            lastError = ocrError;
            if (!waitForAutomation(150, errorText))
                return false;
        }
        errorText = lastError.isEmpty()
            ? QStringLiteral("%1 秒内未识别到“%2”").arg(timeoutMs / 1000.0, 0, 'f', 1).arg(keyword)
            : QStringLiteral("%1 秒内未识别到“%2”：%3").arg(timeoutMs / 1000.0, 0, 'f', 1).arg(keyword, lastError);
        return false;
    };

    SetForegroundWindow(boundHwnd_);

    if (loadoutMode)
    {
        const auto waitStep = [&]() { return waitForAutomation(1500, errorText); };
        const auto stopRequested = [&]() {
            if (!automationCancelRequested_.load())
                return false;
            errorText = QStringLiteral("用户或服务端已强制停止当前动作");
            return true;
        };
        const POINT loadoutPurchase{ 2269, 1147 };
        const POINT captchaConfirm{ 662, 1033 };

        // 先确认当前入口：只有“行前备战”需要点击 OCR 识别到的文本；“配装”直接进入后续流程。
        OcrRegionConfig preparationRegion;
        preparationRegion.enabled = true;
        preparationRegion.x = 1947;
        preparationRegion.y = 1261;
        preparationRegion.width = 500;
        preparationRegion.height = 55;
        preparationRegion.referenceWidth = ReferenceWidth;
        preparationRegion.referenceHeight = ReferenceHeight;
        QElapsedTimer preparationTimer;
        preparationTimer.start();
        bool preparationFound = false;
        bool loadoutEntryFound = false;
        int preparationAttempt = 0;
        while (preparationTimer.elapsed() < 3000 && !preparationFound && !loadoutEntryFound)
        {
            if (stopRequested()) return false;
            QString ocrError;
            const QString imagePath = captureOcrRegion(
                preparationRegion, QStringLiteral("loadout_preparation_entry_%1").arg(++preparationAttempt), ocrError);
            if (!imagePath.isEmpty())
            {
                const QString text = umiOcrRunner_.recognizeImage(
                    imagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), ocrError).simplified();
                QFile::remove(imagePath);
                preparationFound = text.contains(QStringLiteral("行前备战"), Qt::CaseInsensitive);
                loadoutEntryFound = text.contains(QStringLiteral("配装"), Qt::CaseInsensitive);
            }
            if (!ocrError.isEmpty() && !ocrError.contains(QStringLiteral("No text found in image"), Qt::CaseInsensitive))
            {
                errorText = ocrError;
                return false;
            }
            if (!preparationFound && !loadoutEntryFound && !waitForAutomation(150, errorText)) return false;
        }
        if (!preparationFound && !loadoutEntryFound)
        {
            errorText = QStringLiteral("3 秒内未识别到“行前备战”或“配装”");
            return false;
        }
        if (preparationFound)
        {
            emit connectionLogAppended(QStringLiteral("配装上架：确认配装后识别到行前备战，等待300ms后点击固定坐标"));
            if (!waitForAutomation(300, errorText)
                || !click(POINT{ 2039, 1282 }))
            {
                emit connectionLogAppended(QStringLiteral("配装上架：行前备战固定坐标点击失败：%1").arg(errorText));
                return false;
            }
            emit connectionLogAppended(QStringLiteral("配装上架：行前备战固定坐标点击完成，直接继续识别开始行动"));
        }

        // “开始行动”优先直接识别；未出现时先进入“零号大坝”，再重新识别。
        if (!findAndClickWithin(2069, 828, 270, 70,
            QStringLiteral("loadout_start_action"), QStringLiteral("开始行动"), 4000))
        {
            emit connectionLogAppended(QStringLiteral("配装上架：4秒内未找到开始行动，开始查找零号大坝"));
            errorText.clear();
            const bool zeroDamFound = findAndClickWithin(1004, 264, 200, 52,
                QStringLiteral("loadout_zero_dam"), QStringLiteral("零号大坝"), 4000);
            if (zeroDamFound)
            {
                errorText.clear();
                if (!findAndClickWithin(2069, 828, 270, 70,
                    QStringLiteral("loadout_start_action_retry"), QStringLiteral("开始行动"), 4000))
                    return false;
            }
            else
            {
                emit connectionLogAppended(QStringLiteral("配装上架：开始行动和零号大坝均未找到，回调行前备战"));
                // 全自动第二轮可能仍停留在“行前备战”界面：零号大坝和开始行动
                // 都未出现时，回调到行前备战重新点击，再重试开始行动。
                errorText.clear();
                if (!findAndClickWithin(1947, 1261, 500, 55,
                    QStringLiteral("loadout_preparation_entry_retry"), QStringLiteral("行前备战"), 3000))
                    return false;
                emit connectionLogAppended(QStringLiteral("配装上架：回调识别到行前备战，等待300ms后点击固定坐标"));
                if (!waitForAutomation(300, errorText)
                    || !click(POINT{ 2039, 1282 }))
                {
                    emit connectionLogAppended(QStringLiteral("配装上架：回调行前备战固定点击失败：%1").arg(errorText));
                    return false;
                }
                emit connectionLogAppended(QStringLiteral("配装上架：回调行前备战固定坐标点击完成，直接继续识别开始行动"));
                if (!findAndClickWithin(2069, 828, 270, 70,
                    QStringLiteral("loadout_start_action_after_preparation_retry"), QStringLiteral("开始行动"), 4000))
                    return false;
            }
        }

        // 配装方案入口和自定义方案都允许回调重试：自定义方案暂时未出现时，
        // 重新点击配装方案入口，再按相同的 4 秒 OCR 上限查找自定义方案。
        for (;;)
        {
            if (stopRequested())
                return false;
            errorText.clear();
            if (!findAndClickWithin(483, 1380, 80, 45,
                QStringLiteral("loadout_plan"), QStringLiteral("配装方案"), 4000))
                return false;
            errorText.clear();
            if (findAndClickWithin(114, 491, 145, 40,
                QStringLiteral("loadout_custom_plan"), QStringLiteral("自定义方案"), 4000))
                break;
            emit connectionLogAppended(QStringLiteral("配装上架：4秒内未找到自定义方案，回调重新点击配装方案"));
            errorText.clear();
        }
        const auto runCustomPlanPurchase = [&]() {
            if (!click(POINT{ 199, 563 }) || !waitForAutomation(1000, errorText))
                return false;
            if (stopRequested() || !click(loadoutPurchase) || !waitStep())
                return false;

            OcrRegionConfig captchaRegion;
            captchaRegion.enabled = true;
            captchaRegion.x = 1221;
            captchaRegion.y = 491;
            captchaRegion.width = 100;
            captchaRegion.height = 55;
            captchaRegion.referenceWidth = ReferenceWidth;
            captchaRegion.referenceHeight = ReferenceHeight;
            for (;;)
            {
                if (stopRequested())
                    return false;
                QString captchaError;
                const QString imagePath = captureOcrRegion(
                    captchaRegion, QStringLiteral("loadout_listing_captcha"), captchaError);
                if (imagePath.isEmpty())
                {
                    errorText = captchaError;
                    return false;
                }
                const QString captchaText = umiOcrRunner_.recognizeImage(
                    imagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), captchaError);
                QFile::remove(imagePath);
                const bool noText = captchaError.contains(
                    QStringLiteral("No text found in image"), Qt::CaseInsensitive);
                if (!captchaError.isEmpty() && !noText)
                {
                    errorText = captchaError;
                    return false;
                }
                if (noText || !captchaText.contains(QStringLiteral("验证码"), Qt::CaseInsensitive))
                    break;
                if (!click(captchaConfirm) || !waitForAutomation(600, errorText)
                    || !click(loadoutPurchase) || !waitStep())
                    return false;
            }

            // 购买后处理可能出现的“价格变动提醒”，确认后重新识别，直到提醒消失。
            OcrRegionConfig priceChangeRegion;
            priceChangeRegion.enabled = true;
            priceChangeRegion.x = 538;
            priceChangeRegion.y = 313;
            priceChangeRegion.width = 275;
            priceChangeRegion.height = 60;
            priceChangeRegion.referenceWidth = ReferenceWidth;
            priceChangeRegion.referenceHeight = ReferenceHeight;
            const POINT priceChangeConfirm{ 1280, 1009 };
            int priceChangeAttempt = 0;
            for (;;)
            {
                if (stopRequested())
                    return false;
                QString priceChangeError;
                const QString priceChangeImagePath = captureOcrRegion(
                    priceChangeRegion,
                    QStringLiteral("loadout_price_change_%1").arg(++priceChangeAttempt),
                    priceChangeError);
                if (priceChangeImagePath.isEmpty())
                {
                    if (priceChangeError.contains(QStringLiteral("No text found in image"), Qt::CaseInsensitive))
                        break;
                    errorText = priceChangeError;
                    return false;
                }
                QRect priceChangeBounds;
                const bool priceChangeFound = umiOcrRunner_.findTextBounds(
                    priceChangeImagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(),
                    QStringLiteral("价格变动提醒"), priceChangeBounds, priceChangeError);
                QRect schemeBounds;
                const bool schemeFound = umiOcrRunner_.findTextBounds(
                    priceChangeImagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(),
                    QStringLiteral("确认实现方案"), schemeBounds, priceChangeError);
                QFile::remove(priceChangeImagePath);
                if (!priceChangeFound && !schemeFound)
                    break;
                if (!click(priceChangeConfirm) || !waitStep())
                    return false;
            }
            return true;
        };

        if (!runCustomPlanPurchase())
            return false;

        // 配装上架的 OCR 文本成功后，先留出 300ms 让界面稳定，再点击识别到的文字中心。
        const auto navigateLoadoutFromTransferToSell = [&]() {
            if (!findAndClickWithin(394, 1380, 80, 45,
                QStringLiteral("loadout_transfer_all"), QStringLiteral("转移全部"), 4000, 300))
                return false;
            if (!findAndClickWithin(173, 44, 1217, 60,
                QStringLiteral("loadout_listing_market"), QStringLiteral("交易行"), 5000, 300))
                return false;
            if (!waitStep())
                return false;
            if (!findAndClickWithin(195, 125, 495, 46,
                QStringLiteral("loadout_listing_sell"), QStringLiteral("出售"), 5000, 300))
                return false;
            return waitStep();
        };

        const auto navigateLoadoutToSell = [&]() {
            if (!findAndClickWithin(173, 44, 1217, 60,
                QStringLiteral("loadout_listing_warehouse"), QStringLiteral("仓库"), 5000, 300))
                return false;
            return navigateLoadoutFromTransferToSell();
        };

        for (;;)
        {
            if (navigateLoadoutToSell())
                break;
            if (stopRequested())
                return false;
            if (!input_.pressKey(VK_ESCAPE, errorText)
                || !waitForAutomation(800, errorText))
                return false;
            emit connectionLogAppended(QStringLiteral("配装上架：未找到仓库，ESC后回调查找自定义方案"));
            errorText.clear();
            if (!findAndClickWithin(114, 491, 145, 40,
                QStringLiteral("loadout_custom_plan_warehouse_retry"), QStringLiteral("自定义方案"), 4000))
                return false;
            if (!runCustomPlanPurchase())
                return false;
            emit connectionLogAppended(QStringLiteral("配装上架：已从自定义方案重新购买，继续查找仓库"));
        }

        const POINT loadoutPriceArea{ 1693, 282 };
        const auto findLoadoutListingItem = [&]() {
            if (!click(loadoutPriceArea) || !waitStep())
                return false;

            OcrRegionConfig listingItemRegion;
            listingItemRegion.enabled = true;
            listingItemRegion.x = 540;
            listingItemRegion.y = 309;
            listingItemRegion.width = 190;
            listingItemRegion.height = 55;
            listingItemRegion.referenceWidth = ReferenceWidth;
            listingItemRegion.referenceHeight = ReferenceHeight;
            QElapsedTimer listingItemTimer;
            listingItemTimer.start();
            int listingAttempt = 0;
            QString listingItemError;
            while (listingItemTimer.elapsed() < 5000)
            {
                if (stopRequested())
                    return false;
                if (!click(loadoutPriceArea))
                    return false;
                QString ocrError;
                const QString imagePath = captureOcrRegion(
                    listingItemRegion, QStringLiteral("loadout_listing_item_%1").arg(++listingAttempt), ocrError);
                bool foundListingItem = false;
                if (!imagePath.isEmpty())
                {
                    QRect bounds;
                    foundListingItem = umiOcrRunner_.findTextBounds(
                        imagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(),
                        QStringLiteral("上架物品"), bounds, ocrError);
                    QFile::remove(imagePath);
                }
                if (foundListingItem)
                    return true;
                listingItemError = ocrError;
                if (!waitForAutomation(150, errorText))
                    return false;
            }
            errorText = listingItemError.isEmpty()
                ? QStringLiteral("5 秒内未识别到“上架物品”")
                : QStringLiteral("5 秒内未识别到“上架物品”：%1").arg(listingItemError);
            return false;
        };

        // 找不到“上架物品”时关闭当前面板，等待界面回稳，从“仓库”重新执行导航。
        for (;;)
        {
            if (findLoadoutListingItem())
                break;
            if (stopRequested())
                return false;
            if (!input_.pressKey(VK_ESCAPE, errorText)
                || !waitForAutomation(800, errorText))
                return false;
            emit connectionLogAppended(QStringLiteral("配装上架：5秒内未找到“上架物品”，ESC后等待800ms，从“仓库”重新尝试"));
            if (!navigateLoadoutToSell())
                return false;
        }


        if (!enterAndVerifyPrice(POINT{ 1939, 745 }, POINT{ 1827, 858 }, listingPrice,
            QStringLiteral("loadout_listing_price")))
        {
            return false;
        }
        ocrText = QStringLiteral("配装上架流程完成");
        usageResult = QStringLiteral("配装上架流程完成");
        return true;
    }

    // 上架流程按 2560x1440 客户区坐标缩放；整段持有本机输入锁，避免多个客户端抢鼠标键盘。
    if (!findAndClickText(173, 44, 1217, 60, QStringLiteral("listing_market"), QStringLiteral("交易行")))
        return false;
    if (!waitForAutomation(1500, errorText)) return false;
    if (!findAndClickText(195, 125, 495, 46, QStringLiteral("listing_buy"), QStringLiteral("购买")))
        return false;
    if (!waitForAutomation(1500, errorText)) return false;
    if (!click(textInput))
        return false;
    if (!waitForAutomation(1500, errorText)) return false;
    // 搜索框可能保留上一次内容，先全选并清空，再粘贴服务端下发的上架文本。
    if (!input_.pressKeyChord(VK_CONTROL, 'A', errorText))
    {
        return false;
    }
    if (!waitForAutomation(1500, errorText)) return false;
    if (!input_.pressKey(VK_BACK, errorText) || !input_.pasteText(boundHwnd_, listingText, errorText))
    {
        return false;
    }
    if (!waitForAutomation(1500, errorText)) return false;

    if (!click(selectResult))
    {
        return false;
    }
    if (!waitForAutomation(1500, errorText)) return false;

    // 不再直接相信服务端传入的兑换类型；以搜索结果后的界面 OCR 为准，连续判断两次。
    OcrRegionConfig exchangeRegion;
    exchangeRegion.enabled = true;
    exchangeRegion.x = 1911;
    exchangeRegion.y = 953;
    exchangeRegion.width = 530;
    exchangeRegion.height = 360;
    exchangeRegion.referenceWidth = ReferenceWidth;
    exchangeRegion.referenceHeight = ReferenceHeight;

    bool exchangeDetected[2]{};
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        if (stopIfRequested())
        {
            return false;
        }
        QString exchangeOcrError;
        const QString exchangeImagePath = captureOcrRegion(
            exchangeRegion, QStringLiteral("listing_exchange_%1").arg(attempt + 1), exchangeOcrError);
        if (exchangeImagePath.isEmpty())
        {
            errorText = exchangeOcrError;
            return false;
        }

        QRect exchangeTextBounds;
        exchangeDetected[attempt] = umiOcrRunner_.findTextBounds(
            exchangeImagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), QStringLiteral("兑换"),
            exchangeTextBounds, exchangeOcrError);
        QFile::remove(exchangeImagePath);
        if (stopIfRequested())
        {
            return false;
        }

        if (attempt == 0)
        {
            if (!waitForAutomation(1500, errorText)) return false;
        }
    }

    if (exchangeDetected[0] != exchangeDetected[1])
    {
        errorText = QStringLiteral("兑换状态 OCR 两次结果不一致");
        return false;
    }

    const bool detectedExchange = exchangeDetected[0];
    if (!click(detectedExchange ? exchangeEntry : noExchangeEntry))
    {
        return false;
    }
    if (!waitForAutomation(1500, errorText)) return false;
    if (stopIfRequested())
    {
        return false;
    }
    const POINT purchasePoint = detectedExchange ? exchangeOption : noExchangeOption;
    OcrRegionConfig captchaRegion;
    captchaRegion.enabled = true;
    captchaRegion.x = 1221;
    captchaRegion.y = 491;
    captchaRegion.width = 100;
    captchaRegion.height = 55;
    captchaRegion.referenceWidth = ReferenceWidth;
    captchaRegion.referenceHeight = ReferenceHeight;
    int completedPurchaseCount = 0;
    while (completedPurchaseCount < purchaseCount)
    {
        if (automationCancelRequested_)
        {
            errorText = QStringLiteral("用户或服务端已强制停止当前动作");
            return false;
        }
        if (!click(purchasePoint))
        {
            return false;
        }
        if (!waitForAutomation(1500, errorText)) return false;

        QString captchaOcrError;
        const QString captchaImagePath = captureOcrRegion(captchaRegion, QStringLiteral("listing_captcha"), captchaOcrError);
        if (captchaImagePath.isEmpty())
        {
            errorText = captchaOcrError;
            return false;
        }
        const QString captchaText = umiOcrRunner_.recognizeImage(
            captchaImagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), captchaOcrError);
        QFile::remove(captchaImagePath);
        // 验证码区域在正常购买时通常完全空白；UmiOCR 会把这种情况作为 No text 错误返回。
        // 此处仅将“无文字”解释为“未出现验证码”，其他 OCR 故障仍中止流程。
        const bool captchaRegionHasNoText = captchaOcrError.contains(
            QStringLiteral("No text found in image"), Qt::CaseInsensitive);
        if (!captchaOcrError.isEmpty() && !captchaRegionHasNoText)
        {
            errorText = captchaOcrError;
            return false;
        }
        const bool hasCaptcha = !captchaRegionHasNoText
            && captchaText.contains(QStringLiteral("验证码"), Qt::CaseInsensitive);
        if (hasCaptcha)
        {
            if (!click(captchaConfirm))
            {
                return false;
            }
            // 验证码确认后界面恢复较快，按要求仅等待约 600ms，再重新购买。
            if (!waitForAutomation(600, errorText)) return false;
            continue;
        }
        ++completedPurchaseCount;
    }
    if (!waitForAutomation(1500, errorText)) return false;
    if (stopIfRequested())
    {
        return false;
    }

    if (!input_.pressKey(VK_ESCAPE, errorText))
    {
        return false;
    }
    if (!waitForAutomation(1500, errorText)) return false;
    if (stopIfRequested())
    {
        return false;
    }
    // 返回搜索结果后，先在购买/出售标签区域识别并点击“出售”。
    if (!findAndClickText(195, 125, 495, 46, QStringLiteral("listing_sell_tab"), QStringLiteral("出售")))
        return false;
    if (!waitForAutomation(1500, errorText)) return false;
    if (stopIfRequested())
        return false;

    // 点击价格区域后，最多 3 次识别“上架物品”，每次识别前等待 1.5 秒。
    bool foundListingItem = false;
    for (int attempt = 1; attempt <= 3; ++attempt)
    {
        if (!click(priceArea))
            return false;
        if (!waitForAutomation(1500, errorText)) return false;
        if (stopIfRequested())
            return false;

        OcrRegionConfig listingItemRegion;
        listingItemRegion.enabled = true;
        listingItemRegion.x = 540;
        listingItemRegion.y = 309;
        listingItemRegion.width = 190;
        listingItemRegion.height = 55;
        listingItemRegion.referenceWidth = ReferenceWidth;
        listingItemRegion.referenceHeight = ReferenceHeight;
        QString itemOcrError;
        const QString itemImagePath = captureOcrRegion(
            listingItemRegion, QStringLiteral("listing_item_%1").arg(attempt), itemOcrError);
        if (!itemImagePath.isEmpty())
        {
            QRect itemBounds;
            foundListingItem = umiOcrRunner_.findTextBounds(
                itemImagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(),
                QStringLiteral("上架物品"), itemBounds, itemOcrError);
            QFile::remove(itemImagePath);
        }
        if (foundListingItem)
            break;
        if (attempt == 3)
        {
            errorText = itemOcrError.isEmpty()
                ? QStringLiteral("连续 3 次未识别到“上架物品”")
                : QStringLiteral("连续 3 次未识别到“上架物品”：%1").arg(itemOcrError);
            return false;
        }
    }

    if (!enterAndVerifyPrice(pricePanelClick, priceInput, listingPrice,
        QStringLiteral("listing_price")))
        return false;

    OcrRegionConfig region;
    region.enabled = true;
    region.x = 1448;
    region.y = 368;
    region.width = 630;
    region.height = 700;
    region.referenceWidth = ReferenceWidth;
    region.referenceHeight = ReferenceHeight;

    const QString imagePath = captureOcrRegion(region, QStringLiteral("listing_result"), errorText);
    if (imagePath.isEmpty())
    {
        return false;
    }

    ocrText = umiOcrRunner_.recognizeImage(imagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), errorText);
    QFile::remove(imagePath);
    if (stopIfRequested())
    {
        return false;
    }
    if (!errorText.isEmpty())
    {
        return false;
    }

    // 最后核对出售数量区域，参考客户区坐标为 (1477,664)，大小 323×55。
    OcrRegionConfig usageRegion;
    usageRegion.enabled = true;
    usageRegion.x = 1477;
    usageRegion.y = 664;
    usageRegion.width = 323;
    usageRegion.height = 55;
    usageRegion.referenceWidth = ReferenceWidth;
    usageRegion.referenceHeight = ReferenceHeight;
    const QString usageImagePath = captureOcrRegion(usageRegion, QStringLiteral("listing_usage"), errorText);
    if (usageImagePath.isEmpty())
    {
        return false;
    }
    const QString usageText = umiOcrRunner_.recognizeImage(
        usageImagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), errorText);
    QFile::remove(usageImagePath);
    if (stopIfRequested())
    {
        return false;
    }
    if (!errorText.isEmpty())
    {
        return false;
    }

    const QRegularExpression usagePattern(QStringLiteral(R"((\d+)\s*/\s*(\d+))"));
    const QRegularExpressionMatch usageMatch = usagePattern.match(usageText);
    if (!usageMatch.hasMatch())
    {
        errorText = QStringLiteral("使用量 OCR 未识别到 X/N：%1").arg(usageText.trimmed());
        return false;
    }
    const int usedCount = usageMatch.captured(1).toInt();
    const int totalCount = usageMatch.captured(2).toInt();
    const int expectedCount = purchaseCount * 200;
    // 本轮期望值只由购买次数决定，X/N 中只核对 X；N 仅原样记录，便于排查 OCR 结果。
    if (usedCount > expectedCount)
    {
        usageResult = QStringLiteral("使用量超出：%1/%2，期望 %3").arg(usedCount).arg(totalCount).arg(expectedCount);
    }
    else if (usedCount == expectedCount)
    {
        usageResult = QStringLiteral("使用量核对成功：%1/%2").arg(usedCount).arg(totalCount);
    }
    else
    {
        usageResult = QStringLiteral("使用量少：%1/%2，期望 %3").arg(usedCount).arg(totalCount).arg(expectedCount);
    }
    return true;
}

QString ClientController::captureOcrRegion(const OcrRegionConfig& region, const QString& filePrefix, QString& errorText) const
{
    if (!boundHwnd_ || !binder_.isValid(boundHwnd_))
    {
        errorText = QStringLiteral("未绑定有效游戏窗口");
        return {};
    }

    int clientWidth = 0;
    int clientHeight = 0;
    if (!binder_.clientSize(boundHwnd_, clientWidth, clientHeight) || clientWidth <= 0 || clientHeight <= 0)
    {
        errorText = QStringLiteral("读取游戏窗口客户区失败");
        return {};
    }

    const QRect scaledRegion = ScaleReferenceRect(
        QRect(region.x, region.y, region.width, region.height),
        QSize(region.referenceWidth, region.referenceHeight),
        QSize(clientWidth, clientHeight));
    if (!IsRectInsideClient(scaledRegion, QSize(clientWidth, clientHeight)))
    {
        errorText = QStringLiteral("OCR 截图区域超出当前游戏客户区");
        return {};
    }

    POINT screenPoint{scaledRegion.x(), scaledRegion.y()};
    if (!ClientToScreen(boundHwnd_, &screenPoint))
    {
        errorText = QStringLiteral("客户区坐标转屏幕坐标失败");
        return {};
    }
    QScreen* screen = QGuiApplication::primaryScreen();
    if (!screen)
    {
        errorText = QStringLiteral("未找到屏幕对象");
        return {};
    }
    const QPixmap pixmap = screen->grabWindow(0, screenPoint.x, screenPoint.y, scaledRegion.width(), scaledRegion.height());
    if (pixmap.isNull())
    {
        errorText = QStringLiteral("截图失败");
        return {};
    }

    const QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    const QString imagePath = QDir(tempDir).filePath(QStringLiteral("df_playclick_%1_%2.png").arg(filePrefix).arg(reinterpret_cast<quintptr>(boundHwnd_)));
    if (!pixmap.save(imagePath, "PNG"))
    {
        errorText = QStringLiteral("保存OCR截图失败");
        return {};
    }
    return imagePath;
}

bool ClientController::sendTeamReadyResult(const TeamReadyResult& result)
{
    if (!connection_.sendTeamReady(result.state, result.current, result.total))
    {
        return false;
    }

    lastTeamReadyState_ = result.state;
    lastTeamReadyCurrent_ = result.current;
    lastTeamReadyTotal_ = result.total;
    return true;
}
