#include "ServerController.h"

#include <algorithm>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QList>
#include <QSet>
#include <QStringList>
#include <QThread>
#include <QFile>
#include <QTextStream>

#include "AutoStartOcrRules.h"
#include "../Shared/ServerRules.h"
#include "../Shared/Protocol.h"

namespace
{
    constexpr qint64 AutoStartAfterTriggerDelayMs = 1000;
    constexpr qint64 AutoStartMatchingRescueDelayMs = 1500;
    constexpr int AutoStartReferenceWidth = 2560;
    constexpr int AutoStartReferenceHeight = 1440;

    const OcrRequestRegion ActionRegion{ 1945, 1261, 500, 60, AutoStartReferenceWidth, AutoStartReferenceHeight };
    const OcrRequestRegion MapTypeRegion{ 0, 0, 300, 60, AutoStartReferenceWidth, AutoStartReferenceHeight };

    QString AccountCredentialKey(const QString& username, const QString& password)
    {
        return username.trimmed() + QChar(0x1f) + password.trimmed();
    }

    QSet<QString> LoadBannedAccountKeys()
    {
        QSet<QString> keys;
        const QString filePath = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("封号.txt"));
        QFile file(filePath);
        if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            return keys;
        }

        QTextStream stream(&file);
        while (!stream.atEnd())
        {
            const QString line = stream.readLine().trimmed();
            const int separator = line.indexOf(QStringLiteral("----"));
            if (separator <= 0 || separator + 4 >= line.size())
            {
                continue;
            }
            const QString username = line.left(separator).trimmed();
            const QString password = line.mid(separator + 4).trimmed();
            if (!username.isEmpty() && !password.isEmpty())
            {
                keys.insert(AccountCredentialKey(username, password));
            }
        }
        return keys;
    }
}

ServerController::ServerController(QObject* parent)
    : QObject(parent)
{
    autoStartTimer_.setInterval(100);
    compensationSyncTimer_.setInterval(25);
    autoExecuteTimer_.setInterval(200);
    connect(&autoStartTimer_, &QTimer::timeout, this, &ServerController::processAutoStartTick);
    connect(&compensationSyncTimer_, &QTimer::timeout, this, &ServerController::processCompensationSync);
    connect(&autoExecuteTimer_, &QTimer::timeout, this, &ServerController::processAutoExecuteTick);
    connect(&connectionManager_, &ServerConnectionManager::ocrResponseReceived, this, &ServerController::handleAutoStartOcrResponse);
    connect(&connectionManager_, &ServerConnectionManager::selfThunderResponseReceived, this, &ServerController::handleSelfThunderResponse);
    connect(&connectionManager_, &ServerConnectionManager::loadoutResponseReceived, this, &ServerController::handleLoadoutResponse);
    connect(&connectionManager_, &ServerConnectionManager::saveLoadoutResponseReceived, this,
        [this](quint64 clientId, int requestId, bool success, const QString& text) {
            emit connectionLogAppended(QStringLiteral("币号客户端 %1 保存配装%2：%3")
                .arg(clientId).arg(success ? QStringLiteral("完成") : QStringLiteral("失败"), text));
            if (pendingLoginReadyLoadouts_.contains(requestId))
            {
                const LoginReadyState state = pendingLoginReadyLoadouts_.take(requestId);
                pendingLoadoutRequests_.remove(requestId);
                if (!success)
                {
                    emit statusChanged(QStringLiteral("客户端 %1 登录成功后的保存配装失败：%2")
                        .arg(clientId).arg(text), ServerStatusTone::Error);
                    return;
                }
                if (state.listingText.trimmed().isEmpty() || state.listingPrice.trimmed().isEmpty()
                    || state.purchaseCount <= 0 || state.purchaseCount > 99)
                {
                    emit statusChanged(QStringLiteral("客户端 %1 登录成功，但上架参数不完整")
                        .arg(clientId), ServerStatusTone::Error);
                    connectionManager_.forceResetClientAction(clientId);
                    return;
                }

                QString errorText;
                const int listingRequestId = nextListingRequestId_++;
                pendingLoginReadyListings_.insert(listingRequestId, clientId);
                if (connectionManager_.requestListing(clientId, listingRequestId,
                    state.listingText.trimmed(), state.listingPrice.trimmed(), false,
                    state.purchaseCount, state.loadoutMode, errorText))
                {
                    emit connectionLogAppended(QStringLiteral("客户端 %1 登录成功后的配装已完成，已下发上架")
                        .arg(clientId));
                }
                else
                {
                    pendingLoginReadyListings_.remove(listingRequestId);
                    emit statusChanged(QStringLiteral("客户端 %1 登录成功后的上架下发失败：%2")
                        .arg(clientId).arg(errorText), ServerStatusTone::Error);
                    connectionManager_.forceResetClientAction(clientId);
                }
                return;
            }
        });
    connect(&connectionManager_, &ServerConnectionManager::listingResponseReceived, this,
        [this](quint64 clientId, int requestId, bool success, const QString& text) {
            if (pendingLoginReadyListings_.contains(requestId))
            {
                pendingLoginReadyListings_.remove(requestId);
                emit connectionLogAppended(QStringLiteral("客户端 %1 登录成功后的上架%2：%3")
                    .arg(clientId).arg(success ? QStringLiteral("完成") : QStringLiteral("失败"), text));
                emit statusChanged(success
                    ? QStringLiteral("客户端 %1 登录成功后的配装和上架已完成").arg(clientId)
                    : QStringLiteral("客户端 %1 登录成功后的上架失败：%2").arg(clientId).arg(text),
                    success ? ServerStatusTone::Success : ServerStatusTone::Error);
                return;
            }
            const QString resultLabel = success
                ? QStringLiteral("成功")
                : text.contains(QStringLiteral("使用量少")) ? QStringLiteral("少")
                : text.contains(QStringLiteral("使用量超出")) ? QStringLiteral("超出")
                : QStringLiteral("失败");
            emit connectionLogAppended(QStringLiteral("币号客户端 %1 上架%2：%3")
                .arg(clientId).arg(resultLabel, text));
        });
    connect(&connectionManager_, &ServerConnectionManager::mailResponseReceived, this,
        [this](quint64 clientId, int requestId, bool success, const QString& text) {
            emit connectionLogAppended(QStringLiteral("客户端 %1 领取邮件%2：%3")
                .arg(clientId).arg(success ? QStringLiteral("完成") : QStringLiteral("失败"), text));
        });
    connect(&connectionManager_, &ServerConnectionManager::timeSyncResponseReceived, this,
        [this](quint64 clientId, int requestId, qint64 clientReceiveMs, qint64 clientSendMs,
            qint64 serverSendMs, qint64 serverReceiveMs) {
            if (requestId == Protocol::InitialTimeSyncRequestId)
            {
                // 初次连接校时只用于客户端显示，不参与补偿任务。
                const qint64 offsetMs = ((clientReceiveMs - serverSendMs) + (clientSendMs - serverReceiveMs)) / 2;
                QString timeError;
                connectionManager_.sendServerTime(clientId, serverReceiveMs, offsetMs, timeError);
                return;
            }
            if (!compensationRound_.active)
                return;
            if (compensationRound_.requestIds.value(clientId, 0) != requestId)
                return;
            // NTP 四时间戳偏移估计：client - server。
            const qint64 offsetMs = ((clientReceiveMs - serverSendMs) + (clientSendMs - serverReceiveMs)) / 2;
            compensationRound_.offsetsMs.insert(clientId, offsetMs);
            QString timeError;
            connectionManager_.sendServerTime(clientId, serverReceiveMs, offsetMs, timeError);
            if (compensationRound_.offsetsMs.size() == compensationRound_.requestIds.size())
                finishCompensationSync(false);
        });
    connect(&connectionManager_, &ServerConnectionManager::clientsChanged, this, [this]() {
        emit clientsChanged(connectionManager_.clients());
    });
    connect(&connectionManager_, &ServerConnectionManager::accountLoginResponseReceived, this,
        [this](quint64 clientId, int, int status, const QString& text) {
            emit connectionLogAppended(QStringLiteral("客户端 %1 上号状态：%2").arg(clientId).arg(text));
            if (status == -2)
            {
                QString accountLine;
                if (markAccountBanned(clientId, accountLine))
                {
                    if (appendBannedAccount(accountLine))
                    {
                        emit statusChanged(QStringLiteral("客户端 %1 检测到封号，已写入封号.txt，准备切换下一账号").arg(clientId), ServerStatusTone::Error);
                    }
                    else
                    {
                        emit statusChanged(QStringLiteral("客户端 %1 检测到封号，但封号.txt 写入失败").arg(clientId), ServerStatusTone::Error);
                    }
                }
                else
                {
                    emit statusChanged(QStringLiteral("客户端 %1 检测到封号，但未找到对应账号记录").arg(clientId), ServerStatusTone::Error);
                }
                clearPendingAccountLoginRequests(clientId);
                connectionManager_.forceResetClientAction(clientId);
                startAccountLogin(clientId);
                return;
            }
            if (status == 3) { handleAccountLoginReady(clientId); return; }
            if (status != -2
                && text.contains(QStringLiteral("启动器登录组件报错"), Qt::CaseInsensitive)
                && text.contains(QStringLiteral("游戏被强制关闭"), Qt::CaseInsensitive))
            {
                clearPendingAccountLoginRequests(clientId);
                connectionManager_.forceResetClientAction(clientId);
                emit statusChanged(QStringLiteral("客户端 %1 启动器登录组件报错，准备复用当前账号重试").arg(clientId), ServerStatusTone::Error);
                startAccountLogin(clientId);
                return;
            }
            if (status != 1) connectionManager_.forceResetClientAction(clientId);
        });
    connect(&connectionManager_, &ServerConnectionManager::amountReportReceived, this,
        [this](quint64 clientId, qint64 amount) {
            QString errorText;
            if (!connectionManager_.updateBossAmount(clientId, amount, errorText))
                return;
            const auto current = connectionManager_.clients();
            for (const auto& client : current)
            {
                if (client.id != clientId)
                    continue;
                const QString roleText = client.role == ClientRole::Boss
                    ? QStringLiteral("老板") : QStringLiteral("币号");
                emit connectionLogAppended(QStringLiteral("%1客户端 %2 金额更新：%3")
                    .arg(roleText).arg(clientId).arg(amount));
                if (client.role == ClientRole::Boss
                    && client.expectedAmount != QStringLiteral("-")
                    && amount >= client.expectedAmount.toLongLong())
                {
                    requestForceStopAction(clientId);
                    emit connectionLogAppended(QStringLiteral("老板客户端 %1 已达到期望金额，停止该客户端").arg(clientId));
                }
                if (client.consecutiveHighPriceCount >= highPriceStopThreshold_)
                {
                    requestForceStopAllActions();
                    emit connectionLogAppended(QStringLiteral("老板客户端 %1 连续 %2 次高价，停止所有客户端")
                        .arg(clientId).arg(highPriceStopThreshold_));
                }
                break;
            }
        });
    connect(&connectionManager_, &ServerConnectionManager::prepareResponseReceived, this,
        &ServerController::handlePrepareResponse);
    connect(&connectionManager_, &ServerConnectionManager::listeningChanged, this, &ServerController::listeningChanged);
    connect(&connectionManager_, &ServerConnectionManager::networkError, this, [this](const QString& text) {
        emit statusChanged(text, ServerStatusTone::Error);
        emit connectionLogAppended(text);
    });
}

