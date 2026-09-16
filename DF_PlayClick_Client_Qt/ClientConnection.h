#pragma once

#include <winsock2.h>

#include <atomic>
#include <thread>

#include <QObject>
#include <QString>
#include <QTimer>

#include "../Shared/ClientRole.h"
#include "../Shared/SocketUtil.h"

class ClientConnection : public QObject
{
    Q_OBJECT

public:
    explicit ClientConnection(QObject* parent = nullptr);
    ~ClientConnection() override;

    bool isConnected() const;
    bool isReconnecting() const;
    void connectToServer(const QString& ip, int port);
    void disconnectFromServer();
    bool sendReady(bool ready);
    bool sendTeamCodeResponse(const QString& payload);
    bool sendTeamReady(int state, int current, int total);
    bool sendOcrResponse(int requestId, bool success, const QString& payload);
    bool sendSelfThunderResponse(int requestId, bool success, const QString& payload);
    bool sendLoadoutResponse(int requestId, bool success, const QString& payload);
    bool sendListingResponse(int requestId, bool success, const QString& payload);
    bool sendMailResponse(int requestId, bool success, const QString& payload);
    bool sendTimeSyncResponse(int requestId, qint64 clientReceiveMs, qint64 clientSendMs);

signals:
    void connectedChanged(bool connected);
    void reconnectingChanged(bool reconnecting);
    void statusChanged(const QString& text);
    void connectionLogAppended(const QString& text);
    void triggerReceived(int seconds);
    void scheduledTriggerReceived(int seconds, qint64 targetClientMs);
    void serverTimeReceived(qint64 serverTimeMs, qint64 clientOffsetMs);
    void roleReceived(ClientRole role);
    void teamCodeRequestReceived();
    void teamReadyRequestReceived();
    void rebindWindowRequestReceived();
    void forceStopActionRequestReceived();
    // 在接收线程直接通知取消标志，避免 UI 线程被同步自动化流程阻塞时无法中止。
    void forceStopActionImmediate();
    void ocrRequestReceived(int requestId, int x, int y, int width, int height, int referenceWidth, int referenceHeight);
    void selfThunderRequestReceived(int requestId);
    void loadoutRequestReceived(int requestId, const QString& loadoutType);
    void listingRequestReceived(int requestId, bool hasExchange, int purchaseCount, const QString& listingPrice, const QString& listingText);
    void mailRequestReceived(int requestId);

private slots:
    void sendHeartbeat();
    void reconnectToServer();

private:
    bool tryConnectToStoredServer(bool reconnecting);
    bool startReconnect();
    void stopReconnect();
    void receiveLoop();
    void closeSocketOnly();
    void handleConnectionLost(const QString& statusText);
    void appendConnectionLog(const QString& text);

    SocketUtil::WsaSession wsa_;
    SOCKET socket_ = INVALID_SOCKET;
    std::thread receiveThread_;
    QTimer heartbeatTimer_;
    QTimer reconnectTimer_;
    QString serverIp_;
    int serverPort_ = 0;
    bool userRequestedDisconnect_ = true;
    bool desiredReady_ = true;
    int reconnectAttempt_ = 0;
    std::atomic_int heartbeatMisses_ = 0;
    std::atomic_bool connected_ = false;
};
