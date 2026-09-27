#include "ServerConnectionManager.h"

#include <ws2tcpip.h>

#include <algorithm>
#include <string>

#include <QDateTime>
#include <QRegularExpression>

#include "../Shared/Protocol.h"

ServerConnectionManager::ServerConnectionManager(QObject* parent)
    : QObject(parent)
{
}

ServerConnectionManager::~ServerConnectionManager()
{
    stopListening();
}

bool ServerConnectionManager::isListening() const
{
    return listening_.load();
}

bool ServerConnectionManager::startListening(int port, QString& errorText)
{
    if (listening_.load())
    {
        return true;
    }

    if (!wsa_.Ok())
    {
        errorText = QStringLiteral("Winsock 初始化失败");
        return false;
    }

    SOCKET socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socketHandle == INVALID_SOCKET)
    {
        errorText = QStringLiteral("创建监听 Socket 失败");
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<u_short>(port));

    if (bind(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
    {
        closesocket(socketHandle);
        errorText = QStringLiteral("端口绑定失败，可能已被占用");
        return false;
    }

    if (listen(socketHandle, SOMAXCONN) == SOCKET_ERROR)
    {
        closesocket(socketHandle);
        errorText = QStringLiteral("开始监听失败");
        return false;
    }

    listenSocket_ = socketHandle;
    listening_ = true;
    acceptThread_ = std::thread(&ServerConnectionManager::acceptLoop, this);
    emit listeningChanged(true);
    return true;
}

void ServerConnectionManager::stopListening()
{
    if (!listening_.exchange(false) && listenSocket_ == INVALID_SOCKET)
    {
        return;
    }

    SOCKET socketToClose = INVALID_SOCKET;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        socketToClose = listenSocket_;
        listenSocket_ = INVALID_SOCKET;
        for (const auto& client : clients_)
        {
            closeClientSocketLocked(client);
        }
    }

    if (socketToClose != INVALID_SOCKET)
    {
        shutdown(socketToClose, SD_BOTH);
        closesocket(socketToClose);
    }

    if (acceptThread_.joinable())
    {
        acceptThread_.join();
    }

    std::vector<std::shared_ptr<ClientEntry>> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = clients_;
    }

    for (const auto& client : snapshot)
    {
        if (client->receiveThread.joinable())
        {
            client->receiveThread.join();
        }
    }

    emit listeningChanged(false);
    emit clientsChanged();
}

int ServerConnectionManager::triggerReadyClients(int seconds, const QString& bossPrice)
{
    return triggerReadyClientsByRole(seconds, ClientRole::Coin, bossPrice)
        + triggerReadyClientsByRole(seconds, ClientRole::Boss, bossPrice);
}

int ServerConnectionManager::triggerReadyClientsByRole(int seconds, ClientRole role, const QString& bossPrice)
{
    std::vector<std::shared_ptr<ClientEntry>> readyClients;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& client : clients_)
        {
            if (client->connected && client->ready && client->role == role)
            {
                readyClients.push_back(client);
            }
        }
    }

    int sentCount = 0;
    const std::string triggerMessage = Protocol::BuildTriggerMessage(seconds, bossPrice.toUtf8().toStdString());
    // 同一角色组不主动插入延迟，连续写入 socket 以尽量让多客户端同时开始倒计时。
    for (const auto& client : readyClients)
    {
        if (SocketUtil::SendLine(client->socketHandle, triggerMessage))
        {
            ++sentCount;
            continue;
        }

        recordDisconnectReason(client, QStringLiteral("发送 TRIGGER 失败，错误码：%1").arg(WSAGetLastError()));
        markClientDisconnected(client->id);
    }
    return sentCount;
}


int ServerConnectionManager::triggerClientsByIds(int seconds, const QVector<quint64>& clientIds,
    const QString& bossPrice)
{
    std::vector<std::shared_ptr<ClientEntry>> selectedClients;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        selectedClients.reserve(clientIds.size());
        for (quint64 clientId : clientIds)
        {
            const auto client = findClientLocked(clientId);
            if (client && client->connected && client->ready)
            {
                // “执行”后币号会自动复用上架流程，先持有动作锁，确保 ListingResponse 不会被过滤。
                client->actionRunning = true;
                client->runningAction = QStringLiteral("执行中");
                selectedClients.push_back(client);
            }
        }
    }

    int sentCount = 0;
    const std::string triggerMessage = Protocol::BuildTriggerMessage(seconds, bossPrice.toUtf8().toStdString());
    for (const auto& client : selectedClients)
    {
        if (SocketUtil::SendLine(client->socketHandle, triggerMessage))
        {
            ++sentCount;
            continue;
        }
        recordDisconnectReason(client, QStringLiteral("发送指定客户端 TRIGGER 失败，错误码：%1").arg(WSAGetLastError()));
        {
            std::lock_guard<std::mutex> lock(mutex_);
            client->actionRunning = false;
            client->runningAction.clear();
        }
        markClientDisconnected(client->id);
    }
    emit clientsChanged();
    return sentCount;
}

bool ServerConnectionManager::requestTimeSync(quint64 clientId, int requestId, qint64 serverSendMs, QString& errorText)
{
    std::shared_ptr<ClientEntry> client;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        client = findClientLocked(clientId);
        if (!client || !client->connected)
        {
            errorText = QStringLiteral("客户端已断开");
            return false;
        }
        client->timeSyncSendTimesMs.insert(requestId, serverSendMs);
    }
    if (!SocketUtil::SendLine(client->socketHandle, Protocol::BuildTimeSyncRequestMessage(requestId, serverSendMs)))
    {
        errorText = QStringLiteral("发送时间同步请求失败，错误码：%1").arg(WSAGetLastError());
        markClientDisconnected(clientId);
        return false;
    }
    return true;
}