QString ServerController::defaultPortText() const
{
    return QString::number(Protocol::DefaultPort);
}

QVector<ClientListItem> ServerController::clients() const
{
    return connectionManager_.clients();
}

bool ServerController::isListening() const
{
    return connectionManager_.isListening();
}

void ServerController::startListening(const QString& portText)
{
    const int port = parsePort(portText);
    if (port <= 0)
    {
        emit statusChanged(QStringLiteral("端口无效"), ServerStatusTone::Error);
        emit messageRequested(QStringLiteral("错误"), QStringLiteral("端口必须是 1-65535 之间的数字"), true);
        return;
    }

    QString errorText;
    if (!connectionManager_.startListening(port, errorText))
    {
        emit statusChanged(errorText, ServerStatusTone::Error);
        emit messageRequested(QStringLiteral("错误"), errorText, true);
        return;
    }

    emit statusChanged(QStringLiteral("正在监听端口 %1").arg(port), ServerStatusTone::Success);
}

void ServerController::stopListening()
{
    connectionManager_.stopListening();
    emit statusChanged(QStringLiteral("已停止监听"), ServerStatusTone::Neutral);
}

void ServerController::triggerClients(const QString& countdownText, const QString& groupDelayText, bool coinFirst,
    bool compensationEnabled, const QString& compensationLeadText, const QString& bossPriceText, bool allowIncomplete, bool automatic)
{
    const std::wstring countdown = countdownText.toStdWString();
    const int seconds = ParseCountdownSeconds(countdown.c_str());
    if (seconds < 0)
    {
        emit statusChanged(QStringLiteral("倒计时秒数无效"), ServerStatusTone::Error);
        emit messageRequested(QStringLiteral("错误"), QStringLiteral("倒计时秒数必须是不小于 0 的整数"), true);
        return;
    }

    bool delayOk = false;
    const int groupDelayMs = groupDelayText.toInt(&delayOk);
    if (!delayOk || groupDelayMs < 0 || groupDelayMs > 600000)
    {
        emit statusChanged(QStringLiteral("分组延迟无效"), ServerStatusTone::Error);
        emit messageRequested(QStringLiteral("错误"), QStringLiteral("分组延迟必须是 0-600000 ms 的整数"), true);
        return;
    }

    bool leadOk = false;
    const int leadMs = compensationEnabled ? compensationLeadText.toInt(&leadOk) : 0;
    if (compensationEnabled && (!leadOk || leadMs < 0 || leadMs > 600000))
    {
        emit statusChanged(QStringLiteral("提前准备时间无效"), ServerStatusTone::Error);
        emit messageRequested(QStringLiteral("错误"), QStringLiteral("提前准备时间必须是 0-600000 ms 的整数"), true);
        return;
    }

    const ClientRole firstRole = coinFirst ? ClientRole::Coin : ClientRole::Boss;
    const ClientRole secondRole = coinFirst ? ClientRole::Boss : ClientRole::Coin;
    const QString normalizedBossPrice = bossPriceText.trimmed().isEmpty()
        ? bossListingPrice_.trimmed() : bossPriceText.trimmed();
    if (!normalizedBossPrice.isEmpty())
    {
        bossListingPrice_ = normalizedBossPrice;
    }
    bool bossPriceOk = false;
    const double bossPriceValue = normalizedBossPrice.toDouble(&bossPriceOk);
    bool hasConnectedBoss = false;
    for (const ClientListItem& client : connectionManager_.clients())
    {
        if (client.connected && client.role == ClientRole::Boss)
        {
            hasConnectedBoss = true;
            break;
        }
    }
    if (hasConnectedBoss && (!bossPriceOk || bossPriceValue <= 0.0))
    {
        emit statusChanged(QStringLiteral("老板上架价格无效"), ServerStatusTone::Error);
        emit messageRequested(QStringLiteral("错误"), QStringLiteral("请填写有效的老板上架价格后再执行"), true);
        return;
    }

    QVector<quint64> availableBosses;
    QVector<quint64> availableCoins;
    for (const ClientListItem& client : connectionManager_.clients())
    {
        if (!client.connected || !client.ready || client.actionRunning)
            continue;
        // 每一轮执行都先由老板客户端完成独立的准备检查；这里仅筛选空闲老板。
        if (client.role == ClientRole::Boss
            && client.teamReadyText != QStringLiteral("执行中"))
            availableBosses.push_back(client.id);
        else if (client.role == ClientRole::Coin && client.listingStatus == QStringLiteral("成功"))
            availableCoins.push_back(client.id);
    }
    const int completeGroups = qMin(availableBosses.size() / autoExecuteBossThreshold_,
        availableCoins.size() / autoExecuteCoinThreshold_);
    if (completeGroups <= 0 && !allowIncomplete)
    {
        emit statusChanged(QStringLiteral("当前不满足一整组执行条件"), ServerStatusTone::Error);
        if (!automatic)
            emit incompleteTriggerRequested();
        return;
    }
    const int groupsToSend = completeGroups > 0 ? completeGroups : 1;
    const int bossCountToSend = allowIncomplete && completeGroups <= 0
        ? availableBosses.size() : groupsToSend * autoExecuteBossThreshold_;
    const int coinCountToSend = allowIncomplete && completeGroups <= 0
        ? availableCoins.size() : groupsToSend * autoExecuteCoinThreshold_;
    availableBosses.resize(qMin(bossCountToSend, availableBosses.size()));
    availableCoins.resize(qMin(coinCountToSend, availableCoins.size()));
    const QVector<quint64>& firstIds = coinFirst ? availableCoins : availableBosses;
    const QVector<quint64>& secondIds = coinFirst ? availableBosses : availableCoins;
    // 保存最近一次“执行”按钮使用的参数，自动执行达到阈值时沿用相同配置。
    lastTriggerCountdownText_ = countdownText;
    lastTriggerGroupDelayText_ = groupDelayText;
    lastTriggerCoinFirst_ = coinFirst;
    lastTriggerCompensationEnabled_ = compensationEnabled;
    lastTriggerCompensationLeadText_ = compensationLeadText;

    if (!dispatchingPreparedExecution_ && !availableBosses.isEmpty())
    {
        if (preparingExecution_)
            return;
        pendingCountdownText_ = countdownText;
        pendingGroupDelayText_ = groupDelayText;
        pendingCoinFirst_ = coinFirst;
        pendingCompensationEnabled_ = compensationEnabled;
        pendingCompensationLeadText_ = compensationLeadText;
        pendingBossPriceText_ = normalizedBossPrice;
        pendingAllowIncomplete_ = allowIncomplete;
        pendingAutomatic_ = automatic;
        pendingPrepareRequests_.clear();
        preparingExecution_ = true;
        for (quint64 bossId : availableBosses)
        {
            const int requestId = nextOcrRequestId_++;
            QString prepareError;
            if (connectionManager_.requestPrepare(bossId, requestId, prepareError))
                pendingPrepareRequests_.insert(requestId, bossId);
            else
                emit connectionLogAppended(QStringLiteral("老板客户端 %1 准备请求失败：%2").arg(bossId).arg(prepareError));
        }
        if (pendingPrepareRequests_.isEmpty())
        {
            preparingExecution_ = false;
            emit statusChanged(QStringLiteral("没有成功发送老板准备检查"), ServerStatusTone::Error);
            return;
        }
        for (quint64 id : availableBosses)
            connectionManager_.setClientTeamReadyDisplay(id, QStringLiteral("准备检查中"), -1);
        emit statusChanged(QStringLiteral("已发送老板准备检查，等待 %1 个老板回报后下发执行").arg(pendingPrepareRequests_.size()), ServerStatusTone::Neutral);
        return;
    }
    // 执行开始后只清除本轮选中客户端状态，未入组的成功状态保留。
    QSet<quint64> selectedIds;
    for (quint64 id : availableBosses) selectedIds.insert(id);
    for (quint64 id : availableCoins) selectedIds.insert(id);
    for (const ClientListItem& client : connectionManager_.clients())
    {
        if (!selectedIds.contains(client.id))
            continue;
        if (client.role == ClientRole::Coin)
            connectionManager_.setClientListingStatus(client.id, QStringLiteral("执行中"));
        else if (client.role == ClientRole::Boss)
            connectionManager_.setClientTeamReadyDisplay(client.id, QStringLiteral("执行中"), -1);
    }
    if (compensationEnabled)
    {
        if (compensationRound_.active)
        {
            emit statusChanged(QStringLiteral("上一轮时间同步尚未完成"), ServerStatusTone::Error);
            return;
        }
        const QVector<ClientListItem> currentClients = connectionManager_.clients();
        compensationRound_ = CompensationRound{};
        compensationRound_.active = true;
        compensationRound_.seconds = seconds;
        compensationRound_.leadMs = leadMs;
        compensationRound_.groupDelayMs = groupDelayMs;
        compensationRound_.bossPrice = normalizedBossPrice;
        compensationRound_.firstRole = firstRole;
        compensationRound_.secondRole = secondRole;
        compensationRound_.deadlineMs = QDateTime::currentMSecsSinceEpoch() + 10000;
        compensationRound_.selectedClientIds = firstIds;
        compensationRound_.selectedClientIds += secondIds;
        for (const ClientListItem& client : currentClients)
        {
            if (!compensationRound_.selectedClientIds.contains(client.id))
                continue;
            const int requestId = nextTimeSyncRequestId_++;
            QString errorText;
            const qint64 sendMs = QDateTime::currentMSecsSinceEpoch();
            compensationRound_.requestIds.insert(client.id, requestId);
            compensationRound_.sendTimesMs.insert(client.id, sendMs);
            if (connectionManager_.requestTimeSync(client.id, requestId, sendMs, errorText))
            {
                continue;
            }
            compensationRound_.requestIds.remove(client.id);
            compensationRound_.sendTimesMs.remove(client.id);
        }
        if (compensationRound_.requestIds.isEmpty())
        {
            compensationRound_ = CompensationRound{};
            emit statusChanged(QStringLiteral("没有可同步的已准备客户端"), ServerStatusTone::Error);
            return;
        }
        compensationSyncTimer_.start();
        emit statusChanged(QStringLiteral("正在同步客户端时间：0/%1，超时上限 10 秒")
            .arg(compensationRound_.requestIds.size()), ServerStatusTone::Neutral);
        return;
    }
    // “执行”只负责触发已完成上架的客户端；上架状态要保留给全自动阈值判断。
    // 下一轮点击“上架”时再由 startListing() 清零。
    const int firstCount = connectionManager_.triggerClientsByIds(seconds, firstIds, normalizedBossPrice);
    const QString firstText = QString::fromStdWString(ClientRoleUtil::ToDisplayText(firstRole));
    const QString secondText = QString::fromStdWString(ClientRoleUtil::ToDisplayText(secondRole));

    const auto triggerSecondGroup = [this, seconds, secondRole, secondIds, firstCount, firstText, secondText, groupDelayMs, normalizedBossPrice]() {
        const int secondCount = connectionManager_.triggerClientsByIds(seconds, secondIds, normalizedBossPrice);
        emit statusChanged(QStringLiteral("全部出发：%1 %2 个 -> 延迟 %3 ms -> %4 %5 个")
            .arg(firstText).arg(firstCount).arg(groupDelayMs).arg(secondText).arg(secondCount), ServerStatusTone::Success);
    };

    if (groupDelayMs == 0)
    {
        triggerSecondGroup();
        return;
    }

    // 组间延迟使用精确定时器；同组客户端由 manager 紧密连续发送。
    QTimer::singleShot(groupDelayMs, Qt::PreciseTimer, this, triggerSecondGroup);
    emit statusChanged(QStringLiteral("已触发%1 %2 个，%3 ms 后触发%4")
        .arg(firstText).arg(firstCount).arg(groupDelayMs).arg(secondText), ServerStatusTone::Neutral);
}

