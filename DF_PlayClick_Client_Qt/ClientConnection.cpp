#include "ClientConnection.h"

#include <ws2tcpip.h>

#include <QMetaObject>
#include <QDateTime>

#include "../Shared/ConnectionLifecyclePolicy.h"
#include "../Shared/Protocol.h"

namespace
{
bool ConnectSocketWithTimeout(SOCKET socketHandle, const sockaddr_in& serverAddress, int timeoutMs, int& errorCode)
{
    u_long nonBlocking = 1;
    if (ioctlsocket(socketHandle, FIONBIO, &nonBlocking) == SOCKET_ERROR)
    {
        errorCode = WSAGetLastError();
        return false;
    }

    const int connectResult = ::connect(socketHandle,
        reinterpret_cast<const sockaddr*>(&serverAddress), sizeof(serverAddress));
    if (connectResult == 0)
    {
        nonBlocking = 0;
        ioctlsocket(socketHandle, FIONBIO, &nonBlocking);
        return true;
    }

    const int connectError = WSAGetLastError();
    if (connectError != WSAEWOULDBLOCK && connectError != WSAEINPROGRESS && connectError != WSAEALREADY)
    {
        errorCode = connectError;
        return false;
    }

    fd_set writeSet;
    FD_ZERO(&writeSet);
    FD_SET(socketHandle, &writeSet);
    fd_set errorSet;
    FD_ZERO(&errorSet);
    FD_SET(socketHandle, &errorSet);
    timeval timeout{};
    timeout.tv_sec = timeoutMs / 1000;
    timeout.tv_usec = (timeoutMs % 1000) * 1000;

    const int selectResult = ::select(0, nullptr, &writeSet, &errorSet, &timeout);
    if (selectResult <= 0)
    {
        errorCode = selectResult == 0 ? WSAETIMEDOUT : WSAGetLastError();
        return false;
    }

    int socketError = 0;
    int socketErrorSize = sizeof(socketError);
    if (getsockopt(socketHandle, SOL_SOCKET, SO_ERROR,
        reinterpret_cast<char*>(&socketError), &socketErrorSize) == SOCKET_ERROR)
    {
        errorCode = WSAGetLastError();
        return false;
    }
    if (socketError != 0)
    {
        errorCode = socketError;
        return false;
    }

    nonBlocking = 0;
    ioctlsocket(socketHandle, FIONBIO, &nonBlocking);
    return true;
}
}

ClientConnection::ClientConnection(QObject* parent)
    : QObject(parent)
{
    heartbeatTimer_.setInterval(ConnectionLifecyclePolicy::HeartbeatIntervalMs);
    reconnectTimer_.setInterval(ConnectionLifecyclePolicy::ReconnectInitialIntervalMs);
    connect(&heartbeatTimer_, &QTimer::timeout, this, &ClientConnection::sendHeartbeat);
    connect(&reconnectTimer_, &QTimer::timeout, this, &ClientConnection::reconnectToServer);
}

ClientConnection::~ClientConnection()
{
    disconnectFromServer();
}

bool ClientConnection::isConnected() const
{
    return connected_;
}

bool ClientConnection::isReconnecting() const
{
    return reconnectTimer_.isActive();
}

void ClientConnection::connectToServer(const QString& ip, int port)
{
    if (connected_)
    {
        return;
    }

    serverIp_ = ip;
    serverPort_ = port;
    reconnectAttempt_ = 0;
    userRequestedDisconnect_ = false;
    autoReconnectSuppressed_ = false;
    stopReconnect();
    if (!tryConnectToStoredServer(false))
    {
        userRequestedDisconnect_ = true;
    }
}

void ClientConnection::disconnectFromServer()
{
    userRequestedDisconnect_ = true;
    desiredReady_ = false;
    stopReconnect();
    const bool wasConnected = connected_.exchange(false);
    heartbeatTimer_.stop();
    closeSocketOnly();
    if (receiveThread_.joinable())
    {
        receiveThread_.join();
    }
    if (wasConnected)
    {
        appendConnectionLog(QStringLiteral("本机主动断开服务端连接"));
        emit connectedChanged(false);
    }
    emit statusChanged(QStringLiteral("已断开服务端"));
}

void ClientConnection::suppressAutoReconnect()
{
    autoReconnectSuppressed_ = true;
    userRequestedDisconnect_ = true;
}