bool ServerConnectionManager::sendServerTime(quint64 clientId, qint64 serverTimeMs, qint64 clientOffsetMs, QString& errorText)
{
    std::shared_ptr<ClientEntry> client;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        client = findClientLocked(clientId);
        if (!client || !client->connected)
        {
            errorText = QStringLiteral("客户端已断开");
            return false;
        }
    }
    if (!SocketUtil::SendLine(client->socketHandle, Protocol::BuildServerTimeMessage(
        static_cast<long long>(serverTimeMs), static_cast<long long>(clientOffsetMs))))
    {
        errorText = QStringLiteral("发送服务器时间失败，错误码：%1").arg(WSAGetLastError());
        markClientDisconnected(clientId);
        return false;
    }
    return true;
}

bool ServerConnectionManager::sendScheduledTrigger(quint64 clientId, int seconds, qint64 targetClientMs,
    const QString& bossPrice, QString& errorText)
{
    std::shared_ptr<ClientEntry> client;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        client = findClientLocked(clientId);
        if (!client || !client->connected)
        {
            errorText = QStringLiteral("客户端已断开");
            return false;
        }
        if (client->actionRunning)
        {
            errorText = QStringLiteral("该客户端正在执行：%1").arg(client->runningAction);
            return false;
        }
        client->actionRunning = true;
        client->runningAction = QStringLiteral("执行中");
    }
    if (!SocketUtil::SendLine(client->socketHandle, Protocol::BuildScheduledTriggerMessage(
        seconds, targetClientMs, bossPrice.toUtf8().toStdString())))
    {
        errorText = QStringLiteral("发送定时出发指令失败，错误码：%1").arg(WSAGetLastError());
        {
            std::lock_guard<std::mutex> lock(mutex_);
            client->actionRunning = false;
            client->runningAction.clear();
        }
        markClientDisconnected(clientId);
        return false;
    }
    emit clientsChanged();
    return true;
}

bool ServerConnectionManager::triggerSingleClient(quint64 clientId, int seconds, QString& errorText)
{
    return triggerSingleClient(clientId, seconds, QString(), errorText);
}

bool ServerConnectionManager::triggerSingleClient(quint64 clientId, int seconds, const QString& bossPrice, QString& errorText)
{
    std::shared_ptr<ClientEntry> client;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        client = findClientLocked(clientId);
        if (!client || !client->connected)
        {
            errorText = QStringLiteral("客户端已断开");
            return false;
        }
        if (client->actionRunning)
        {
            errorText = QStringLiteral("该客户端正在执行：%1").arg(client->runningAction);
            return false;
        }
        client->actionRunning = true;
        client->runningAction = QStringLiteral("出发");
    }

    if (!SocketUtil::SendLine(client->socketHandle, Protocol::BuildTriggerMessage(
        seconds, bossPrice.toUtf8().toStdString())))
    {
        recordDisconnectReason(client, QStringLiteral("发送单客户端 TRIGGER 失败，错误码：%1").arg(WSAGetLastError()));
        markClientDisconnected(client->id);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        client->actionRunning = false;
        client->runningAction.clear();
    }

    emit clientsChanged();
    return true;
}

bool ServerConnectionManager::requestTeamCode(quint64 clientId, QString& errorText)
{
    std::shared_ptr<ClientEntry> client;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        client = findClientLocked(clientId);
        if (!client || !client->connected)
        {
            errorText = QStringLiteral("客户端已断开");
            return false;
        }
        if (client->actionRunning)
        {
            errorText = QStringLiteral("该客户端正在执行：%1").arg(client->runningAction);
            return false;
        }
        client->actionRunning = true;
        client->runningAction = QStringLiteral("获取组队码");
        client->teamCode = QStringLiteral("获取中...");
    }

    if (!SocketUtil::SendLine(client->socketHandle, Protocol::BuildTeamCodeRequestMessage()))
    {
        recordDisconnectReason(client, QStringLiteral("发送 TEAM_CODE_REQUEST 失败，错误码：%1").arg(WSAGetLastError()));
        markClientDisconnected(client->id);
        return false;
    }

    emit clientsChanged();
    return true;
}

bool ServerConnectionManager::requestTeamReady(quint64 clientId, QString& errorText)
{
    std::shared_ptr<ClientEntry> client;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        client = findClientLocked(clientId);
        if (!client || !client->connected)
        {
            errorText = QStringLiteral("客户端已断开");
            return false;
        }
        if (client->actionRunning)
        {
            errorText = QStringLiteral("该客户端正在执行：%1").arg(client->runningAction);
            return false;
        }
        client->actionRunning = true;
        client->runningAction = QStringLiteral("识别队伍状态");
    }

    // 队伍状态识别由服务端主动触发，客户端只在收到请求后执行一次 OCR。
    if (!SocketUtil::SendLine(client->socketHandle, Protocol::BuildTeamReadyRequestMessage()))
    {
        recordDisconnectReason(client, QStringLiteral("发送 TEAM_READY_REQUEST 失败，错误码：%1").arg(WSAGetLastError()));
        markClientDisconnected(client->id);
        return false;
    }

    emit clientsChanged();
    return true;
}