void ServerController::processCompensationSync()
{
    if (!compensationRound_.active)
    {
        compensationSyncTimer_.stop();
        return;
    }
    if (QDateTime::currentMSecsSinceEpoch() >= compensationRound_.deadlineMs)
        finishCompensationSync(true);
}

void ServerController::finishCompensationSync(bool timedOut)
{
    if (!compensationRound_.active)
        return;
    compensationSyncTimer_.stop();
    if (timedOut || compensationRound_.offsetsMs.size() != compensationRound_.requestIds.size())
    {
        emit statusChanged(QStringLiteral("时间同步超时：已同步 %1/%2，未执行补偿任务")
            .arg(compensationRound_.offsetsMs.size()).arg(compensationRound_.requestIds.size()), ServerStatusTone::Error);
        compensationRound_ = CompensationRound{};
        return;
    }

    const qint64 targetServerA = QDateTime::currentMSecsSinceEpoch()
        + compensationRound_.leadMs + static_cast<qint64>(compensationRound_.seconds) * 1000;
    const qint64 targetServerB = targetServerA + compensationRound_.groupDelayMs;
    // 延迟补偿下发也不能清除币号本轮上架成功状态，否则全自动执行无法触发。
    for (const ClientListItem& client : connectionManager_.clients())
    {
        if (!compensationRound_.selectedClientIds.contains(client.id)
            || !compensationRound_.offsetsMs.contains(client.id))
            continue;
        if (client.role == ClientRole::Coin)
            connectionManager_.setClientListingStatus(client.id, QStringLiteral("执行中"));
        else if (client.role == ClientRole::Boss)
            connectionManager_.setClientTeamReadyDisplay(client.id, QStringLiteral("执行中"), -1);
    }
    int sentCount = 0;
    for (const ClientListItem& client : connectionManager_.clients())
    {
        if (!compensationRound_.selectedClientIds.contains(client.id)
            || !compensationRound_.offsetsMs.contains(client.id))
            continue;
        const qint64 targetServer = client.role == compensationRound_.firstRole ? targetServerA : targetServerB;
        QString errorText;
        if (connectionManager_.sendScheduledTrigger(client.id, compensationRound_.seconds,
            targetServer + compensationRound_.offsetsMs.value(client.id), compensationRound_.bossPrice, errorText))
            ++sentCount;
    }
    emit statusChanged(QStringLiteral("延迟补偿已下发：%1 个客户端，A 目标 %2，B 目标 %3")
        .arg(sentCount)
        .arg(QDateTime::fromMSecsSinceEpoch(targetServerA).toString(QStringLiteral("HH:mm:ss.zzz")))
        .arg(QDateTime::fromMSecsSinceEpoch(targetServerB).toString(QStringLiteral("HH:mm:ss.zzz"))), ServerStatusTone::Success);
    compensationRound_ = CompensationRound{};
}

void ServerController::triggerClient(quint64 clientId, const QString& bossPriceText)
{
    QString errorText;
    const QString normalizedBossPrice = bossPriceText.trimmed().isEmpty()
        ? bossListingPrice_ : bossPriceText.trimmed();
    if (!connectionManager_.triggerSingleClient(clientId, 0, normalizedBossPrice, errorText))
    {
        emit statusChanged(errorText, ServerStatusTone::Error);
        return;
    }
    emit statusChanged(QStringLiteral("已向指定客户端发送立即出发"), ServerStatusTone::Success);
}

void ServerController::requestTeamCode(quint64 clientId)
{
    QString errorText;
    if (!connectionManager_.requestTeamCode(clientId, errorText))
    {
        emit statusChanged(errorText, ServerStatusTone::Error);
        return;
    }
    emit statusChanged(QStringLiteral("已发送获取组队码指令"), ServerStatusTone::Success);
}

void ServerController::requestTeamReady(quint64 clientId)
{
    QString errorText;
    if (!connectionManager_.requestTeamReady(clientId, errorText))
    {
        emit statusChanged(errorText, ServerStatusTone::Error);
        return;
    }
    emit statusChanged(QStringLiteral("已发送识别队伍状态指令"), ServerStatusTone::Success);
}

void ServerController::requestRebindWindow(quint64 clientId)
{
    QString errorText;
    if (connectionManager_.requestRebindWindow(clientId, errorText))
    {
        emit statusChanged(QStringLiteral("已向客户端发送重新绑定窗口指令"), ServerStatusTone::Success);
    }
    else
    {
        emit statusChanged(errorText, ServerStatusTone::Error);
    }
}

void ServerController::requestRebindAllWindows()
{
    const int sentCount = connectionManager_.requestRebindAllWindows();
    emit statusChanged(QStringLiteral("已向 %1 个客户端发送重新绑定窗口指令").arg(sentCount), ServerStatusTone::Success);
}

void ServerController::requestForceStopAction(quint64 clientId)
{
    QString errorText;
    if (!connectionManager_.requestForceStopAction(clientId, errorText))
    {
        emit statusChanged(errorText, ServerStatusTone::Error);
        return;
    }
    resetAutoStartClient(clientId);
    connectionManager_.forceResetClientAction(clientId);
    emit statusChanged(QStringLiteral("已向客户端发送强制停止指令"), ServerStatusTone::Success);
}