bool ClientConnection::tryConnectToStoredServer(bool reconnecting)
{
    if (connected_)
    {
        return true;
    }

    if (!wsa_.Ok())
    {
        appendConnectionLog(QStringLiteral("Winsock 初始化失败"));
        emit statusChanged(QStringLiteral("Winsock 初始化失败"));
        return false;
    }

    SOCKET socketHandle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socketHandle == INVALID_SOCKET)
    {
        appendConnectionLog(QStringLiteral("创建套接字失败，错误码：%1").arg(WSAGetLastError()));
        emit statusChanged(QStringLiteral("创建套接字失败"));
        return false;
    }

    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(static_cast<u_short>(serverPort_));
    const QByteArray ipBytes = serverIp_.toUtf8();
    if (inet_pton(AF_INET, ipBytes.constData(), &serverAddress.sin_addr) != 1)
    {
        closesocket(socketHandle);
        appendConnectionLog(QStringLiteral("服务端 IP 格式无效：%1").arg(serverIp_));
        emit statusChanged(QStringLiteral("服务端 IP 格式无效"));
        return false;
    }

    int errorCode = 0;
    if (!ConnectSocketWithTimeout(socketHandle, serverAddress, ConnectionLifecyclePolicy::ConnectTimeoutMs, errorCode))
    {
        closesocket(socketHandle);
        if (!reconnecting)
        {
            appendConnectionLog(QStringLiteral("连接服务端失败，错误码：%1").arg(errorCode));
            emit statusChanged(QStringLiteral("连接服务端失败"));
        }
        else
        {
            emit statusChanged(QStringLiteral("重连服务端失败，等待下次重试，错误码：%1").arg(errorCode));
        }
        return false;
    }

    socket_ = socketHandle;
    heartbeatMisses_ = 0;
    connected_ = true;
    if (!SocketUtil::SendLine(socket_, Protocol::BuildHelloMessage()))
    {
        appendConnectionLog(QStringLiteral("连接建立后发送 HELLO 失败，错误码：%1").arg(WSAGetLastError()));
        closesocket(socketHandle);
        socket_ = INVALID_SOCKET;
        connected_ = false;
        emit statusChanged(QStringLiteral("连接建立后发送 HELLO 失败"));
        return false;
    }

    // 客户端不再提供准备按钮；每次连接成功后都自动进入准备状态。
    desiredReady_ = true;
    if (!SocketUtil::SendLine(socket_, Protocol::BuildReadyMessage(true)))
    {
        appendConnectionLog(QStringLiteral("连接后发送默认准备状态失败，错误码：%1").arg(WSAGetLastError()));
        connected_ = false;
        closesocket(socketHandle);
        socket_ = INVALID_SOCKET;
        emit statusChanged(QStringLiteral("连接后发送默认准备状态失败"));
        return false;
    }

    stopReconnect();
    reconnectAttempt_ = 0;
    appendConnectionLog(reconnecting
        ? QStringLiteral("已重新连接服务端 %1:%2").arg(serverIp_).arg(serverPort_)
        : QStringLiteral("已连接服务端 %1:%2").arg(serverIp_).arg(serverPort_));
    emit connectedChanged(true);
    emit statusChanged(reconnecting ? QStringLiteral("已重新连接服务端") : QStringLiteral("已连接服务端"));
    heartbeatTimer_.start();

    const quint64 generation = ++connectionGeneration_;
    receiveThread_ = std::thread(&ClientConnection::receiveLoop, this, socketHandle, generation);
    return true;
}

bool ClientConnection::startReconnect()
{
    if (autoReconnectSuppressed_.load())
    {
        return false;
    }
    if (!ConnectionLifecyclePolicy::ShouldAutoReconnect(userRequestedDisconnect_, !serverIp_.isEmpty() && serverPort_ > 0))
    {
        return false;
    }
    if (reconnectTimer_.isActive())
    {
        return true;
    }

    // 非手动断开时进入自动重连，避免短暂网络/服务端重启导致客户端停在离线状态。
    appendConnectionLog(QStringLiteral("连接中断，正在自动重连服务端 %1:%2").arg(serverIp_).arg(serverPort_));
    emit statusChanged(QStringLiteral("连接中断，正在自动重连"));
    reconnectAttempt_ = 0;
    reconnectTimer_.start(ConnectionLifecyclePolicy::ReconnectDelayMs(reconnectAttempt_));
    emit reconnectingChanged(true);
    return true;
}