bool ServerConnectionManager::requestPrepare(quint64 clientId, int requestId, QString& errorText)
{
    std::shared_ptr<ClientEntry> client;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        client = findClientLocked(clientId);
        if (!client || !client->connected) { errorText = QStringLiteral("客户端已断开"); return false; }
        if (client->role != ClientRole::Boss) { errorText = QStringLiteral("执行前准备检查仅适用于老板"); return false; }
        if (client->actionRunning) { errorText = QStringLiteral("该客户端正在执行：%1").arg(client->runningAction); return false; }
        client->actionRunning = true;
        client->runningAction = QStringLiteral("执行前准备检查");
        client->teamReadyText = QStringLiteral("准备检查中");
        client->teamReadyState = -1;
    }
    if (!SocketUtil::SendLine(client->socketHandle, Protocol::BuildPrepareRequestMessage(requestId)))
    {
        recordDisconnectReason(client, QStringLiteral("发送 PREPARE_REQUEST 失败，错误码：%1").arg(WSAGetLastError()));
        markClientDisconnected(client->id);
        return false;
    }
    emit clientsChanged();
    return true;
}

bool ServerConnectionManager::requestRebindWindow(quint64 clientId, QString& errorText)
{
    std::shared_ptr<ClientEntry> client;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        client = findClientLocked(clientId);
        if (!client || !client->connected)
        {
            errorText = QStringLiteral("客户端已断开");
            return false;
        }
    }
    if (!SocketUtil::SendLine(client->socketHandle, Protocol::BuildRebindWindowRequestMessage()))
    {
        errorText = QStringLiteral("发送重新绑定窗口指令失败，错误码：%1").arg(WSAGetLastError());
        return false;
    }
    return true;
}

int ServerConnectionManager::requestRebindAllWindows()
{
    const QVector<ClientListItem> currentClients = clients();
    int sentCount = 0;
    for (const ClientListItem& item : currentClients)
    {
        QString errorText;
        if (item.connected && requestRebindWindow(item.id, errorText))
        {
            ++sentCount;
        }
    }
    return sentCount;
}

bool ServerConnectionManager::requestForceStopAction(quint64 clientId, QString& errorText)
{
    std::shared_ptr<ClientEntry> client;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        client = findClientLocked(clientId);
        if (!client || !client->connected)
        {
            errorText = QStringLiteral("客户端已断开");
            return false;
        }
    }
    if (!SocketUtil::SendLine(client->socketHandle, Protocol::BuildForceStopActionRequestMessage()))
    {
        errorText = QStringLiteral("发送强制停止指令失败，错误码：%1").arg(WSAGetLastError());
        return false;
    }
    return true;
}

int ServerConnectionManager::requestForceStopAllActions()
{
    const QVector<ClientListItem> currentClients = clients();
    int sentCount = 0;
    for (const ClientListItem& item : currentClients)
    {
        QString errorText;
        if (item.connected && requestForceStopAction(item.id, errorText))
        {
            forceResetClientAction(item.id);
            ++sentCount;
        }
    }
    return sentCount;
}

void ServerConnectionManager::resetAllClientStates()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& client : clients_)
        {
            client->actionRunning = false;
            client->runningAction.clear();
            client->listingStatus = QStringLiteral("待执行");
            client->listingQuantityX = -1;
            client->teamReadyText = QStringLiteral("未识别");
            client->teamReadyState = -1;
            client->teamReadyCurrent = 0;
            client->teamReadyTotal = 0;
            client->teamReadyUpdatedAtMs = 0;
        }
    }
    emit clientsChanged();
}

bool ServerConnectionManager::requestOcr(quint64 clientId, int requestId, const OcrRequestRegion& region, QString& errorText)
{
    std::shared_ptr<ClientEntry> client;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        client = findClientLocked(clientId);
        if (!client || !client->connected)
        {
            errorText = QStringLiteral("客户端已断开");
            return false;
        }
        if (client->actionRunning)
        {
            errorText = QStringLiteral("该客户端正在执行：%1").arg(client->runningAction);
            return false;
        }
        client->actionRunning = true;
        client->runningAction = QStringLiteral("OCR识别");
    }

    if (!SocketUtil::SendLine(client->socketHandle, Protocol::BuildOcrRequestMessage(requestId, region.x, region.y, region.width, region.height, region.referenceWidth, region.referenceHeight)))
    {
        recordDisconnectReason(client, QStringLiteral("发送 OCR_REQUEST 失败，错误码：%1").arg(WSAGetLastError()));
        markClientDisconnected(client->id);
        return false;
    }

    emit clientsChanged();
    return true;
}

bool ServerConnectionManager::requestSelfThunder(quint64 clientId, int requestId, QString& errorText)
{
    std::shared_ptr<ClientEntry> client;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        client = findClientLocked(clientId);
        if (!client || !client->connected)
        {
            errorText = QStringLiteral("客户端已断开");
            return false;
        }
        if (client->actionRunning)
        {
            errorText = QStringLiteral("该客户端正在执行：%1").arg(client->runningAction);
            return false;
        }
        client->actionRunning = true;
        client->runningAction = QStringLiteral("自雷中");
        client->teamReadyText = QStringLiteral("自雷中");
        client->teamReadyState = -1;
        client->teamReadyCurrent = 0;
        client->teamReadyTotal = 0;
        client->teamReadyUpdatedAtMs = QDateTime::currentMSecsSinceEpoch();
    }

    if (!SocketUtil::SendLine(client->socketHandle, Protocol::BuildSelfThunderRequestMessage(requestId)))
    {
        recordDisconnectReason(client, QStringLiteral("发送 SELF_THUNDER_REQUEST 失败，错误码：%1").arg(WSAGetLastError()));
        markClientDisconnected(client->id);
        return false;
    }

    emit clientsChanged();
    return true;
}