void ServerController::setBossExpectedAmount(quint64 clientId, const QString& amountText)
{
    if (amountText.trimmed().isEmpty())
    {
        QString errorText;
        if (connectionManager_.setBossExpectedAmount(clientId, -1, errorText))
        {
            emit statusChanged(QStringLiteral("已清除老板期望金额，将持续执行"), ServerStatusTone::Success);
            return;
        }
        emit statusChanged(errorText, ServerStatusTone::Error);
        return;
    }
    bool ok = false;
    const qint64 amount = amountText.trimmed().toLongLong(&ok);
    QString errorText;
    if (!ok || amount < 0 || !connectionManager_.setBossExpectedAmount(clientId, amount, errorText))
    {
        emit statusChanged(ok ? errorText : QStringLiteral("期望金额必须是非负整数"), ServerStatusTone::Error);
        return;
    }
    emit statusChanged(QStringLiteral("已设置老板期望金额"), ServerStatusTone::Success);
}

void ServerController::requestForceStopAllActions()
{
    const int sentCount = connectionManager_.requestForceStopAllActions();
    autoStartClientStates_.clear();
    compensationSyncTimer_.stop();
    compensationRound_ = CompensationRound{};
    autoExecuteTriggered_ = false;
    preparingExecution_ = false;
    pendingPrepareRequests_.clear();
    connectionManager_.resetAllClientStates();
    emit statusChanged(QStringLiteral("已向 %1 个客户端发送强制停止指令").arg(sentCount), ServerStatusTone::Success);
}

void ServerController::requestSelfThunder(quint64 clientId)
{
    AutoStartClientState* autoStartState = nullptr;
    auto stateIterator = autoStartClientStates_.find(clientId);
    if (stateIterator != autoStartClientStates_.end())
    {
        autoStartState = &stateIterator.value();
        if (autoStartState->pendingSelfThunderRequestId > 0)
        {
            emit statusChanged(QStringLiteral("该客户端正在执行自雷中"), ServerStatusTone::Neutral);
            return;
        }
    }

    const int requestId = nextOcrRequestId_++;
    QString errorText;
    if (!connectionManager_.requestSelfThunder(clientId, requestId, errorText))
    {
        emit statusChanged(errorText, ServerStatusTone::Error);
        return;
    }

    if (autoStartState != nullptr)
    {
        autoStartState->stage = AutoStartStage::SelfThunder;
        autoStartState->pendingSelfThunderRequestId = requestId;
    }
    pendingSelfThunderRequests_.insert(requestId, clientId);
    emit statusChanged(QStringLiteral("已发送自雷指令"), ServerStatusTone::Success);
}

void ServerController::requestLoadout(quint64 clientId)
{
    AutoStartClientState state;
    if (!requestLoadout(clientId, state))
    {
        return;
    }
    emit statusChanged(QStringLiteral("已发送配装指令"), ServerStatusTone::Success);
}

void ServerController::setAutoStartEnabled(bool enabled, const QString& countdownText)
{
    if (enabled)
    {
        const std::wstring countdown = countdownText.toStdWString();
        if (ParseCountdownSeconds(countdown.c_str()) < 0)
        {
            autoStartEnabled_ = false;
            autoStartTimer_.stop();
            emit autoStartEnabledChanged(false);
            emit statusChanged(QStringLiteral("自动开始失败：倒计时秒数无效"), ServerStatusTone::Error);
            emit messageRequested(QStringLiteral("错误"), QStringLiteral("倒计时秒数必须是不小于 0 的整数"), true);
            return;
        }

        autoStartCountdownText_ = countdownText;
        autoStartEnabled_ = true;
        autoStartClientStates_.clear();
        pendingOcrRequests_.clear();
        pendingSelfThunderRequests_.clear();
        autoStartCanTrigger_ = true;
        autoStartCycleStartedAtMs_ = QDateTime::currentMSecsSinceEpoch();
        if (!autoStartTimer_.isActive())
        {
            autoStartTimer_.start();
        }
        emit statusChanged(QStringLiteral("自动开始已开启，等待队伍满员"), ServerStatusTone::Success);
        processAutoStartTick();
        return;
    }

    autoStartEnabled_ = false;
    autoStartTimer_.stop();
    autoStartClientStates_.clear();
    pendingOcrRequests_.clear();
    pendingSelfThunderRequests_.clear();
    pendingLoadoutRequests_.clear();
    autoStartCanTrigger_ = true;
    autoStartCycleStartedAtMs_ = 0;
    emit statusChanged(QStringLiteral("自动开始已关闭"), ServerStatusTone::Neutral);
}

void ServerController::setAutoStartCountdownText(const QString& countdownText)
{
    autoStartCountdownText_ = countdownText;
}

void ServerController::handleAutoStartOcrResponse(quint64 clientId, int requestId, bool success, const QString& text)
{
    if (!pendingOcrRequests_.contains(requestId) || pendingOcrRequests_.value(requestId) != clientId)
    {
        return;
    }
    pendingOcrRequests_.remove(requestId);

    auto stateIterator = autoStartClientStates_.find(clientId);
    if (stateIterator == autoStartClientStates_.end() || stateIterator->pendingRequestId != requestId)
    {
        return;
    }
    AutoStartClientState& state = stateIterator.value();
    state.pendingRequestId = 0;

    const bool noTextFound = text.contains(QStringLiteral("No text found"), Qt::CaseInsensitive);
    if (!success)
    {
        if (state.stage == AutoStartStage::Matching && state.sawCancelAction && noTextFound)
        {
            // 已识别过“取消行动”后，按钮区无文本可视为“配装/出发/取消行动”已全部消失，进入加载阶段。
            state.stage = AutoStartStage::Loading;
            state.stageStartedAtMs = QDateTime::currentMSecsSinceEpoch();
            connectionManager_.setClientTeamReadyDisplay(clientId, QStringLiteral("正在加载"), -1);
            return;
        }

        emit statusChanged(QStringLiteral("自动开始：OCR识别失败，等待重试：%1").arg(text), ServerStatusTone::Error);
        return;
    }

    if (state.stage == AutoStartStage::Matching)
    {
        if (AutoStartOcrRules::ContainsCancelActionMarker(text))
        {
            state.sawCancelAction = true;
            connectionManager_.setClientTeamReadyDisplay(clientId, QStringLiteral("匹配中"), -1);
        }

        if (state.sawCancelAction && !AutoStartOcrRules::ContainsActionRegionWaitingMarker(text))
        {
            // 行动按钮区域消失后进入加载阶段，下一轮改识别左上角地图类型。
            state.stage = AutoStartStage::Loading;
            state.stageStartedAtMs = QDateTime::currentMSecsSinceEpoch();
            connectionManager_.setClientTeamReadyDisplay(clientId, QStringLiteral("正在加载"), -1);
        }
        return;
    }

    if (state.stage == AutoStartStage::Loading)
    {
        if (AutoStartOcrRules::ContainsMapTypeMarker(text))
        {
            // 识别到地图类型说明已进入对局，随后向该客户端发送自雷请求。
            state.stage = AutoStartStage::InGame;
            connectionManager_.setClientTeamReadyDisplay(clientId, QStringLiteral("进入对局"), -1);
            requestSelfThunder(clientId, state);
        }
    }
}

void ServerController::handleSelfThunderResponse(quint64 clientId, int requestId, bool success, const QString& text)
{
    if (!pendingSelfThunderRequests_.contains(requestId) || pendingSelfThunderRequests_.value(requestId) != clientId)
    {
        return;
    }
    pendingSelfThunderRequests_.remove(requestId);

    auto stateIterator = autoStartClientStates_.find(clientId);
    if (stateIterator != autoStartClientStates_.end() && stateIterator->pendingSelfThunderRequestId == requestId)
    {
        stateIterator->pendingSelfThunderRequestId = 0;

        if (success)
        {
            connectionManager_.setClientTeamReadyDisplay(clientId, QStringLiteral("自雷完成"), -1);
            emit statusChanged(QStringLiteral("自动开始：客户端自雷完成，回到队伍状态判断"), ServerStatusTone::Success);
            resetAutoStartClient(clientId);
            return;
        }

        connectionManager_.setClientTeamReadyDisplay(clientId, QStringLiteral("自雷失败"), -1);
        emit statusChanged(QStringLiteral("自动开始：自雷失败，等待人工处理：%1").arg(text), ServerStatusTone::Error);
        return;
    }

    if (success)
    {
        connectionManager_.setClientTeamReadyDisplay(clientId, QStringLiteral("自雷完成"), -1);
        emit statusChanged(QStringLiteral("客户端自雷完成"), ServerStatusTone::Success);
        return;
    }

    connectionManager_.setClientTeamReadyDisplay(clientId, QStringLiteral("自雷失败"), -1);
    emit statusChanged(QStringLiteral("客户端自雷失败：%1").arg(text), ServerStatusTone::Error);
}

void ServerController::handleLoadoutResponse(quint64 clientId, int requestId, bool success, const QString& text)
{
    if (!pendingLoadoutRequests_.contains(requestId) || pendingLoadoutRequests_.value(requestId) != clientId)
    {
        return;
    }
    pendingLoadoutRequests_.remove(requestId);

    auto stateIterator = autoStartClientStates_.find(clientId);
    if (stateIterator != autoStartClientStates_.end() && stateIterator->pendingLoadoutRequestId == requestId)
    {
        stateIterator->pendingLoadoutRequestId = 0;
        if (success)
        {
            connectionManager_.setClientTeamReadyDisplay(clientId, QStringLiteral("配装完成"), -1);
            emit statusChanged(QStringLiteral("自动开始：客户端配装完成"), ServerStatusTone::Success);
            return;
        }

        connectionManager_.setClientTeamReadyDisplay(clientId, QStringLiteral("配装失败"), -1);
        stateIterator->loadoutFailed = true;
        emit statusChanged(QStringLiteral("自动开始：配装失败，等待人工处理：%1").arg(text), ServerStatusTone::Error);
        return;
    }

    emit statusChanged(success ? QStringLiteral("客户端配装完成") : QStringLiteral("客户端配装失败：%1").arg(text), success ? ServerStatusTone::Success : ServerStatusTone::Error);
}

