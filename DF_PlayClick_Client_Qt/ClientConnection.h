#pragma once

#include <winsock2.h>

#include <atomic>
#include <mutex>
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
    void setDeviceId(const QString& deviceId);
    void connectToServer(const QString& ip, int port);
    void disconnectFromServer();
    // 服务端主动踢出后禁止本次连接生命周期的自动重连；手动重新连接会清除该状态。
    void suppressAutoReconnect();
    bool sendReady(bool ready);
    bool sendAuctionBinding(bool bound);
    bool sendAuctionCatalog(const QString& catalog);
    bool sendAuctionConfigResult(int revision, int status, const QString& text);
    bool sendTeamCodeResponse(const QString& payload);
    bool sendTeamReady(int state, int current, int total);
    bool sendPrepareResponse(int requestId, const QString& status, const QString& payload);
    bool sendOcrResponse(int requestId, bool success, const QString& payload);
    bool sendSelfThunderResponse(int requestId, bool success, const QString& payload);
    bool sendLoadoutResponse(int requestId, bool success, const QString& payload);
    bool sendSaveLoadoutResponse(int requestId, bool success, const QString& payload);
    bool sendListingResponse(int requestId, bool success, const QString& payload);
    bool sendMailResponse(int requestId, bool success, const QString& payload);
    bool sendAmountReport(qint64 amount);
    bool sendTimeSyncResponse(int requestId, qint64 clientReceiveMs, qint64 clientSendMs);
    bool sendAccountLoginResponse(int requestId, const QString& status, const QString& payload);
    bool sendExecutionResponse(bool success, const QString& payload);
    bool sendForceStopActionAck();

signals:
    void auctionCatalogRequested();
    void auctionConfigReceived(int revision, const QString& config);
    void connectedChanged(bool connected);
    void reconnectingChanged(bool reconnecting);
    void statusChanged(const QString& text);
    void connectionLogAppended(const QString& text);
    void triggerReceived(int seconds, const QString& bossPrice);
    void scheduledTriggerReceived(int seconds, qint64 targetClientMs, const QString& bossPrice);
    void serverTimeReceived(qint64 serverTimeMs, qint64 clientOffsetMs);
    void roleReceived(ClientRole role);
    void teamCodeRequestReceived();
    void teamReadyRequestReceived();
    void prepareRequestReceived(int requestId);
    void rebindWindowRequestReceived();
    void forceStopActionRequestReceived();
    void forceStopActionAckReceived();
    // 在接收线程直接通知取消标志，避免 UI 线程被同步自动化流程阻塞时无法中止。
    void forceStopActionImmediate();
    void accountLoginStopRequestReceived();
    void accountLoginStopImmediate();
    void clientKicked();
    void clientKickedImmediate();
    void ocrRequestReceived(int requestId, int x, int y, int width, int height, int referenceWidth, int referenceHeight);
    void selfThunderRequestReceived(int requestId);
    void loadoutRequestReceived(int requestId, const QString& loadoutType);
    void saveLoadoutRequestReceived(int requestId, const QString& weapon, const QString& backpack,
        const QString& egg, const QString& bulletName, int perClick, int bulletTotal);
    void listingRequestReceived(int requestId, bool hasExchange, int purchaseCount, bool loadoutMode, const QString& listingPrice, const QString& listingText);
    void mailRequestReceived(int requestId);
    void accountLoginRequestReceived(int requestId, const QString& username, const QString& password);

private slots:
    void sendHeartbeat();
    void reconnectToServer();

private:
    bool tryConnectToStoredServer(bool reconnecting);
    bool startReconnect();
    void stopReconnect();
    void receiveLoop(SOCKET socketHandle, quint64 generation);
    void closeSocketOnly();
    void handleConnectionLost(const QString& statusText);
    void appendConnectionLog(const QString& text);

    SocketUtil::WsaSession wsa_;
    SOCKET socket_ = INVALID_SOCKET;
    std::thread receiveThread_;
    std::mutex sendMutex_;
    QTimer heartbeatTimer_;
    QTimer reconnectTimer_;
    QString serverIp_;
    QString deviceId_;
    int serverPort_ = 0;
    bool userRequestedDisconnect_ = true;
    bool desiredReady_ = true;
    int reconnectAttempt_ = 0;
    std::atomic_int heartbeatMisses_ = 0;
    std::atomic_bool connected_ = false;
    std::atomic<quint64> connectionGeneration_{0};
    std::atomic_bool autoReconnectSuppressed_ = false;
};
