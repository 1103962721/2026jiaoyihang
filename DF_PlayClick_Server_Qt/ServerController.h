#pragma once

#include <QHash>
#include <QObject>
#include <QTimer>
#include <QString>
#include <QVector>

#include "ServerConnectionManager.h"
#include "ServerModels.h"

class ServerController : public QObject
{
    Q_OBJECT

public:
    explicit ServerController(QObject* parent = nullptr);

    QString defaultPortText() const;
    QVector<ClientListItem> clients() const;
    bool isListening() const;

public slots:
    void startListening(const QString& portText);
    void stopListening();
    void triggerClients(const QString& countdownText, const QString& groupDelayText, bool coinFirst);
    void triggerClient(quint64 clientId);
    void requestTeamCode(quint64 clientId);
    void requestTeamReady(quint64 clientId);
    void requestRebindWindow(quint64 clientId);
    void requestRebindAllWindows();
    void requestForceStopAction(quint64 clientId);
    void requestForceStopAllActions();
    void requestSelfThunder(quint64 clientId);
    void requestLoadout(quint64 clientId);
    void setAutoStartEnabled(bool enabled, const QString& countdownText);
    void setAutoStartCountdownText(const QString& countdownText);
    void forceResetClientAction(quint64 clientId);
    void setClientTeamCode(quint64 clientId, const QString& teamCode);
    void showSelfThunderPlaceholder(quint64 clientId);
    void setClientRemark(quint64 clientId, const QString& remark);
    void setClientRole(quint64 clientId, ClientRole role);
    void startListing(const QString& listingText, const QString& listingPrice, bool hasExchange, int purchaseCount);
    void requestMail(quint64 clientId);
    void requestMailAll();
    void kickClient(quint64 clientId);

signals:
    void clientsChanged(const QVector<ClientListItem>& clients);
    void listeningChanged(bool listening);
    void statusChanged(const QString& text, ServerStatusTone tone);
    void connectionLogAppended(const QString& text);
    void messageRequested(const QString& title, const QString& text, bool error);
    void autoStartEnabledChanged(bool enabled);

private:
    int parsePort(const QString& portText) const;
    void processAutoStartTick();

    enum class AutoStartStage
    {
        WaitingTeamFull,
        Loadout,
        AfterTriggerDelay,
        Matching,
        Loading,
        InGame,
        SelfThunder
    };

    struct AutoStartClientState
    {
        AutoStartStage stage = AutoStartStage::WaitingTeamFull;
        qint64 stageStartedAtMs = 0;
        int pendingRequestId = 0;
        int pendingSelfThunderRequestId = 0;
        int pendingLoadoutRequestId = 0;
        bool sawCancelAction = false;
        bool rescueTriggerSent = false;
        bool loadoutFailed = false;
    };

    void handleAutoStartOcrResponse(quint64 clientId, int requestId, bool success, const QString& text);
    void handleSelfThunderResponse(quint64 clientId, int requestId, bool success, const QString& text);
    void handleLoadoutResponse(quint64 clientId, int requestId, bool success, const QString& text);
    void resetAutoStartRound();
    void resetAutoStartClient(quint64 clientId);
    void processTriggeredClientStage(const ClientListItem& client, AutoStartClientState& state, qint64 nowMs);
    void processMatchingRescue(const QVector<ClientListItem>& clients, qint64 nowMs);
    bool requestStageOcr(quint64 clientId, AutoStartClientState& state, AutoStartStage stage, const OcrRequestRegion& region);
    bool requestSelfThunder(quint64 clientId, AutoStartClientState& state);
    bool requestLoadout(quint64 clientId, AutoStartClientState& state);
    void executeSelfThunderPlaceholder(quint64 clientId);

    ServerConnectionManager connectionManager_;
    QTimer autoStartTimer_;
    QString autoStartCountdownText_;
    QHash<quint64, AutoStartClientState> autoStartClientStates_;
    QHash<int, quint64> pendingOcrRequests_;
    QHash<int, quint64> pendingSelfThunderRequests_;
    QHash<int, quint64> pendingLoadoutRequests_;
    qint64 autoStartCycleStartedAtMs_ = 0;
    int nextOcrRequestId_ = 1;
    int nextListingRequestId_ = 1;
    int nextMailRequestId_ = 1;
    bool autoStartEnabled_ = false;
    bool autoStartCanTrigger_ = true;
};