void ClientConnection::stopReconnect()
{
    if (!reconnectTimer_.isActive())
    {
        return;
    }

    reconnectTimer_.stop();
    reconnectAttempt_ = 0;
    emit reconnectingChanged(false);
}

void ClientConnection::reconnectToServer()
{
    if (connected_)
    {
        stopReconnect();
        return;
    }

    if (!tryConnectToStoredServer(true))
    {
        ++reconnectAttempt_;
        reconnectTimer_.start(ConnectionLifecyclePolicy::ReconnectDelayMs(reconnectAttempt_));
    }
}

bool ClientConnection::sendReady(bool ready)
{
    if (!connected_ || socket_ == INVALID_SOCKET)
    {
        return false;
    }

    if (!SocketUtil::SendLine(socket_, Protocol::BuildReadyMessage(ready)))
    {
        handleConnectionLost(QStringLiteral("发送准备状态失败，服务端连接已断开，错误码：%1").arg(WSAGetLastError()));
        return false;
    }

    desiredReady_ = ready;
    return true;
}

bool ClientConnection::sendTeamCodeResponse(const QString& payload)
{
    if (!connected_ || socket_ == INVALID_SOCKET)
    {
        return false;
    }

    if (!SocketUtil::SendLine(socket_, Protocol::BuildTeamCodeResponseMessage(payload.toStdString())))
    {
        handleConnectionLost(QStringLiteral("发送组队码结果失败，服务端连接已断开，错误码：%1").arg(WSAGetLastError()));
        return false;
    }
    return true;
}

bool ClientConnection::sendTeamReady(int state, int current, int total)
{
    if (!connected_ || socket_ == INVALID_SOCKET)
    {
        return false;
    }

    if (!SocketUtil::SendLine(socket_, Protocol::BuildTeamReadyMessage(state, current, total)))
    {
        handleConnectionLost(QStringLiteral("发送队伍准备状态失败，服务端连接已断开，错误码：%1").arg(WSAGetLastError()));
        return false;
    }
    return true;
}

bool ClientConnection::sendPrepareResponse(int requestId, const QString& status, const QString& payload)
{
    if (!connected_ || socket_ == INVALID_SOCKET)
        return false;
    if (!SocketUtil::SendLine(socket_, Protocol::BuildPrepareResponseMessage(
        requestId, status.toStdString(), payload.toUtf8().toStdString())))
    {
        handleConnectionLost(QStringLiteral("发送执行前准备结果失败，服务端连接已断开，错误码：%1").arg(WSAGetLastError()));
        return false;
    }
    return true;
}

bool ClientConnection::sendOcrResponse(int requestId, bool success, const QString& payload)
{
    if (!connected_ || socket_ == INVALID_SOCKET)
    {
        return false;
    }

    if (!SocketUtil::SendLine(socket_, Protocol::BuildOcrResponseMessage(requestId, success, payload.toStdString())))
    {
        handleConnectionLost(QStringLiteral("发送OCR结果失败，服务端连接已断开，错误码：%1").arg(WSAGetLastError()));
        return false;
    }
    return true;
}

bool ClientConnection::sendSelfThunderResponse(int requestId, bool success, const QString& payload)
{
    if (!connected_ || socket_ == INVALID_SOCKET)
    {
        return false;
    }

    if (!SocketUtil::SendLine(socket_, Protocol::BuildSelfThunderResponseMessage(requestId, success, payload.toStdString())))
    {
        handleConnectionLost(QStringLiteral("发送自雷结果失败，服务端连接已断开，错误码：%1").arg(WSAGetLastError()));
        return false;
    }
    return true;
}

bool ClientConnection::sendLoadoutResponse(int requestId, bool success, const QString& payload)
{
    if (!connected_ || socket_ == INVALID_SOCKET)
    {
        return false;
    }

    if (!SocketUtil::SendLine(socket_, Protocol::BuildLoadoutResponseMessage(requestId, success, payload.toStdString())))
    {
        handleConnectionLost(QStringLiteral("发送配装结果失败，服务端连接已断开，错误码：%1").arg(WSAGetLastError()));
        return false;
    }
    return true;
}

bool ClientConnection::sendSaveLoadoutResponse(int requestId, bool success, const QString& payload)
{
    if (!connected_ || socket_ == INVALID_SOCKET)
    {
        return false;
    }
    if (!SocketUtil::SendLine(socket_, Protocol::BuildSaveLoadoutResponseMessage(
        requestId, success, payload.toUtf8().toStdString())))
    {
        handleConnectionLost(QStringLiteral("发送保存配装结果失败，服务端连接已断开，错误码：%1").arg(WSAGetLastError()));
        return false;
    }
    return true;
}