bool ServerConnectionManager::requestLoadout(quint64 clientId, int requestId, const QString& loadoutType, QString& errorText)
{
    std::shared_ptr<ClientEntry> client;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        client = findClientLocked(clientId);
        if (!client || !client->connected)
        {
            errorText = QStringLiteral("客户端已断开");
            return false;
        }
        if (client->actionRunning)
        {
            errorText = QStringLiteral("该客户端正在执行：%1").arg(client->runningAction);
            return false;
        }
        client->actionRunning = true;
        client->runningAction = QStringLiteral("配装");
        client->teamReadyText = QStringLiteral("配装中");
        client->teamReadyState = -1;
        client->teamReadyCurrent = 0;
        client->teamReadyTotal = 0;
        client->teamReadyUpdatedAtMs = QDateTime::currentMSecsSinceEpoch();
    }

    const Protocol::LoadoutType type = loadoutType == QStringLiteral("机密") ? Protocol::LoadoutType::Secret : Protocol::LoadoutType::Normal;
    if (!SocketUtil::SendLine(client->socketHandle, Protocol::BuildLoadoutRequestMessage(requestId, type)))
    {
        recordDisconnectReason(client, QStringLiteral("发送 LOADOUT_REQUEST 失败，错误码：%1").arg(WSAGetLastError()));
        markClientDisconnected(client->id);
        return false;
    }

    emit clientsChanged();
    return true;
}

bool ServerConnectionManager::requestListing(quint64 clientId, int requestId, const QString& listingText, const QString& listingPrice, bool hasExchange, int purchaseCount, bool loadoutMode, QString& errorText)
{
    std::shared_ptr<ClientEntry> client;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        client = findClientLocked(clientId);
        if (!client || !client->connected)
        {
            errorText = QStringLiteral("客户端已断开");
            return false;
        }
        if (client->role != ClientRole::Coin)
        {
            errorText = QStringLiteral("上架功能仅对币号生效");
            return false;
        }
        if (client->actionRunning)
        {
            errorText = QStringLiteral("该客户端正在执行：%1").arg(client->runningAction);
            return false;
        }
        client->actionRunning = true;
        client->runningAction = QStringLiteral("上架");
        client->listingStatus = QStringLiteral("执行中");
    }

    const std::string message = Protocol::BuildListingRequestMessage(
        requestId, hasExchange, purchaseCount, loadoutMode, listingPrice.toUtf8().toStdString(), listingText.toUtf8().toStdString());
    if (!SocketUtil::SendLine(client->socketHandle, message))
    {
        recordDisconnectReason(client, QStringLiteral("发送 LISTING_REQUEST 失败，错误码：%1").arg(WSAGetLastError()));
        markClientDisconnected(client->id);
        return false;
    }

    emit clientsChanged();
    return true;
}

bool ServerConnectionManager::requestSaveLoadout(quint64 clientId, int requestId, ClientRole targetRole, const QString& weapon,
    const QString& backpack, const QString& egg, const QString& bulletName, int perClick, int bulletTotal,
    QString& errorText)
{
    std::shared_ptr<ClientEntry> client;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        client = findClientLocked(clientId);
        if (!client || !client->connected)
        {
            errorText = QStringLiteral("客户端已断开");
            return false;
        }
        if (client->role != targetRole)
        {
            errorText = QStringLiteral("客户端角色不匹配");
            return false;
        }
        if (client->actionRunning)
        {
            errorText = QStringLiteral("该客户端正在执行：%1").arg(client->runningAction);
            return false;
        }
        client->actionRunning = true;
        client->runningAction = QStringLiteral("保存配装");
    }

    const std::string message = Protocol::BuildSaveLoadoutRequestMessage(
        requestId, weapon.toUtf8().toStdString(), backpack.toUtf8().toStdString(), egg.toUtf8().toStdString(),
        bulletName.toUtf8().toStdString(), perClick, bulletTotal);
    if (!SocketUtil::SendLine(client->socketHandle, message))
    {
        recordDisconnectReason(client, QStringLiteral("发送 SAVE_LOADOUT_REQUEST 失败，错误码：%1").arg(WSAGetLastError()));
        markClientDisconnected(client->id);
        return false;
    }
    emit clientsChanged();
    return true;
}

bool ServerConnectionManager::requestMail(quint64 clientId, int requestId, QString& errorText)
{
    std::shared_ptr<ClientEntry> client;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        client = findClientLocked(clientId);
        if (!client || !client->connected) { errorText = QStringLiteral("客户端已断开"); return false; }
        if (client->actionRunning) { errorText = QStringLiteral("该客户端正在执行：%1").arg(client->runningAction); return false; }
        client->actionRunning = true;
        client->runningAction = QStringLiteral("领取邮件");
    }
    if (!SocketUtil::SendLine(client->socketHandle, Protocol::BuildMailRequestMessage(requestId)))
    {
        recordDisconnectReason(client, QStringLiteral("发送 MAIL_REQUEST 失败，错误码：%1").arg(WSAGetLastError()));
        markClientDisconnected(client->id);
        return false;
    }
    emit clientsChanged();
    return true;
}