void ServerController::resetAutoStartRound()
{
    autoStartClientStates_.clear();
    pendingOcrRequests_.clear();
    pendingSelfThunderRequests_.clear();
    pendingLoadoutRequests_.clear();
    autoStartCanTrigger_ = true;
    autoStartCycleStartedAtMs_ = QDateTime::currentMSecsSinceEpoch();
}

void ServerController::resetAutoStartClient(quint64 clientId)
{
    autoStartClientStates_.remove(clientId);

    QList<int> staleRequests;
    for (auto iterator = pendingOcrRequests_.cbegin(); iterator != pendingOcrRequests_.cend(); ++iterator)
    {
        if (iterator.value() == clientId)
        {
            staleRequests.push_back(iterator.key());
        }
    }
    for (int requestId : staleRequests)
    {
        pendingOcrRequests_.remove(requestId);
    }

    QList<int> staleSelfThunderRequests;
    for (auto iterator = pendingSelfThunderRequests_.cbegin(); iterator != pendingSelfThunderRequests_.cend(); ++iterator)
    {
        if (iterator.value() == clientId)
        {
            staleSelfThunderRequests.push_back(iterator.key());
        }
    }
    for (int requestId : staleSelfThunderRequests)
    {
        pendingSelfThunderRequests_.remove(requestId);
    }

    QList<int> staleLoadoutRequests;
    for (auto iterator = pendingLoadoutRequests_.cbegin(); iterator != pendingLoadoutRequests_.cend(); ++iterator)
    {
        if (iterator.value() == clientId)
        {
            staleLoadoutRequests.push_back(iterator.key());
        }
    }
    for (int requestId : staleLoadoutRequests)
    {
        pendingLoadoutRequests_.remove(requestId);
    }

    // 单客户端完成自雷后立即回到队伍状态判断，不等待其它客户端结束本轮。
    autoStartCanTrigger_ = true;
    autoStartCycleStartedAtMs_ = QDateTime::currentMSecsSinceEpoch();
}

bool ServerController::requestStageOcr(quint64 clientId, AutoStartClientState& state, AutoStartStage stage, const OcrRequestRegion& region)
{
    if (state.pendingRequestId > 0)
    {
        return false;
    }

    const int requestId = nextOcrRequestId_++;
    QString errorText;
    if (!connectionManager_.requestOcr(clientId, requestId, region, errorText))
    {
        emit statusChanged(errorText, ServerStatusTone::Error);
        return false;
    }

    state.stage = stage;
    state.pendingRequestId = requestId;
    pendingOcrRequests_.insert(requestId, clientId);
    return true;
}

bool ServerController::requestSelfThunder(quint64 clientId, AutoStartClientState& state)
{
    if (state.pendingSelfThunderRequestId > 0)
    {
        return false;
    }

    const int requestId = nextOcrRequestId_++;
    QString errorText;
    if (!connectionManager_.requestSelfThunder(clientId, requestId, errorText))
    {
        emit statusChanged(errorText, ServerStatusTone::Error);
        return false;
    }

    state.stage = AutoStartStage::SelfThunder;
    state.pendingSelfThunderRequestId = requestId;
    pendingSelfThunderRequests_.insert(requestId, clientId);
    return true;
}

bool ServerController::requestLoadout(quint64 clientId, AutoStartClientState& state)
{
    if (state.pendingLoadoutRequestId > 0)
    {
        return false;
    }

    const int requestId = nextOcrRequestId_++;
    QString errorText;
    if (!connectionManager_.requestLoadout(clientId, requestId, QStringLiteral("普通"), errorText))
    {
        emit statusChanged(errorText, ServerStatusTone::Error);
        return false;
    }

    state.stage = AutoStartStage::Loadout;
    state.pendingLoadoutRequestId = requestId;
    pendingLoadoutRequests_.insert(requestId, clientId);
    return true;
}

void ServerController::processTriggeredClientStage(const ClientListItem& client, AutoStartClientState& state, qint64 nowMs)
{
    if (state.pendingRequestId > 0 || client.actionRunning)
    {
        return;
    }

    if (state.stage == AutoStartStage::AfterTriggerDelay)
    {
        if (nowMs - state.stageStartedAtMs < AutoStartAfterTriggerDelayMs)
        {
            return;
        }
        // 全局出发后先看右下角行动按钮区域，判断是否进入匹配/加载阶段。
        requestStageOcr(client.id, state, AutoStartStage::Matching, ActionRegion);
        return;
    }

    if (state.stage == AutoStartStage::Matching)
    {
        requestStageOcr(client.id, state, AutoStartStage::Matching, ActionRegion);
        return;
    }

    if (state.stage == AutoStartStage::Loading)
    {
        requestStageOcr(client.id, state, AutoStartStage::Loading, MapTypeRegion);
    }
}

void ServerController::processMatchingRescue(const QVector<ClientListItem>& clients, qint64 nowMs)
{
    int connectedClientCount = 0;
    for (const ClientListItem& client : clients)
    {
        if (client.connected)
        {
            ++connectedClientCount;
        }
    }
    const bool enoughConnectedClients = connectedClientCount >= 2;
    if (!enoughConnectedClients)
    {
        return;
    }

    bool hasStableLoadingClient = false;
    for (auto iterator = autoStartClientStates_.cbegin(); iterator != autoStartClientStates_.cend(); ++iterator)
    {
        if (iterator->stage == AutoStartStage::Loading && nowMs - iterator->stageStartedAtMs >= AutoStartMatchingRescueDelayMs)
        {
            hasStableLoadingClient = true;
            break;
        }
    }
    if (!hasStableLoadingClient)
    {
        return;
    }

    quint64 rescueClientId = 0;
    for (auto iterator = autoStartClientStates_.cbegin(); iterator != autoStartClientStates_.cend(); ++iterator)
    {
        if (iterator->stage == AutoStartStage::Matching
            && iterator->sawCancelAction
            && iterator->pendingRequestId == 0
            && !iterator->rescueTriggerSent)
        {
            rescueClientId = iterator.key();
            break;
        }
    }
    if (rescueClientId == 0)
    {
        return;
    }

    const quint64 clientId = rescueClientId;
    QString errorText;
    if (!connectionManager_.triggerSingleClient(clientId, 0, bossListingPrice_, errorText))
    {
        emit statusChanged(errorText, ServerStatusTone::Error);
        return;
    }

    auto stateIterator = autoStartClientStates_.find(clientId);
    if (stateIterator != autoStartClientStates_.end())
    {
        stateIterator->rescueTriggerSent = true;
    }

    // 有其它客户端已进入加载时，卡在匹配中的客户端立即补发出发，并只让该客户端回到 [x/n] 判断。
    resetAutoStartClient(clientId);
    connectionManager_.resetClientTeamReadyDisplay(clientId);
    emit statusChanged(QStringLiteral("自动开始：检测到客户端仍在匹配中，已单独补发立即出发"), ServerStatusTone::Success);
}

void ServerController::executeSelfThunderPlaceholder(quint64 clientId)
{
    Q_UNUSED(clientId);
    // 自雷功能暂留空方法，自动流程需要调用点以便后续接入真实动作。
}