bool ClientConnection::sendListingResponse(int requestId, bool success, const QString& payload)
{
    if (!connected_ || socket_ == INVALID_SOCKET)
    {
        return false;
    }

    if (!SocketUtil::SendLine(socket_, Protocol::BuildListingResponseMessage(requestId, success, payload.toUtf8().toStdString())))
    {
        handleConnectionLost(QStringLiteral("发送上架结果失败，服务端连接已断开，错误码：%1").arg(WSAGetLastError()));
        return false;
    }
    return true;
}

bool ClientConnection::sendMailResponse(int requestId, bool success, const QString& payload)
{
    if (!connected_ || socket_ == INVALID_SOCKET)
    {
        return false;
    }
    if (!SocketUtil::SendLine(socket_, Protocol::BuildMailResponseMessage(requestId, success, payload.toUtf8().toStdString())))
    {
        handleConnectionLost(QStringLiteral("发送领取邮件结果失败，服务端连接已断开，错误码：%1").arg(WSAGetLastError()));
        return false;
    }
    return true;
}

bool ClientConnection::sendAccountLoginResponse(int requestId, const QString& status, const QString& payload)
{
    int errorCode = 0;
    {
        std::lock_guard<std::mutex> lock(sendMutex_);
        if (!connected_.load()) return false;
        if (SocketUtil::SendLine(socket_, Protocol::BuildAccountLoginResponseMessage(
            requestId, status.toStdString(), payload.toUtf8().toStdString()))) return true;
        errorCode = WSAGetLastError();
    }
    handleConnectionLost(QStringLiteral("发送上号状态失败，错误码：%1").arg(errorCode));
    return false;
}

bool ClientConnection::sendAmountReport(qint64 amount)
{
    if (!connected_ || socket_ == INVALID_SOCKET)
        return false;
    return SocketUtil::SendLine(socket_, Protocol::BuildAmountReportMessage(amount));
}

bool ClientConnection::sendTimeSyncResponse(int requestId, qint64 clientReceiveMs, qint64 clientSendMs)
{
    if (!connected_ || socket_ == INVALID_SOCKET)
    {
        return false;
    }
    return SocketUtil::SendLine(socket_, Protocol::BuildTimeSyncResponseMessage(
        requestId, static_cast<long long>(clientReceiveMs), static_cast<long long>(clientSendMs)));
}