bool ServerConnectionManager::requestAccountLogin(quint64 clientId, int requestId, const QString& username, const QString& password, QString& errorText)
{
    std::shared_ptr<ClientEntry> client;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        client = findClientLocked(clientId);
        if (!client || !client->connected) { errorText = QStringLiteral("客户端已断开"); return false; }
        if (client->role != ClientRole::Coin) { errorText = QStringLiteral("仅币号客户端支持上号"); return false; }
        if (client->actionRunning) { errorText = QStringLiteral("该客户端正在执行：%1").arg(client->runningAction); return false; }
        client->actionRunning = true;
        client->runningAction = QStringLiteral("上号中");
    }
    if (!SocketUtil::SendLine(client->socketHandle, Protocol::BuildAccountLoginRequestMessage(
        requestId, username.toUtf8().toStdString(), password.toUtf8().toStdString())))
    {
        errorText = QStringLiteral("发送上号指令失败");
        markClientDisconnected(clientId);
        return false;
    }
    emit clientsChanged();
    return true;
}
bool ServerConnectionManager::setClientTeamReadyDisplay(quint64 clientId, const QString& text, int state)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto client = findClientLocked(clientId);
        if (!client)
        {
            return false;
        }
        client->teamReadyText = text;
        client->teamReadyState = state;
        client->teamReadyCurrent = 0;
        client->teamReadyTotal = 0;
        client->teamReadyUpdatedAtMs = QDateTime::currentMSecsSinceEpoch();
    }

    emit clientsChanged();
    return true;
}

bool ServerConnectionManager::resetClientTeamReadyDisplay(quint64 clientId)
{
    return setClientTeamReadyDisplay(clientId, QStringLiteral("未识别"), -1);
}

bool ServerConnectionManager::forceResetClientAction(quint64 clientId)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto client = findClientLocked(clientId);
        if (!client)
        {
            return false;
        }
        client->actionRunning = false;
        client->runningAction.clear();
        // 单客户端停止与“停止所有动作”保持相同的业务状态重置范围，
        // 但不影响其他客户端的任务和金额统计。
        client->listingStatus = client->role == ClientRole::Coin
            ? QStringLiteral("待执行") : QStringLiteral("不适用");
        client->listingQuantityX = -1;
        client->teamReadyText = QStringLiteral("未识别");
        client->teamReadyState = -1;
        client->teamReadyCurrent = 0;
        client->teamReadyTotal = 0;
        client->teamReadyUpdatedAtMs = 0;
        if (client->teamCode == QStringLiteral("获取中..."))
            client->teamCode.clear();
    }

    emit clientsChanged();
    return true;
}

bool ServerConnectionManager::setClientTeamCode(quint64 clientId, const QString& teamCode, QString& errorText)
{
    const std::string payload = teamCode.trimmed().toStdString();
    if (!payload.empty() && !Protocol::IsValidTeamCodePayload(payload))
    {
        errorText = QStringLiteral("组队码必须是 3 个大写字母 + 4 个数字，例如 ABC1234");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto client = findClientLocked(clientId);
        if (!client)
        {
            errorText = QStringLiteral("客户端不存在");
            return false;
        }
        client->teamCode = QString::fromStdString(payload);
    }

    emit clientsChanged();
    return true;
}

bool ServerConnectionManager::setClientRemark(quint64 clientId, const QString& remark)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto client = findClientLocked(clientId);
        if (!client)
        {
            return false;
        }
        client->remark = remark;
    }

    emit clientsChanged();
    return true;
}


bool ServerConnectionManager::setClientRole(quint64 clientId, ClientRole role)
{
    std::shared_ptr<ClientEntry> client;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        client = findClientLocked(clientId);
        if (!client)
        {
            return false;
        }
        client->role = role;
        client->listingStatus = role == ClientRole::Coin ? QStringLiteral("待执行") : QStringLiteral("不适用");
    }

    // 角色是客户端选择固定出发坐标的唯一依据，服务端修改后立即同步。
    if (client->connected && !SocketUtil::SendLine(client->socketHandle, Protocol::BuildRoleMessage(role)))
    {
        recordDisconnectReason(client, QStringLiteral("发送 ROLE 失败，错误码：%1").arg(WSAGetLastError()));
        markClientDisconnected(client->id);
        return false;
    }

    emit clientsChanged();
    return true;
}

bool ServerConnectionManager::setClientListingStatus(quint64 clientId, const QString& status)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto client = findClientLocked(clientId);
        if (!client)
        {
            return false;
        }
        client->listingStatus = status;
    }
    emit clientsChanged();
    return true;
}

bool ServerConnectionManager::resetClientListingQuantity(quint64 clientId)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto client = findClientLocked(clientId);
        if (!client)
        {
            return false;
        }
        client->listingQuantityX = -1;
    }
    emit clientsChanged();
    return true;
}

bool ServerConnectionManager::setBossExpectedAmount(quint64 clientId, qint64 amount, QString& errorText)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto client = findClientLocked(clientId);
    if (!client || client->role != ClientRole::Boss)
    {
        errorText = QStringLiteral("客户端不是老板或已断开");
        return false;
    }
    client->expectedAmount = amount < 0 ? QStringLiteral("-") : QString::number(amount);
    return true;
}

