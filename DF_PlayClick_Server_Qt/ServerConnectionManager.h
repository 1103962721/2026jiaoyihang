#pragma once

#include <winsock2.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <QObject>
#include <QString>
#include <QVector>

#include "ServerModels.h"
#include "../Shared/SocketUtil.h"

class ServerConnectionManager : public QObject
{
    Q_OBJECT

public:
    explicit ServerConnectionManager(QObject* parent = nullptr);
    ~ServerConnectionManager() override;

    bool isListening() const;
    bool startListening(int port, QString& errorText);
    void stopListening();
    int triggerReadyClients(int seconds);
    int triggerReadyClientsByRole(int seconds, ClientRole role);
    bool triggerSingleClient(quint64 clientId, int seconds, QString& errorText);
    bool requestTeamCode(quint64 clientId, QString& errorText);
    bool requestTeamReady(quint64 clientId, QString& errorText);
    bool requestRebindWindow(quint64 clientId, QString& errorText);
    int requestRebindAllWindows();
    bool requestForceStopAction(quint64 clientId, QString& errorText);
    int requestForceStopAllActions();
    bool requestOcr(quint64 clientId, int requestId, const OcrRequestRegion& region, QString& errorText);
    bool requestSelfThunder(quint64 clientId, int requestId, QString& errorText);
    bool requestLoadout(quint64 clientId, int requestId, const QString& loadoutType, QString& errorText);
    bool requestListing(quint64 clientId, int requestId, const QString& listingText, const QString& listingPrice, bool hasExchange, int purchaseCount, QString& errorText);
    bool requestMail(quint64 clientId, int requestId, QString& errorText);
    bool setClientTeamReadyDisplay(quint64 clientId, const QString& text, int state);
    bool resetClientTeamReadyDisplay(quint64 clientId);
    bool forceResetClientAction(quint64 clientId);
    bool setClientTeamCode(quint64 clientId, const QString& teamCode, QString& errorText);
    bool setClientRemark(quint64 clientId, const QString& remark);
    bool setClientRole(quint64 clientId, ClientRole role);
    bool setClientListingStatus(quint64 clientId, const QString& status);
    int coinClientCount(bool readyOnly) const;
    bool kickClient(quint64 clientId);
    QVector<ClientListItem> clients() const;

signals:
    void clientsChanged();
    void listeningChanged(bool listening);
    void networkError(const QString& text);
    void ocrResponseReceived(quint64 clientId, int requestId, bool success, const QString& text);
    void selfThunderResponseReceived(quint64 clientId, int requestId, bool success, const QString& text);
    void loadoutResponseReceived(quint64 clientId, int requestId, bool success, const QString& text);
    void listingResponseReceived(quint64 clientId, int requestId, bool success, const QString& text);
    void mailResponseReceived(quint64 clientId, int requestId, bool success, const QString& text);

private:
    struct ClientEntry
    {
        quint64 id = 0;
        SOCKET socketHandle = INVALID_SOCKET;
        QString ip;
        ClientRole role = ClientRoleUtil::DefaultRole;
        QString remark = QStringLiteral("普通");
        QString teamReadyText = QStringLiteral("未识别");
        int teamReadyState = -1;
        int teamReadyCurrent = 0;
        int teamReadyTotal = 0;
        qint64 teamReadyUpdatedAtMs = 0;
        QString teamCode;
        QString runningAction;
        QString listingStatus = QStringLiteral("???");
        bool actionRunning = false;
        bool ready = false;
        bool connected = true;
        std::thread receiveThread;
    };

    void acceptLoop();
    void receiveLoop(std::shared_ptr<ClientEntry> client);
    void markClientDisconnected(quint64 clientId);
    void recordDisconnectReason(const std::shared_ptr<ClientEntry>& client, const QString& reason);
    std::shared_ptr<ClientEntry> findClientLocked(quint64 clientId) const;
    void closeClientSocketLocked(const std::shared_ptr<ClientEntry>& client);

    SocketUtil::WsaSession wsa_;
    SOCKET listenSocket_ = INVALID_SOCKET;
    std::thread acceptThread_;
    std::atomic_bool listening_ = false;
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<ClientEntry>> clients_;
    quint64 nextClientId_ = 1;
};