void ClientConnection::receiveLoop(SOCKET socketHandle, quint64 generation)
{
    std::string buffer;
    char chunk[512]{};
    while (connected_ && connectionGeneration_.load() == generation)
    {
        const int received = recv(socketHandle, chunk, sizeof(chunk), 0);
        if (received == 0)
        {
            QMetaObject::invokeMethod(this, [this, generation]() {
                if (connectionGeneration_.load() != generation) return;
                handleConnectionLost(QStringLiteral("recv 返回 0，服务端已关闭连接"));
            }, Qt::QueuedConnection);
            return;
        }
        if (received == SOCKET_ERROR)
        {
            const int errorCode = WSAGetLastError();
            if (errorCode == WSAEINTR)
            {
                continue;
            }
            QMetaObject::invokeMethod(this, [this, errorCode, generation]() {
                if (connectionGeneration_.load() != generation) return;
                handleConnectionLost(QStringLiteral("Socket 接收失败，错误码：%1").arg(errorCode));
            }, Qt::QueuedConnection);
            return;
        }

        buffer.append(chunk, received);
        size_t lineEnd = std::string::npos;
        while ((lineEnd = buffer.find('\n')) != std::string::npos)
        {
            std::string line = buffer.substr(0, lineEnd);
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            buffer.erase(0, lineEnd + 1);

            Protocol::ParsedMessage message{};
            if (!Protocol::ParseLine(line, message))
            {
                continue;
            }

            if (message.type == Protocol::MessageType::Trigger)
            {
                QMetaObject::invokeMethod(this, [this, seconds = message.value,
                    bossPrice = QString::fromUtf8(message.bossPrice.data(), static_cast<int>(message.bossPrice.size()))]() {
                    emit triggerReceived(seconds, bossPrice);
                }, Qt::QueuedConnection);
            }
            else if (message.type == Protocol::MessageType::ScheduledTrigger)
            {
                QMetaObject::invokeMethod(this, [this, seconds = message.value,
                    targetClientMs = static_cast<qint64>(message.timestampMs),
                    bossPrice = QString::fromUtf8(message.bossPrice.data(), static_cast<int>(message.bossPrice.size()))]() {
                    emit scheduledTriggerReceived(seconds, targetClientMs, bossPrice);
                }, Qt::QueuedConnection);
            }
            else if (message.type == Protocol::MessageType::TimeSyncRequest)
            {
                const qint64 clientReceiveMs = QDateTime::currentMSecsSinceEpoch();
                const bool sent = sendTimeSyncResponse(message.requestId, clientReceiveMs,
                    QDateTime::currentMSecsSinceEpoch());
                Q_UNUSED(sent);
            }
            else if (message.type == Protocol::MessageType::ServerTime)
            {
                QMetaObject::invokeMethod(this, [this,
                    serverTimeMs = static_cast<qint64>(message.timestampMs),
                    clientOffsetMs = static_cast<qint64>(message.timestampMs2)]() {
                    emit serverTimeReceived(serverTimeMs, clientOffsetMs);
                }, Qt::QueuedConnection);
            }
            else if (message.type == Protocol::MessageType::Role)
            {
                QMetaObject::invokeMethod(this, [this, role = message.role]() {
                    emit roleReceived(role);
                }, Qt::QueuedConnection);
            }
            else if (message.type == Protocol::MessageType::TeamCodeRequest)
            {
                QMetaObject::invokeMethod(this, [this]() {
                    emit teamCodeRequestReceived();
                }, Qt::QueuedConnection);
            }
            else if (message.type == Protocol::MessageType::TeamReadyRequest)
            {
                QMetaObject::invokeMethod(this, [this]() {
                    emit teamReadyRequestReceived();
                }, Qt::QueuedConnection);
            }
            else if (message.type == Protocol::MessageType::PrepareRequest)
            {
                QMetaObject::invokeMethod(this, [this, requestId = message.requestId]() {
                    emit prepareRequestReceived(requestId);
                }, Qt::QueuedConnection);
            }
            else if (message.type == Protocol::MessageType::RebindWindowRequest)
            {
                QMetaObject::invokeMethod(this, [this]() {
                    emit rebindWindowRequestReceived();
                }, Qt::QueuedConnection);
            }
            else if (message.type == Protocol::MessageType::ForceStopActionRequest)
            {
                // 先在接收线程设置取消标志，避免客户端 UI 线程正执行同步自动化时无法及时中止。
                emit forceStopActionImmediate();
                QMetaObject::invokeMethod(this, [this]() {
                    emit forceStopActionRequestReceived();
                }, Qt::QueuedConnection);
            }
            else if (message.type == Protocol::MessageType::OcrRequest)
            {
                QMetaObject::invokeMethod(this, [this, message]() {
                    emit ocrRequestReceived(message.requestId, message.x, message.y, message.width, message.height, message.referenceWidth, message.referenceHeight);
                }, Qt::QueuedConnection);
            }
            else if (message.type == Protocol::MessageType::SelfThunderRequest)
            {
                QMetaObject::invokeMethod(this, [this, requestId = message.requestId]() {
                    emit selfThunderRequestReceived(requestId);
                }, Qt::QueuedConnection);
            }
            else if (message.type == Protocol::MessageType::LoadoutRequest)
            {
                QMetaObject::invokeMethod(this, [this, requestId = message.requestId, loadoutType = QString::fromStdString(message.payload)]() {
                    emit loadoutRequestReceived(requestId, loadoutType);
                }, Qt::QueuedConnection);
            }
            else if (message.type == Protocol::MessageType::SaveLoadoutRequest)
            {
                const QString weapon = QString::fromUtf8(message.loadoutWeapon.data(), static_cast<int>(message.loadoutWeapon.size()));
                const QString backpack = QString::fromUtf8(message.loadoutBackpack.data(), static_cast<int>(message.loadoutBackpack.size()));
                const QString egg = QString::fromUtf8(message.loadoutEgg.data(), static_cast<int>(message.loadoutEgg.size()));
                const QString bulletName = QString::fromUtf8(message.loadoutBulletName.data(), static_cast<int>(message.loadoutBulletName.size()));
                QMetaObject::invokeMethod(this, [this, requestId = message.requestId, weapon, backpack, egg, bulletName,
                    perClick = message.loadoutPerClick, bulletTotal = message.loadoutBulletTotal]() {
                    emit saveLoadoutRequestReceived(requestId, weapon, backpack, egg, bulletName, perClick, bulletTotal);
                }, Qt::QueuedConnection);
            }
            else if (message.type == Protocol::MessageType::ListingRequest)
            {
                const QString listingPrice = QString::fromUtf8(message.price.data(), static_cast<int>(message.price.size()));
                const QString listingText = QString::fromUtf8(message.payload.data(), static_cast<int>(message.payload.size()));
                QMetaObject::invokeMethod(this, [this, requestId = message.requestId, hasExchange = message.value == 1,
                    loadoutMode = message.loadoutMode, purchaseCount = message.purchaseCount, listingPrice, listingText]() {
                    emit listingRequestReceived(requestId, hasExchange, purchaseCount, loadoutMode, listingPrice, listingText);
                }, Qt::QueuedConnection);
            }
            else if (message.type == Protocol::MessageType::MailRequest)
            {
                QMetaObject::invokeMethod(this, [this, requestId = message.requestId]() {
                    emit mailRequestReceived(requestId);
                }, Qt::QueuedConnection);
            }
            else if (message.type == Protocol::MessageType::AccountLoginStopRequest)
            {
                emit accountLoginStopImmediate();
                QMetaObject::invokeMethod(this, [this]() {
                    emit accountLoginStopRequestReceived();
                }, Qt::QueuedConnection);
            }
            else if (message.type == Protocol::MessageType::ClientKicked)
            {
                // 先设置禁止自动重连，再等待服务端关闭 socket，避免断线回调重新发起连接。
                autoReconnectSuppressed_ = true;
                userRequestedDisconnect_ = true;
                emit clientKickedImmediate();
                QMetaObject::invokeMethod(this, [this]() {
                    emit clientKicked();
                }, Qt::QueuedConnection);
            }
            else if (message.type == Protocol::MessageType::AccountLoginRequest)
            {
                const auto separator = message.payload.find('\n');
                if (separator != std::string::npos)
                {
                    const QString username = QString::fromUtf8(message.payload.substr(0, separator).data());
                    const QString password = QString::fromUtf8(message.payload.substr(separator + 1).data());
                    QMetaObject::invokeMethod(this, [this, generation, requestId = message.requestId, username, password]() {
                        if (!connected_ || connectionGeneration_.load() != generation) return;
                        emit accountLoginRequestReceived(requestId, username, password);
                    }, Qt::QueuedConnection);
                }
            }
            else if (message.type == Protocol::MessageType::Pong)
            {
                heartbeatMisses_ = 0;
            }
        }
    }
}