bool ServerConnectionManager::updateBossAmount(quint64 clientId, qint64 amount, QString& errorText)
{
    bool reachedExpected = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto client = findClientLocked(clientId);
        if (!client)
        {
            errorText = QStringLiteral("客户端已断开");
            return false;
        }
        const bool hasPrevious = client->hasAmountBaseline;
        if (client->role == ClientRole::Boss && hasPrevious)
        {
            const qint64 previous = client->currentAmount.toLongLong();
            const qint64 delta = amount - previous;
            client->cumulativeIncome += delta;
            if (amount > previous)
            {
                ++client->lowPriceCount;
                client->consecutiveHighPriceCount = 0;
            }
            else if (amount < previous)
            {
                ++client->highPriceCount;
                ++client->consecutiveHighPriceCount;
            }
        }
        client->currentAmount = QString::number(amount);
        client->hasAmountBaseline = true;
        reachedExpected = client->role == ClientRole::Boss
            && client->expectedAmount != QStringLiteral("-")
            && amount >= client->expectedAmount.toLongLong();
    }
    emit clientsChanged();
    if (reachedExpected)
        errorText = QStringLiteral("期望金额已达到");
    return true;
}

void ServerConnectionManager::resetListingQuantities()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& client : clients_)
        {
            client->listingQuantityX = -1;
            if (client->role == ClientRole::Coin && client->connected)
            {
                client->listingStatus = QStringLiteral("待执行");
            }
        }
    }
    emit clientsChanged();
}
int ServerConnectionManager::coinClientCount(bool readyOnly) const
{
    int count = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& client : clients_)
    {
        if (client->connected && client->role == ClientRole::Coin && (!readyOnly || client->ready))
        {
            ++count;
        }
    }
    return count;
}

bool ServerConnectionManager::kickClient(quint64 clientId)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto client = findClientLocked(clientId);
        if (!client)
        {
            return false;
        }
        closeClientSocketLocked(client);
    }

    emit clientsChanged();
    return true;
}

QVector<ClientListItem> ServerConnectionManager::clients() const
{
    QVector<ClientListItem> result;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& client : clients_)
    {
        // 列表只展示当前有效连接，断开的历史连接不再占用界面行。
        if (!client->connected)
        {
            continue;
        }

        ClientListItem item;
        item.id = client->id;
        item.ip = client->ip;
        item.ready = client->ready;
        item.remark = client->remark;
        item.role = client->role;
        item.teamReadyText = client->teamReadyText;
        item.teamReadyState = client->teamReadyState;
        item.teamReadyCurrent = client->teamReadyCurrent;
        item.teamReadyTotal = client->teamReadyTotal;
        item.teamReadyUpdatedAtMs = client->teamReadyUpdatedAtMs;
        item.teamCode = client->teamCode;
        item.runningAction = client->runningAction;
    item.listingStatus = client->listingStatus;
    item.listingQuantityX = client->listingQuantityX;
    item.currentAmount = client->currentAmount;
    item.expectedAmount = client->expectedAmount;
    item.cumulativeIncome = client->cumulativeIncome;
    item.lowPriceCount = client->lowPriceCount;
    item.highPriceCount = client->highPriceCount;
    item.consecutiveHighPriceCount = client->consecutiveHighPriceCount;
    item.hasAmountBaseline = client->hasAmountBaseline;
        item.actionRunning = client->actionRunning;
        item.connected = client->connected;
        result.push_back(item);
    }
    return result;
}

void ServerConnectionManager::acceptLoop()
{
    while (listening_.load())
    {
        sockaddr_in clientAddress{};
        int addressLength = sizeof(clientAddress);
        SOCKET clientSocket = accept(listenSocket_, reinterpret_cast<sockaddr*>(&clientAddress), &addressLength);
        if (clientSocket == INVALID_SOCKET)
        {
            if (listening_.load())
            {
                emit networkError(QStringLiteral("接受客户端连接失败"));
            }
            break;
        }

        char ipBuffer[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &clientAddress.sin_addr, ipBuffer, sizeof(ipBuffer));

        auto client = std::make_shared<ClientEntry>();
        client->socketHandle = clientSocket;
        client->ip = QString::fromLatin1(ipBuffer);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            client->id = nextClientId_++;
            clients_.push_back(client);
        }

        // 新连接默认为币号，先下发角色，确保第一次 TRIGGER 就使用正确固定点。
        if (!SocketUtil::SendLine(client->socketHandle, Protocol::BuildRoleMessage(client->role)))
        {
            recordDisconnectReason(client, QStringLiteral("初始角色同步失败，错误码：%1").arg(WSAGetLastError()));
            markClientDisconnected(client->id);
            continue;
        }

        // 每个客户端独立接收 READY 状态，避免单个连接阻塞影响其他客户端。
        client->receiveThread = std::thread(&ServerConnectionManager::receiveLoop, this, client);
        // 连接建立后立即校准一次时间；使用协议允许的保留编号，
        // 延迟补偿任务使用控制器分配的更高编号。
        QString syncError;
        const qint64 initialSyncSendMs = QDateTime::currentMSecsSinceEpoch();
        if (!requestTimeSync(client->id, Protocol::InitialTimeSyncRequestId, initialSyncSendMs, syncError))
        {
            recordDisconnectReason(client, syncError);
        }
        emit clientsChanged();
    }
}