void ServerController::processAutoStartTick()
{
    if (!autoStartEnabled_)
    {
        return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const QVector<ClientListItem> clients = connectionManager_.clients();
    QVector<ClientListItem> targets;
    QVector<ClientListItem> waitingTargets;
    for (const ClientListItem& client : clients)
    {
        if (client.connected && client.ready)
        {
            targets.push_back(client);
            if (autoStartClientStates_.contains(client.id))
            {
                continue;
            }
            waitingTargets.push_back(client);
        }
    }

    processMatchingRescue(clients, nowMs);

    for (const ClientListItem& client : targets)
    {
        auto stateIterator = autoStartClientStates_.find(client.id);
        if (stateIterator != autoStartClientStates_.end() && stateIterator->stage != AutoStartStage::Loadout)
        {
            processTriggeredClientStage(client, stateIterator.value(), nowMs);
        }
    }

    if (targets.isEmpty())
    {
        emit statusChanged(QStringLiteral("自动开始：等待已准备客户端"), ServerStatusTone::Neutral);
        return;
    }

    bool hasLoadoutStates = false;
    bool allLoadoutFinished = true;
    bool hasLoadoutFailed = false;
    for (auto iterator = autoStartClientStates_.cbegin(); iterator != autoStartClientStates_.cend(); ++iterator)
    {
        if (iterator->stage == AutoStartStage::Loadout)
        {
            hasLoadoutStates = true;
            if (iterator->pendingLoadoutRequestId > 0)
            {
                allLoadoutFinished = false;
            }
            if (iterator->loadoutFailed)
            {
                hasLoadoutFailed = true;
            }
        }
    }
    if (hasLoadoutStates)
    {
        if (hasLoadoutFailed)
        {
            emit statusChanged(QStringLiteral("自动开始：配装失败，等待人工处理"), ServerStatusTone::Error);
            return;
        }
        if (!allLoadoutFinished)
        {
            emit statusChanged(QStringLiteral("自动开始：等待客户端配装完成"), ServerStatusTone::Neutral);
            return;
        }

        const std::wstring countdown = autoStartCountdownText_.toStdWString();
        if (ParseCountdownSeconds(countdown.c_str()) < 0)
        {
            emit statusChanged(QStringLiteral("自动开始失败：倒计时秒数无效"), ServerStatusTone::Error);
            return;
        }

        triggerClients(autoStartCountdownText_, QStringLiteral("0"), true,
            false, QStringLiteral("1500"), bossListingPrice_);
        for (auto iterator = autoStartClientStates_.begin(); iterator != autoStartClientStates_.end(); ++iterator)
        {
            if (iterator->stage == AutoStartStage::Loadout)
            {
                iterator->stage = AutoStartStage::AfterTriggerDelay;
                iterator->stageStartedAtMs = nowMs;
            }
        }
        emit statusChanged(QStringLiteral("自动开始：配装完成，已执行全局出发，等待 1 秒后检测匹配状态"), ServerStatusTone::Success);
        return;
    }

    const bool hasActivePostTriggerStates = !autoStartClientStates_.isEmpty();
    if (hasActivePostTriggerStates && waitingTargets.isEmpty())
    {
        emit statusChanged(QStringLiteral("自动开始：正在跟踪出发后 OCR 阶段"), ServerStatusTone::Neutral);
        return;
    }

    if (waitingTargets.isEmpty())
    {
        emit statusChanged(QStringLiteral("自动开始：等待下一轮队伍状态判断"), ServerStatusTone::Neutral);
        return;
    }

    bool allFull = true;
    for (const ClientListItem& client : waitingTargets)
    {
        const bool full = client.teamReadyState == 1;
        if (!full)
        {
            allFull = false;
            break;
        }
    }

    if (!hasActivePostTriggerStates && autoStartCanTrigger_ && allFull)
    {
        autoStartCanTrigger_ = false;
        int loadoutRequestedCount = 0;
        for (const ClientListItem& client : waitingTargets)
        {
            AutoStartClientState state;
            state.stage = AutoStartStage::Loadout;
            state.stageStartedAtMs = nowMs;
            autoStartClientStates_.insert(client.id, state);
            auto stateIterator = autoStartClientStates_.find(client.id);
            if (stateIterator != autoStartClientStates_.end())
            {
                if (requestLoadout(client.id, stateIterator.value()))
                {
                    ++loadoutRequestedCount;
                }
                else
                {
                    stateIterator->loadoutFailed = true;
                }
            }
        }
        emit statusChanged(QStringLiteral("自动开始：队伍已满员，已发送配装指令 %1 个").arg(loadoutRequestedCount), ServerStatusTone::Success);
        return;
    }

    int requestedCount = 0;
    int busyCount = 0;
    for (const ClientListItem& client : waitingTargets)
    {
        const bool fresh = client.teamReadyUpdatedAtMs >= autoStartCycleStartedAtMs_;
        const bool full = fresh && client.teamReadyState == 1;
        if (autoStartCanTrigger_ && full)
        {
            continue;
        }
        if (client.actionRunning)
        {
            ++busyCount;
            continue;
        }

        QString errorText;
        if (connectionManager_.requestTeamReady(client.id, errorText))
        {
            ++requestedCount;
        }
    }

    if (requestedCount > 0)
    {
        emit statusChanged(QStringLiteral("自动开始：已发送队伍状态识别请求 %1 个").arg(requestedCount), ServerStatusTone::Neutral);
    }
    else if (busyCount > 0)
    {
        emit statusChanged(QStringLiteral("自动开始：等待客户端空闲或 OCR 返回"), ServerStatusTone::Neutral);
    }
    else
    {
        emit statusChanged(QStringLiteral("自动开始：等待队伍满员"), ServerStatusTone::Neutral);
    }
}

void ServerController::forceResetClientAction(quint64 clientId)
{
    resetAutoStartClient(clientId);
    if (connectionManager_.forceResetClientAction(clientId))
    {
        connectionManager_.resetClientTeamReadyDisplay(clientId);
        emit statusChanged(QStringLiteral("已强制清零该客户端状态"), ServerStatusTone::Neutral);
    }
}

void ServerController::setClientTeamCode(quint64 clientId, const QString& teamCode)
{
    QString errorText;
    if (!connectionManager_.setClientTeamCode(clientId, teamCode, errorText))
    {
        emit statusChanged(errorText, ServerStatusTone::Error);
        emit messageRequested(QStringLiteral("错误"), errorText, true);
        return;
    }
    emit statusChanged(QStringLiteral("已更新组队码"), ServerStatusTone::Success);
}

void ServerController::showSelfThunderPlaceholder(quint64 clientId)
{
    Q_UNUSED(clientId);
    emit statusChanged(QStringLiteral("自雷功能暂未实现"), ServerStatusTone::Neutral);
}

void ServerController::setClientNote(quint64 clientId, const QString& note)
{
    if (connectionManager_.setClientNote(clientId, note))
        emit statusChanged(QStringLiteral("已更新客户端备注"), ServerStatusTone::Success);
}

void ServerController::setClientRole(quint64 clientId, ClientRole role)
{
    if (connectionManager_.setClientRole(clientId, role))
    {
        emit statusChanged(QStringLiteral("已更新客户端角色：%1")
                               .arg(QString::fromStdWString(ClientRoleUtil::ToDisplayText(role))),
                           ServerStatusTone::Success);
    }
}

void ServerController::startListing(const QString& listingText, const QString& listingPrice, bool hasExchange, int purchaseCount, bool loadoutMode)
{
    const QString normalizedText = listingText.trimmed();
    const QString normalizedPrice = listingPrice.trimmed();
    bool priceOk = false;
    const double priceValue = normalizedPrice.toDouble(&priceOk);
    if (!priceOk || priceValue <= 0.0)
    {
        emit statusChanged(QStringLiteral("请输入有效的上架价格"), ServerStatusTone::Error);
        return;
    }
    if (purchaseCount <= 0 || purchaseCount > 99)
    {
        emit statusChanged(QStringLiteral("购买次数必须在 1 到 99 之间"), ServerStatusTone::Error);
        return;
    }

    const QVector<ClientListItem> currentClients = connectionManager_.clients();
    // 每轮全体上架开始前清零上一轮各客户端的出售数量 X。
    connectionManager_.resetListingQuantities();
    // 服务端点击“上架”后先留出 1 秒，让界面和任务参数稳定，再向客户端下发任务。
    QThread::msleep(1000);
    int coinCount = 0;
    int sentCount = 0;
    int failedCount = 0;
    for (const ClientListItem& client : currentClients)
    {
        if (!client.connected || client.role != ClientRole::Coin)
        {
            continue;
        }

        ++coinCount;
        connectionManager_.setClientListingStatus(client.id, QStringLiteral("待执行"));
        QString errorText;
        const int requestId = nextListingRequestId_++;
        if (connectionManager_.requestListing(client.id, requestId, normalizedText, normalizedPrice, hasExchange, purchaseCount, loadoutMode, errorText))
        {
            ++sentCount;
        }
        else
        {
            ++failedCount;
            connectionManager_.setClientListingStatus(client.id, QStringLiteral("失败"));
            emit connectionLogAppended(QStringLiteral("币号客户端 %1 上架下发失败：%2").arg(client.id).arg(errorText));
        }
    }

    if (coinCount == 0)
    {
        emit statusChanged(QStringLiteral("当前没有可执行上架的币号客户端"), ServerStatusTone::Error);
        return;
    }

    emit statusChanged(QStringLiteral("已向 %1 个币号下发上架任务，失败 %2 个")
        .arg(sentCount).arg(failedCount), failedCount == 0 ? ServerStatusTone::Success : ServerStatusTone::Neutral);
}

void ServerController::startListingForClient(quint64 clientId, const QString& listingText,
    const QString& listingPrice, bool hasExchange, int purchaseCount, bool loadoutMode)
{
    const QString normalizedText = listingText.trimmed();
    const QString normalizedPrice = listingPrice.trimmed();
    bool priceOk = false;
    const double priceValue = normalizedPrice.toDouble(&priceOk);
    if (!priceOk || priceValue <= 0.0)
    {
        emit statusChanged(QStringLiteral("请输入有效的上架价格"), ServerStatusTone::Error);
        return;
    }
    if (purchaseCount <= 0 || purchaseCount > 99)
    {
        emit statusChanged(QStringLiteral("购买次数必须在 1 到 99 之间"), ServerStatusTone::Error);
        return;
    }

    const auto currentClients = connectionManager_.clients();
    const auto target = std::find_if(currentClients.cbegin(), currentClients.cend(),
        [clientId](const ClientListItem& client) { return client.id == clientId; });
    if (target == currentClients.cend() || !target->connected)
    {
        emit statusChanged(QStringLiteral("指定客户端已断开或不存在"), ServerStatusTone::Error);
        return;
    }
    if (target->role != ClientRole::Coin)
    {
        emit statusChanged(QStringLiteral("上架功能仅对币号生效"), ServerStatusTone::Error);
        return;
    }

    connectionManager_.resetClientListingQuantity(clientId);
    connectionManager_.setClientListingStatus(clientId, QStringLiteral("待执行"));
    // 与群发上架保持相同的界面稳定等待，但只向当前目标客户端发送请求。
    QThread::msleep(1000);
    QString errorText;
    const int requestId = nextListingRequestId_++;
    if (!connectionManager_.requestListing(clientId, requestId, normalizedText, normalizedPrice,
        hasExchange, purchaseCount, loadoutMode, errorText))
    {
        connectionManager_.setClientListingStatus(clientId, QStringLiteral("失败"));
        emit connectionLogAppended(QStringLiteral("币号客户端 %1 单独上架下发失败：%2")
            .arg(clientId).arg(errorText));
        emit statusChanged(QStringLiteral("客户端 %1 上架下发失败：%2").arg(clientId).arg(errorText),
            ServerStatusTone::Error);
        return;
    }

    emit connectionLogAppended(QStringLiteral("已向币号客户端 %1 单独下发上架任务，requestId=%2")
        .arg(clientId).arg(requestId));
    emit statusChanged(QStringLiteral("已向客户端 %1 下发单独上架任务").arg(clientId), ServerStatusTone::Success);
}

void ServerController::saveLoadout(const QString& weapon, const QString& backpack, const QString& egg,
    const QString& bulletName, int perClick, int bulletTotal)
{
    saveLoadoutForRole(ClientRole::Coin, weapon, backpack, egg, bulletName, perClick, bulletTotal);
}

void ServerController::saveLoadoutForRole(ClientRole targetRole, const QString& weapon, const QString& backpack, const QString& egg,
    const QString& bulletName, int perClick, int bulletTotal)
{
    const QStringList values = { weapon.trimmed(), backpack.trimmed(), egg.trimmed(), bulletName.trimmed() };
    for (const QString& value : values)
    {
        if (value.isEmpty())
        {
            emit statusChanged(QStringLiteral("请完整填写配装参数"), ServerStatusTone::Error);
            emit messageRequested(QStringLiteral("参数错误"), QStringLiteral("武器、背包、蛋挂和子弹名称均不能为空"), true);
            return;
        }
    }
    if (perClick <= 0 || bulletTotal <= 0 || bulletTotal % perClick != 0)
    {
        emit statusChanged(QStringLiteral("子弹参数错误"), ServerStatusTone::Error);
        emit messageRequested(QStringLiteral("参数错误"), QStringLiteral("子弹总数必须能被单次数量整除"), true);
        return;
    }

    int coinCount = 0;
    int sentCount = 0;
    for (const ClientListItem& client : connectionManager_.clients())
    {
        if (!client.connected || client.role != targetRole)
        {
            continue;
        }
        ++coinCount;
        QString errorText;
        if (connectionManager_.requestSaveLoadout(client.id, nextSaveLoadoutRequestId_++, targetRole, values[0], values[1],
            values[2], values[3], perClick, bulletTotal, errorText))
        {
            ++sentCount;
        }
        else
        {
            emit connectionLogAppended(QStringLiteral("%1客户端 %2 保存配装下发失败：%3")
                .arg(targetRole == ClientRole::Coin ? QStringLiteral("币号") : QStringLiteral("老板"))
                .arg(client.id).arg(errorText));
        }
    }
    if (coinCount == 0)
    {
        emit statusChanged(QStringLiteral("当前没有可执行保存配装的目标客户端"), ServerStatusTone::Error);
        return;
    }
    emit statusChanged(QStringLiteral("已向 %1 个币号下发保存配装任务，失败 %2 个")
        .arg(sentCount).arg(coinCount - sentCount),
        sentCount == coinCount ? ServerStatusTone::Success : ServerStatusTone::Neutral);
}

void ServerController::saveLoadoutForClient(quint64 clientId, ClientRole targetRole, const QString& weapon,
    const QString& backpack, const QString& egg, const QString& bulletName, int perClick, int bulletTotal)
{
    const QStringList values = { weapon.trimmed(), backpack.trimmed(), egg.trimmed(), bulletName.trimmed() };
    for (const QString& value : values)
    {
        if (value.isEmpty())
        {
            emit statusChanged(QStringLiteral("请完整填写配装参数"), ServerStatusTone::Error);
            emit messageRequested(QStringLiteral("参数错误"), QStringLiteral("武器、背包、蛋挂和子弹名称均不能为空"), true);
            return;
        }
    }
    if (perClick <= 0 || bulletTotal <= 0 || bulletTotal % perClick != 0)
    {
        emit statusChanged(QStringLiteral("子弹参数错误"), ServerStatusTone::Error);
        emit messageRequested(QStringLiteral("参数错误"), QStringLiteral("子弹总数必须能被单次数量整除"), true);
        return;
    }

    QString errorText;
    if (!connectionManager_.requestSaveLoadout(clientId, nextSaveLoadoutRequestId_++, targetRole,
        values[0], values[1], values[2], values[3], perClick, bulletTotal, errorText))
    {
        emit statusChanged(errorText, ServerStatusTone::Error);
        emit connectionLogAppended(QStringLiteral("客户端 %1 保存配装下发失败：%2").arg(clientId).arg(errorText));
        return;
    }
    emit statusChanged(QStringLiteral("已向客户端 %1 发送保存配装任务").arg(clientId), ServerStatusTone::Success);
}

void ServerController::requestMail(quint64 clientId)
{
    QString errorText;
    if (connectionManager_.requestMail(clientId, nextMailRequestId_++, errorText))
        emit statusChanged(QStringLiteral("已向客户端发送领取邮件指令"), ServerStatusTone::Success);
    else
        emit statusChanged(errorText, ServerStatusTone::Error);
}

void ServerController::requestMailAll()
{
    int sent = 0;
    for (const ClientListItem& client : connectionManager_.clients())
    {
        if (!client.connected) continue;
        QString errorText;
        if (connectionManager_.requestMail(client.id, nextMailRequestId_++, errorText)) ++sent;
    }
    emit statusChanged(QStringLiteral("已向 %1 个客户端发送领取邮件指令").arg(sent), ServerStatusTone::Success);
}

void ServerController::importAccounts(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) { emit statusChanged(QStringLiteral("账号文件打开失败"), ServerStatusTone::Error); return; }
    QVector<AccountCredential> loaded;
    QSet<QString> seenCredentials;
    const QSet<QString> bannedCredentials = LoadBannedAccountKeys();
    int skippedBanned = 0;
    QTextStream stream(&file);
    while (!stream.atEnd())
    {
        const QString line = stream.readLine().trimmed();
        const int separator = line.indexOf(QStringLiteral("----"));
        if (separator <= 0 || separator + 4 >= line.size()) continue;
        AccountCredential item{line.left(separator).trimmed(), line.mid(separator + 4).trimmed(), false, false, 0};
        const QString key = AccountCredentialKey(item.username, item.password);
        if (!item.username.isEmpty() && !item.password.isEmpty() && !seenCredentials.contains(key))
        {
            seenCredentials.insert(key);
            if (bannedCredentials.contains(key))
            {
                ++skippedBanned;
                continue;
            }
            loaded.push_back(item);
        }
    }
    accounts_ = loaded;
    emit accountQueueChanged(accounts_.size(), accounts_.size(), 0);
    emit statusChanged(skippedBanned > 0
        ? QStringLiteral("已导入 %1 个账号，跳过 %2 个封禁账号").arg(accounts_.size()).arg(skippedBanned)
        : QStringLiteral("已导入 %1 个账号").arg(accounts_.size()), ServerStatusTone::Success);
}