void ClientConnection::sendHeartbeat()
{
    if (!connected_ || socket_ == INVALID_SOCKET)
    {
        return;
    }

    // 心跳只用于保活和触发 socket 错误，不再因为未收到 PONG 主动断开。
    ++heartbeatMisses_;
    if (ConnectionLifecyclePolicy::ShouldAutoDisconnectForHeartbeatMiss(heartbeatMisses_.load()))
    {
        handleConnectionLost(QStringLiteral("心跳超时，连续 %1 次未收到服务端 PONG，服务端连接已断开").arg(heartbeatMisses_.load()));
        return;
    }

    // 空闲时定时发送心跳，尽早发现公网/NAT 静默断线。
    if (!SocketUtil::SendLine(socket_, Protocol::BuildPingMessage()))
    {
        handleConnectionLost(QStringLiteral("心跳发送失败，服务端连接已断开，错误码：%1").arg(WSAGetLastError()));
    }
}

void ClientConnection::handleConnectionLost(const QString& statusText)
{
    if (!connected_.exchange(false))
    {
        return;
    }

    heartbeatTimer_.stop();
    appendConnectionLog(statusText);
    closeSocketOnly();
    if (receiveThread_.joinable() && receiveThread_.get_id() != std::this_thread::get_id())
    {
        receiveThread_.join();
    }

    const bool reconnecting = startReconnect();
    emit connectedChanged(false);
    if (!reconnecting)
    {
        emit statusChanged(statusText);
    }
}

void ClientConnection::appendConnectionLog(const QString& text)
{
    emit connectionLogAppended(text);
}

void ClientConnection::closeSocketOnly()
{
    // 已排队的旧 recv 结果不能关闭随后建立的新连接（Socket 数值也可能被复用）。
    ++connectionGeneration_;
    if (socket_ != INVALID_SOCKET)
    {
        shutdown(socket_, SD_BOTH);
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
}