void ServerConnectionManager::receiveLoop(std::shared_ptr<ClientEntry> client)
{
    std::string buffer;
    char temp[512]{};

    while (listening_.load())
    {
        int received = recv(client->socketHandle, temp, sizeof(temp), 0);
        if (received == 0)
        {
            recordDisconnectReason(client, QStringLiteral("recv 返回 0，对端已关闭连接"));
            break;
        }
        if (received == SOCKET_ERROR)
        {
            recordDisconnectReason(client, QStringLiteral("Socket 接收失败，错误码：%1").arg(WSAGetLastError()));
            break;
        }

        buffer.append(temp, temp + received);
        size_t lineEnd = std::string::npos;
        while ((lineEnd = buffer.find('\n')) != std::string::npos)
        {
            std::string line = buffer.substr(0, lineEnd);
            buffer.erase(0, lineEnd + 1);
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }

            Protocol::ParsedMessage message{};
            if (!Protocol::ParseLine(line, message))
            {
                continue;
            }

            if (message.type == Protocol::MessageType::Ready)
            {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    client->ready = message.value == 1;
                }
                emit clientsChanged();
            }
            else if (message.type == Protocol::MessageType::TimeSyncResponse)
            {
                qint64 serverSendMs = 0;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    serverSendMs = client->timeSyncSendTimesMs.take(message.requestId);
                }
                if (serverSendMs <= 0)
                    serverSendMs = QDateTime::currentMSecsSinceEpoch();
                emit timeSyncResponseReceived(client->id, message.requestId,
                    static_cast<qint64>(message.timestampMs), static_cast<qint64>(message.timestampMs2),
                    serverSendMs, QDateTime::currentMSecsSinceEpoch());
            }
            else if (message.type == Protocol::MessageType::TeamCodeResponse)
            {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    // 获取组队码流程结束后释放该客户端动作锁，避免后续按钮一直不可用。
                    client->actionRunning = false;
                    client->runningAction.clear();
                    if (Protocol::IsValidTeamCodePayload(message.payload))
                    {
                        client->teamCode = QString::fromStdString(message.payload);
                    }
                    else if (message.payload == "INVALID")
                    {
                        client->teamCode = QStringLiteral("无效");
                    }
                    else
                    {
                        client->teamCode = QStringLiteral("失败");
                    }
                }
                emit clientsChanged();
            }
            else if (message.type == Protocol::MessageType::TeamReady)
            {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    // 队伍准备状态独立显示，不混入 READY 准备状态列。
                    client->actionRunning = false;
                    client->runningAction.clear();
                    client->teamReadyState = message.value;
                    client->teamReadyCurrent = message.current;
                    client->teamReadyTotal = message.total;
                    client->teamReadyUpdatedAtMs = QDateTime::currentMSecsSinceEpoch();
                    client->teamReadyText = message.value < 0
                        ? QStringLiteral("未识别")
                        : QStringLiteral("[%1/%2]").arg(message.current).arg(message.total);
                }
                emit clientsChanged();
            }
            else if (message.type == Protocol::MessageType::PrepareResponse)
            {
                const QString responseText = QString::fromUtf8(message.payload.data(), static_cast<int>(message.payload.size()));
                if (message.value != 1)
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    client->actionRunning = false;
                    client->runningAction.clear();
                    client->teamReadyState = message.success ? 1 : 0;
                    client->teamReadyText = message.success ? QStringLiteral("准备成功") : QStringLiteral("准备失败");
                    client->teamReadyUpdatedAtMs = QDateTime::currentMSecsSinceEpoch();
                }
                else
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    client->teamReadyText = QStringLiteral("准备失败，ESC 后重试");
                    client->teamReadyState = 0;
                    client->teamReadyUpdatedAtMs = QDateTime::currentMSecsSinceEpoch();
                }
                emit clientsChanged();
                emit prepareResponseReceived(client->id, message.requestId, message.value, responseText);
            }
            else if (message.type == Protocol::MessageType::OcrResponse)
            {
                const QString responseText = QString::fromStdString(message.payload);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    // 通用 OCR 请求结束后释放动作锁，由上层按 requestId 决定如何消费结果。
                    client->actionRunning = false;
                    client->runningAction.clear();
                }
                emit clientsChanged();
                emit ocrResponseReceived(client->id, message.requestId, message.success, responseText);
            }
            else if (message.type == Protocol::MessageType::SelfThunderResponse)
            {
                const QString responseText = QString::fromStdString(message.payload);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    // 自雷流程由客户端串行动作，回包后释放该客户端动作锁。
                    client->actionRunning = false;
                    client->runningAction.clear();
                    client->teamReadyText = message.success ? QStringLiteral("自雷完成") : QStringLiteral("自雷失败");
                    client->teamReadyState = -1;
                    client->teamReadyCurrent = 0;
                    client->teamReadyTotal = 0;
                    client->teamReadyUpdatedAtMs = QDateTime::currentMSecsSinceEpoch();
                }
                emit clientsChanged();
                emit selfThunderResponseReceived(client->id, message.requestId, message.success, responseText);
            }
            else if (message.type == Protocol::MessageType::LoadoutResponse)
            {
                const QString responseText = QString::fromStdString(message.payload);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    // 配装流程结束后释放动作锁，自动开始再根据 requestId 决定是否出发。
                    client->actionRunning = false;
                    client->runningAction.clear();
                    client->teamReadyText = message.success ? QStringLiteral("配装完成") : QStringLiteral("配装失败");
                    client->teamReadyState = -1;
                    client->teamReadyCurrent = 0;
                    client->teamReadyTotal = 0;
                    client->teamReadyUpdatedAtMs = QDateTime::currentMSecsSinceEpoch();
                }
                emit clientsChanged();
                emit loadoutResponseReceived(client->id, message.requestId, message.success, responseText);
            }
            else if (message.type == Protocol::MessageType::SaveLoadoutResponse)
            {
                const QString responseText = QString::fromUtf8(message.payload.data(), static_cast<int>(message.payload.size()));
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    client->actionRunning = false;
                    client->runningAction.clear();
                    client->teamReadyText = message.success ? QStringLiteral("配装保存完成") : QStringLiteral("配装保存失败");
                    client->teamReadyState = -1;
                    client->teamReadyCurrent = 0;
                    client->teamReadyTotal = 0;
                    client->teamReadyUpdatedAtMs = QDateTime::currentMSecsSinceEpoch();
                }
                emit clientsChanged();
                emit saveLoadoutResponseReceived(client->id, message.requestId, message.success, responseText);
            }
            else if (message.type == Protocol::MessageType::ListingResponse)
            {
                const QString responseText = QString::fromUtf8(message.payload.data(), static_cast<int>(message.payload.size()));
                bool acceptResponse = false;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    // 停止动作后服务端已清除 actionRunning；此时到达的旧上架回包
                    // 不能再次把“待执行”覆盖成“失败[0/N]”。
                    if (client->actionRunning)
                    {
                        // 上架回包后释放动作锁，并将 OCR 最终判断直接显示在对应币号行。
                        client->actionRunning = false;
                        client->runningAction.clear();
                        const QRegularExpression quantityPattern(QStringLiteral(R"((\d+)\s*/\s*(\d+))"));
                        const QRegularExpressionMatch quantityMatch = quantityPattern.match(responseText);
                        client->listingQuantityX = quantityMatch.hasMatch() ? quantityMatch.captured(1).toInt() : -1;
                        if (message.success)
                        {
                            client->listingStatus = QStringLiteral("成功");
                        }
                        else if (responseText.contains(QStringLiteral("使用量少")))
                        {
                            // 数量不足是 OCR 的有效结果，不应在列表中归类为通用失败。
                            client->listingStatus = QStringLiteral("少");
                        }
                        else if (responseText.contains(QStringLiteral("使用量超出")))
                        {
                            client->listingStatus = QStringLiteral("超出");
                        }
                        else
                        {
                            client->listingStatus = QStringLiteral("失败");
                        }
                        acceptResponse = true;
                    }
                }
                if (acceptResponse)
                {
                    emit clientsChanged();
                    emit listingResponseReceived(client->id, message.requestId, message.success, responseText);
                }
            }
            else if (message.type == Protocol::MessageType::MailResponse)
            {
                const QString responseText = QString::fromUtf8(message.payload.data(), static_cast<int>(message.payload.size()));
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    client->actionRunning = false;
                    client->runningAction.clear();
                }
                emit clientsChanged();
                emit mailResponseReceived(client->id, message.requestId, message.success, responseText);
            }
            else if (message.type == Protocol::MessageType::AccountLoginResponse)
            {
                const QString responseText = QString::fromUtf8(message.payload.data(), static_cast<int>(message.payload.size()));
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (message.value != 1)
                    {
                        client->actionRunning = false;
                        client->runningAction.clear();
                    }
                    client->teamReadyText = message.value == 1 ? QStringLiteral("上号中")
                        : (message.success ? QStringLiteral("上号成功") : QStringLiteral("上号失败"));
                }
                emit clientsChanged();
                emit accountLoginResponseReceived(client->id, message.requestId, message.value, responseText);
            }
            else if (message.type == Protocol::MessageType::AmountReport)
            {
                emit amountReportReceived(client->id, static_cast<qint64>(message.timestampMs));
            }
            else if (message.type == Protocol::MessageType::Ping)
            {
                // 收到客户端心跳后立即回包，让两端都能尽早发现断线。
                if (!SocketUtil::SendLine(client->socketHandle, Protocol::BuildPongMessage()))
                {
                    recordDisconnectReason(client, QStringLiteral("发送 PONG 失败，错误码：%1").arg(WSAGetLastError()));
                    markClientDisconnected(client->id);
                    return;
                }
            }
        }
    }

    markClientDisconnected(client->id);
}