void ServerController::setAccountLoginConfig(quint64 clientId, const QString& weapon, const QString& backpack,
    const QString& egg, const QString& bulletName, int perClick, int bulletTotal,
    const QString& listingText, const QString& listingPrice, int purchaseCount)
{
    LoginReadyState state;
    state.weapon = weapon.trimmed();
    state.backpack = backpack.trimmed();
    state.egg = egg.trimmed();
    state.bulletName = bulletName.trimmed();
    state.perClick = perClick;
    state.bulletTotal = bulletTotal;
    state.listingText = listingText.trimmed();
    state.listingPrice = listingPrice.trimmed();
    state.purchaseCount = purchaseCount;
    state.loadoutMode = true;
    loginReadyStates_.insert(clientId, state);
}

void ServerController::startAccountLogin(quint64 clientId)
{
    dispatchAccountLogin(clientId);
}

bool ServerController::dispatchAccountLogin(quint64 clientId)
{
    if (!loginReadyStates_.contains(clientId))
    {
        // 兼容没有经过新 UI 配置入口的调用方；客户端随后会返回缺少配装参数，
        // 但不会误用旧的固定坐标 LOADOUT_REQUEST。
        loginReadyStates_.insert(clientId, LoginReadyState{});
    }
    const QSet<QString> bannedCredentials = LoadBannedAccountKeys();
    for (AccountCredential& account : accounts_)
    {
        if (bannedCredentials.contains(AccountCredentialKey(account.username, account.password)))
        {
            account.banned = true;
        }
    }

    AccountCredential* selected = nullptr;
    for (AccountCredential& account : accounts_)
    {
        if (account.assigned && !account.banned && account.clientId == clientId)
        {
            selected = &account;
            break;
        }
    }
    if (!selected)
    {
        for (AccountCredential& account : accounts_)
        {
            if (!account.assigned && !account.banned)
            {
                selected = &account;
                break;
            }
        }
    }
    if (!selected)
    {
        emit statusChanged(QStringLiteral("没有可分发的账号"), ServerStatusTone::Error);
        return false;
    }
    QString errorText;
    if (!connectionManager_.requestAccountLogin(clientId, nextAccountLoginRequestId_++, selected->username, selected->password, errorText))
    {
        emit statusChanged(errorText, ServerStatusTone::Error);
        return false;
    }
    if (!selected->assigned)
    {
        selected->assigned = true;
        selected->clientId = clientId;
        int assigned = 0;
        for (const AccountCredential& value : accounts_) if (value.assigned) ++assigned;
        emit accountQueueChanged(accounts_.size(), accounts_.size() - assigned, assigned);
    }
    emit statusChanged(QStringLiteral("已向币号客户端 %1 下发上号指令").arg(clientId), ServerStatusTone::Neutral);
    return true;
}

