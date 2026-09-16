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
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QScreen>
#include <QStandardPaths>
#include <QThread>

#include <cmath>

#include "../DF_PlayClick_Client/ClientBindingRules.h"
#include "../DF_PlayClick_Client/ClientCountdownRules.h"
#include "../Shared/Protocol.h"

namespace
{
const UINT WM_DF_PLAYCLICK_DELAY_CHANGED = RegisterWindowMessageW(L"DF_PlayClick_ClickQueueDelayChanged");
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
    connect(&connection_, &ClientConnection::rebindWindowRequestReceived, this, &ClientController::rebindGameWindow);
    connect(&connection_, &ClientConnection::forceStopActionRequestReceived, this, &ClientController::forceStopCurrentAction);
    connect(&connection_, &ClientConnection::forceStopActionImmediate, this, [this]() {
        automationCancelRequested_.store(true);
    }, Qt::DirectConnection);
    connect(&connection_, &ClientConnection::ocrRequestReceived, this, &ClientController::handleOcrRequest);
    connect(&connection_, &ClientConnection::selfThunderRequestReceived, this, &ClientController::handleSelfThunderRequest);
    connect(&connection_, &ClientConnection::loadoutRequestReceived, this, &ClientController::handleLoadoutRequest);
    connect(&connection_, &ClientConnection::listingRequestReceived, this, &ClientController::handleListingRequest);
    connect(&connection_, &ClientConnection::mailRequestReceived, this, &ClientController::handleMailRequest);
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

void ClientController::startCountdown(int seconds)
{
    scheduledTriggerTimer_.stop();
    countdownTimer_.stop();
    countdown_ = seconds;
    if (ShouldClickImmediatelyForCountdown(seconds))
    {
        emit countdownChanged(QStringLiteral("倒计时：点击"));
        clickEnabledTasks();
        return;
    }

    emit countdownChanged(QStringLiteral("倒计时：%1").arg(countdown_));
    countdownTimer_.start();
}

void ClientController::startScheduledTrigger(int seconds, qint64 targetClientMs)
{
    countdownTimer_.stop();
    scheduledTriggerTimer_.stop();
    scheduledTriggerSeconds_ = seconds;
    scheduledTriggerTargetMs_ = targetClientMs;
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
    // 出发点不接受用户取点，只按角色使用 2560x1440 固定坐标并适配当前客户区。
    if (!input_.clickReferencePoint(boundHwnd_, point, 2560, 1440, clickQueueDelayMs_, errorText))
    {
        setStatus(errorText);
        emit messageRequested(QStringLiteral("提示"), errorText, false);
        return;
    }

    const QString roleText = QString::fromStdWString(ClientRoleUtil::ToDisplayText(currentRole_));
    setStatus(QStringLiteral("%1固定坐标点击完成").arg(roleText));
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

    // OCR 只响应服务端的一次请求，不再连接后自动轮询。
    const QString imagePath = captureOcrRegion(region, QStringLiteral("team_ready"), errorText);
    if (!imagePath.isEmpty())
    {
        const QString ocrText = umiOcrRunner_.recognizeImage(imagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), errorText);
        QFile::remove(imagePath);
        if (!ocrText.isEmpty())
        {
            result = teamReadyParser_.parse(ocrText);
        }
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

void ClientController::handleListingRequest(int requestId, bool hasExchange, int purchaseCount, const QString& listingPrice, const QString& listingText)
{
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
    if (!executeListingSequence(hasExchange, purchaseCount, listingPrice, listingText, ocrText, usageResult, errorText))
    {
        connection_.sendListingResponse(requestId, false, errorText);
        setStatus(QStringLiteral("上架失败：%1").arg(errorText));
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

bool ClientController::executeMailSequence(QString& errorText)
{
    if (!boundHwnd_ || !binder_.isValid(boundHwnd_))
    {
        errorText = QStringLiteral("未绑定有效游戏窗口");
        return false;
    }

    constexpr int ReferenceWidth = 2560;
    constexpr int ReferenceHeight = 1440;
    const POINT mailButton{ 2322, 68 };
    const auto click = [this, &errorText](POINT point) {
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
    const auto clickFound = [this, &errorText](const QRect& bounds) {
        const QPoint point = bounds.center();
        return input_.clickClientPoint(boundHwnd_, POINT{ point.x(), point.y() }, 0, errorText);
    };

    SetForegroundWindow(boundHwnd_);
    if (!input_.moveReferencePoint(boundHwnd_, mailButton, ReferenceWidth, ReferenceHeight, errorText)
        || !waitForAutomation(1500, errorText) || !click(mailButton)
        || !waitForAutomation(1500, errorText))
    {
        return false;
    }

    QRect bounds;
    bool found = false;
    for (int attempt = 0; attempt < 3 && !found; ++attempt)
    {
        if (automationCancelRequested_.load()) { errorText = QStringLiteral("用户或服务端已强制停止当前动作"); return false; }
        found = findKeyword(124, 1245, 260, 55, QStringLiteral("mail_claim_all_%1").arg(attempt + 1), QStringLiteral("全部领取"), bounds);
    }
    if (!found) { errorText = QStringLiteral("3 次未找到“全部领取”"); return false; }
    if (!waitForAutomation(1500, errorText) || !clickFound(bounds) || !waitForAutomation(1500, errorText)) return false;

    bounds = {};
    found = findKeyword(1156, 302, 290, 70, QStringLiteral("mail_complete"), QStringLiteral("领取完成"), bounds);
    if (!found) { errorText = QStringLiteral("未找到“领取完成”"); return false; }
    if (!waitForAutomation(1500, errorText) || !input_.pressKey(VK_SPACE, errorText) || !waitForAutomation(1500, errorText)) return false;

    bounds = {};
    found = false;
    for (int attempt = 0; attempt < 3 && !found; ++attempt)
    {
        if (automationCancelRequested_.load()) { errorText = QStringLiteral("用户或服务端已强制停止当前动作"); return false; }
        found = findKeyword(395, 1243, 260, 55, QStringLiteral("mail_delete_all_%1").arg(attempt + 1), QStringLiteral("删除已读"), bounds);
    }
    if (!found) { errorText = QStringLiteral("3 次未找到“删除已读”"); return false; }
    if (!waitForAutomation(1500, errorText) || !clickFound(bounds) || !waitForAutomation(1500, errorText)) return false;

    bounds = {};
    found = findKeyword(1295, 864, 400, 75, QStringLiteral("mail_delete_confirm"), QStringLiteral("确认"), bounds);
    if (!found) { errorText = QStringLiteral("未找到“确认”"); return false; }
    if (!waitForAutomation(1500, errorText) || !clickFound(bounds) || !waitForAutomation(1500, errorText)) return false;
    return input_.pressKey(VK_ESCAPE, errorText);
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

bool ClientController::executeListingSequence(bool hasExchange, int purchaseCount, const QString& listingPrice, const QString& listingText,
    QString& ocrText, QString& usageResult, QString& errorText)
{
    Q_UNUSED(hasExchange);
    if (!boundHwnd_ || !binder_.isValid(boundHwnd_))
    {
        errorText = QStringLiteral("未绑定有效游戏窗口");
        return false;
    }
    if (listingText.trimmed().isEmpty() || listingPrice.trimmed().isEmpty())
    {
        errorText = QStringLiteral("上架文本或上架价格为空");
        return false;
    }
    if (purchaseCount <= 0 || purchaseCount > 99)
    {
        errorText = QStringLiteral("购买次数必须在 1 到 99 之间");
        return false;
    }

    constexpr int ReferenceWidth = 2560;
    constexpr int ReferenceHeight = 1440;
    const POINT openSearch{ 946, 64 };
    const POINT textInput{ 183, 232 };
    const POINT selectResult{ 786, 304 };
    const POINT noExchangeEntry{ 2323, 1111 };
    const POINT exchangeEntry{ 2324, 1032 };
    const POINT noExchangeOption{ 2200, 1223 };
    const POINT exchangeOption{ 2187, 1150 };
    const POINT returnButton{ 429, 72 };
    const POINT priceArea{ 1686, 214 };
    const POINT firstPriceClick{ 1880, 934 };
    const POINT pricePanelClick{ 1939, 745 };
    const POINT priceInput{ 1827, 858 };
    const POINT captchaConfirm{ 662, 1033 };

    Win32InputSequenceLock inputSequenceLock;
    if (!inputSequenceLock.acquire(errorText, 120000))
    {
        return false;
    }

    const auto click = [this, &errorText, ReferenceWidth, ReferenceHeight](POINT point) {
        return input_.clickReferencePoint(boundHwnd_, point, ReferenceWidth, ReferenceHeight, 0, errorText);
    };
    const auto stopIfRequested = [&]() {
        if (!automationCancelRequested_.load())
        {
            return false;
        }
        errorText = QStringLiteral("用户或服务端已强制停止当前动作");
        return true;
    };

    SetForegroundWindow(boundHwnd_);
    // 上架流程按 2560x1440 客户区坐标缩放；整段持有本机输入锁，避免多个客户端抢鼠标键盘。
    if (!click(openSearch))
    {
        return false;
    }
    if (!waitForAutomation(1500, errorText)) return false;

    if (!click(textInput))
    {
        return false;
    }
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
    if (!click(returnButton))
    {
        return false;
    }

    if (!input_.moveReferencePoint(boundHwnd_, priceArea, ReferenceWidth, ReferenceHeight, errorText))
    {
        return false;
    }
    if (!waitForAutomation(1500, errorText)) return false;
    if (stopIfRequested())
    {
        return false;
    }
    // “出售”OCR区域以2560×1440客户区为基准，截图时按实际客户区尺寸等比例适配。
    OcrRegionConfig sellRegion;
    sellRegion.enabled = true;
    sellRegion.x = 0;
    sellRegion.y = 0;
    sellRegion.width = 1650;
    sellRegion.height = 1440;
    sellRegion.referenceWidth = ReferenceWidth;
    sellRegion.referenceHeight = ReferenceHeight;

    QRect sellTextBounds;
    bool foundSell = false;
    QString lastSellError;
    constexpr int SellRecognitionAttempts = 3;
    for (int attempt = 1; attempt <= SellRecognitionAttempts; ++attempt)
    {
        // 每轮都重新点击价格区域，再截图识别“出售”；任意一轮成功后立即继续后续流程。
        if (!click(priceArea))
        {
            return false;
        }
        if (!waitForAutomation(1500, errorText)) return false;
        if (stopIfRequested())
        {
            return false;
        }

        errorText.clear();
        const QString sellImagePath = captureOcrRegion(sellRegion, QStringLiteral("listing_sell"), errorText);
        if (!sellImagePath.isEmpty())
        {
            foundSell = umiOcrRunner_.findTextBounds(sellImagePath, config_.loadUmiOcrPath(), config_.loadUmiOcrPort(), QStringLiteral("出售"),
                sellTextBounds, errorText);
            QFile::remove(sellImagePath);
        }
        if (foundSell)
        {
            break;
        }
        lastSellError = errorText;
    }
    if (!foundSell)
    {
        errorText = lastSellError.isEmpty()
            ? QStringLiteral("连续 3 次未识别到“出售”")
            : QStringLiteral("连续 3 次未识别到“出售”：%1").arg(lastSellError);
        return false;
    }

    int clientWidth = 0;
    int clientHeight = 0;
    if (!binder_.clientSize(boundHwnd_, clientWidth, clientHeight) || clientWidth <= 0 || clientHeight <= 0)
    {
        errorText = QStringLiteral("读取游戏窗口客户区失败");
        return false;
    }
    const QRect scaledSellRegion = ScaleReferenceRect(
        QRect(sellRegion.x, sellRegion.y, sellRegion.width, sellRegion.height),
        QSize(ReferenceWidth, ReferenceHeight), QSize(clientWidth, clientHeight));
    const QPoint relativeSellCenter = sellTextBounds.center();
    if (!QRect(QPoint(0, 0), scaledSellRegion.size()).contains(relativeSellCenter))
    {
        errorText = QStringLiteral("OCR识别到的“出售”坐标超出截图区域");
        return false;
    }
    // UmiOCR返回的是截图内相对坐标，需要叠加缩放后截图区域的左上角得到客户区坐标。
    const POINT sellClickPoint{
        scaledSellRegion.x() + relativeSellCenter.x(),
        scaledSellRegion.y() + relativeSellCenter.y()
    };
    if (!input_.clickClientPoint(boundHwnd_, sellClickPoint, 0, errorText))
    {
        return false;
    }
    if (!waitForAutomation(2000, errorText)) return false;
    if (stopIfRequested())
    {
        return false;
    }

    if (!click(firstPriceClick))
    {
        return false;
    }
    // 首次点击后等待出售界面稳定，再点击价格面板并等待控件展开。
    if (!waitForAutomation(2000, errorText)) return false;
    if (stopIfRequested())
    {
        return false;
    }
    if (!click(pricePanelClick))
    {
        return false;
    }
    if (!waitForAutomation(1500, errorText)) return false;
    if (stopIfRequested())
    {
        return false;
    }
    if (!click(priceInput))
    {
        return false;
    }
    // 点击价格输入框后等待焦点稳定，再清空并输入新价格。
    if (!waitForAutomation(1500, errorText)) return false;
    if (stopIfRequested())
    {
        return false;
    }
    if (!input_.pressKeyRepeated(VK_BACK, 5, 20, errorText))
    {
        return false;
    }
    // 清空旧价格后必须立即粘贴服务端下发的上架价格，再进行最终 OCR 判断。
    if (!input_.pasteText(boundHwnd_, listingPrice, errorText))
    {
        return false;
    }
    if (!waitForAutomation(1500, errorText)) return false;
    if (stopIfRequested())
    {
        return false;
    }

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

    POINT screenPoint{ scaledRegion.x(), scaledRegion.y() };
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

    // OCR 区域按参考客户区坐标缩放后从屏幕截图，后续只识别这块小图。
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