void ServerConnectionManager::markClientDisconnected(quint64 clientId)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto client = findClientLocked(clientId);
        if (!client)
        {
            return;
        }
        closeClientSocketLocked(client);
    }

    emit clientsChanged();
}

void ServerConnectionManager::recordDisconnectReason(const std::shared_ptr<ClientEntry>& client, const QString& reason)
{
    if (!client)
    {
        return;
    }

    emit networkError(QStringLiteral("客户端 %1 断开：%2").arg(client->ip, reason));
}

std::shared_ptr<ServerConnectionManager::ClientEntry> ServerConnectionManager::findClientLocked(quint64 clientId) const
{
    auto iterator = std::find_if(clients_.begin(), clients_.end(), [clientId](const std::shared_ptr<ClientEntry>& client) {
        return client->id == clientId;
    });
    return iterator == clients_.end() ? nullptr : *iterator;
}

void ServerConnectionManager::closeClientSocketLocked(const std::shared_ptr<ClientEntry>& client)
{
    if (!client || client->socketHandle == INVALID_SOCKET)
    {
        return;
    }

    client->connected = false;
    client->ready = false;
    const SOCKET socketHandle = client->socketHandle;
    client->socketHandle = INVALID_SOCKET;
    shutdown(socketHandle, SD_BOTH);
    closesocket(socketHandle);
}