void ServerController::startAccountLoginForAllCoins()
{
    int sent = 0;
    for (const ClientListItem& client : connectionManager_.clients())
    {
        if (!client.connected || client.role != ClientRole::Coin)
            continue;
        if (dispatchAccountLogin(client.id))
            ++sent;
    }
    emit statusChanged(QStringLiteral("已向 %1 个币号客户端执行一键上号请求").arg(sent),
        sent > 0 ? ServerStatusTone::Success : ServerStatusTone::Error);
}

void ServerController::handleAccountLoginReady(quint64 clientId)
{
    connectionManager_.forceResetClientAction(clientId);
    clearPendingAccountLoginRequests(clientId);
    emit statusChanged(QStringLiteral("客户端 %1 登录成功，500ms 后开始配装").arg(clientId), ServerStatusTone::Success);
    QTimer::singleShot(500, this, [this, clientId]() {
        QString error;
        if (!connectionManager_.requestRebindWindow(clientId, error)) {
            emit statusChanged(QStringLiteral("客户端 %1 重绑窗口失败：%2").arg(clientId).arg(error), ServerStatusTone::Error);
            return;
        }
        QTimer::singleShot(500, this, [this, clientId]() {
            QString error;
            const int requestId = nextLoginLoadoutRequestId_++;
            const LoginReadyState state = loginReadyStates_.value(clientId);
            pendingLoadoutRequests_.insert(requestId, clientId);
            pendingLoginReadyLoadouts_.insert(requestId, state);
            if (!connectionManager_.requestSaveLoadout(clientId, requestId, ClientRole::Coin,
                state.weapon, state.backpack, state.egg, state.bulletName,
                state.perClick, state.bulletTotal, error))
            {
                pendingLoadoutRequests_.remove(requestId);
                pendingLoginReadyLoadouts_.remove(requestId);
                emit statusChanged(QStringLiteral("客户端 %1 登录成功后的保存配装下发失败：%2")
                    .arg(clientId).arg(error), ServerStatusTone::Error);
                connectionManager_.forceResetClientAction(clientId);
                return;
            }
        });
    });
}

void ServerController::clearPendingAccountLoginRequests(quint64 clientId)
{
    // 清除同一客户端可能遗留的旧登录配装/上架请求，避免旧账号回包串到新账号。
    QList<int> staleLoginLoadoutRequests;
    for (auto iterator = pendingLoginReadyLoadouts_.cbegin(); iterator != pendingLoginReadyLoadouts_.cend(); ++iterator)
    {
        if (pendingLoadoutRequests_.value(iterator.key(), 0) == clientId)
        {
            staleLoginLoadoutRequests.push_back(iterator.key());
        }
    }
    for (int requestId : staleLoginLoadoutRequests)
    {
        pendingLoginReadyLoadouts_.remove(requestId);
        pendingLoadoutRequests_.remove(requestId);
    }

    QList<int> staleLoginListingRequests;
    for (auto iterator = pendingLoginReadyListings_.cbegin(); iterator != pendingLoginReadyListings_.cend(); ++iterator)
    {
        if (iterator.value() == clientId)
        {
            staleLoginListingRequests.push_back(iterator.key());
        }
    }
    for (int requestId : staleLoginListingRequests)
    {
        pendingLoginReadyListings_.remove(requestId);
    }
}

bool ServerController::markAccountBanned(quint64 clientId, QString& accountLine)
{
    for (AccountCredential& account : accounts_)
    {
        if (!account.assigned || account.banned || account.clientId != clientId)
        {
            continue;
        }
        account.banned = true;
        accountLine = account.username + QStringLiteral("----") + account.password;
        return true;
    }
    return false;
}

bool ServerController::appendBannedAccount(const QString& accountLine)
{
    const QString filePath = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("封号.txt"));
    QFile existing(filePath);
    if (existing.exists() && existing.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        while (!existing.atEnd())
        {
            if (QString::fromUtf8(existing.readLine()).trimmed() == accountLine)
            {
                existing.close();
                return true;
            }
        }
        existing.close();
    }

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
    {
        return false;
    }
    file.write(accountLine.toUtf8());
    file.write("\n");
    return file.flush();
}

void ServerController::setAutoExecuteThresholds(int bossCount, int coinCount)
{
    autoExecuteBossThreshold_ = qMax(1, bossCount);
    autoExecuteCoinThreshold_ = qMax(1, coinCount);
    autoExecuteTriggered_ = false;
}

void ServerController::setHighPriceStopThreshold(int threshold)
{
    highPriceStopThreshold_ = qBound(1, threshold, 99);
}

void ServerController::setAutoExecuteEnabled(bool enabled)
{
    autoExecuteEnabled_ = enabled;
    autoExecuteTriggered_ = false;
    if (autoExecuteEnabled_)
        autoExecuteTimer_.start();
    else
        autoExecuteTimer_.stop();
}

void ServerController::updateAutoExecuteParameters(const QString& countdownText, const QString& groupDelayText,
    bool coinFirst, bool compensationEnabled, const QString& compensationLeadText, const QString& bossPriceText)
{
    const int seconds = ParseCountdownSeconds(countdownText.toStdWString().c_str());
    bool delayOk = false;
    const int delay = groupDelayText.toInt(&delayOk);
    if (seconds >= 0 && delayOk && delay >= 0 && delay <= 600000)
    {
        lastTriggerCountdownText_ = countdownText;
        lastTriggerGroupDelayText_ = groupDelayText;
    }
    bool leadOk = false;
    const int lead = compensationLeadText.toInt(&leadOk);
    if (!compensationEnabled || (leadOk && lead >= 0 && lead <= 600000))
    {
        lastTriggerCoinFirst_ = coinFirst;
        lastTriggerCompensationEnabled_ = compensationEnabled;
        lastTriggerCompensationLeadText_ = compensationLeadText;
    }
    const QString price = bossPriceText.trimmed();
    bool priceOk = false;
    const double value = price.toDouble(&priceOk);
    if (price.isEmpty() || (priceOk && value > 0.0))
    {
        if (!price.isEmpty())
            bossListingPrice_ = price;
    }
}

void ServerController::processAutoExecuteTick()
{
    if (!autoExecuteEnabled_)
        return;
    int readyBosses = 0;
    int listedCoins = 0;
    for (const ClientListItem& client : connectionManager_.clients())
    {
        if (!client.connected || !client.ready || client.actionRunning) continue;
        if (client.role == ClientRole::Boss && client.teamReadyState == 1)
            ++readyBosses;
        if (client.role == ClientRole::Coin && client.listingStatus == QStringLiteral("成功"))
            ++listedCoins;
    }
    const int completeGroups = qMin(readyBosses / autoExecuteBossThreshold_,
        listedCoins / autoExecuteCoinThreshold_);
    if (!autoExecuteTriggered_ && completeGroups >= 1)
    {
        autoExecuteTriggered_ = true;
        triggerClients(lastTriggerCountdownText_, lastTriggerGroupDelayText_, lastTriggerCoinFirst_,
            lastTriggerCompensationEnabled_, lastTriggerCompensationLeadText_, bossListingPrice_, false, true);
        emit statusChanged(QStringLiteral("自动执行：老板 %1 个、币号 %2 个已就绪，已触发执行").arg(readyBosses).arg(listedCoins), ServerStatusTone::Success);
    }
    if (completeGroups == 0)
        autoExecuteTriggered_ = false;
}

void ServerController::handlePrepareResponse(quint64 clientId, int requestId, int status, const QString& text)
{
    if (!preparingExecution_ || !pendingPrepareRequests_.contains(requestId))
        return;
    if (status == 1)
    {
        emit connectionLogAppended(QStringLiteral("老板客户端 %1 准备失败后重试：%2").arg(clientId).arg(text));
        return;
    }
    pendingPrepareRequests_.remove(requestId);
    if (status < 0)
    {
        preparingExecution_ = false;
        pendingPrepareRequests_.clear();
        autoExecuteTriggered_ = false;
        emit statusChanged(QStringLiteral("老板客户端 %1 准备检查失败：%2").arg(clientId).arg(text), ServerStatusTone::Error);
        return;
    }
    emit connectionLogAppended(QStringLiteral("老板客户端 %1 准备检查成功：%2，剩余 %3 个").arg(clientId).arg(text).arg(pendingPrepareRequests_.size()));
    if (!pendingPrepareRequests_.isEmpty())
        return;

    preparingExecution_ = false;
    dispatchingPreparedExecution_ = true;
    triggerClients(pendingCountdownText_, pendingGroupDelayText_, pendingCoinFirst_,
        pendingCompensationEnabled_, pendingCompensationLeadText_, pendingBossPriceText_,
        pendingAllowIncomplete_, pendingAutomatic_);
    dispatchingPreparedExecution_ = false;
}
void ServerController::kickClient(quint64 clientId)
{
    if (connectionManager_.kickClient(clientId))
    {
        emit statusChanged(QStringLiteral("已踢出客户端"), ServerStatusTone::Neutral);
    }
}

int ServerController::parsePort(const QString& portText) const
{
    bool ok = false;
    const int port = portText.trimmed().toInt(&ok);
    return ok && port >= 1 && port <= 65535 ? port : -1;
}
