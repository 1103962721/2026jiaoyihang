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
    void triggerClients(const QString& countdownText, const QString& groupDelayText, bool coinFirst,
        bool compensationEnabled = false, const QString& compensationLeadText = QStringLiteral("1500"),
        const QString& bossPriceText = QString(), bool allowIncomplete = false, bool automatic = false);
    void triggerClient(quint64 clientId, const QString& bossPriceText = QString());
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
    void startListing(const QString& listingText, const QString& listingPrice, bool hasExchange, int purchaseCount, bool loadoutMode);
    void startListingForClient(quint64 clientId, const QString& listingText, const QString& listingPrice,
        bool hasExchange, int purchaseCount, bool loadoutMode);
    void saveLoadout(const QString& weapon, const QString& backpack, const QString& egg,
        const QString& bulletName, int perClick, int bulletTotal);
    void saveLoadoutForRole(ClientRole role, const QString& weapon, const QString& backpack, const QString& egg,
        const QString& bulletName, int perClick, int bulletTotal);
    void saveLoadoutForClient(quint64 clientId, ClientRole role, const QString& weapon, const QString& backpack,
        const QString& egg, const QString& bulletName, int perClick, int bulletTotal);
    void startCoinSemiAuto(const QString& weapon, const QString& backpack, const QString& egg,
        const QString& bulletName, int perClick, int bulletTotal, const QString& listingText,
        const QString& listingPrice, int purchaseCount, bool loadoutMode);
    void requestMail(quint64 clientId);
    void requestMailAll();
    void importAccounts(const QString& filePath);
    void startAccountLogin(quint64 clientId);
    void setAutoExecuteThresholds(int bossCount, int coinCount);
    void setHighPriceStopThreshold(int threshold);
    void setAutoExecuteEnabled(bool enabled);
    void updateAutoExecuteParameters(const QString& countdownText, const QString& groupDelayText,
        bool coinFirst, bool compensationEnabled, const QString& compensationLeadText,
        const QString& bossPriceText);
    void kickClient(quint64 clientId);
    void setBossExpectedAmount(quint64 clientId, const QString& amountText);

signals:
    void clientsChanged(const QVector<ClientListItem>& clients);
    void listeningChanged(bool listening);
    void statusChanged(const QString& text, ServerStatusTone tone);
    void connectionLogAppended(const QString& text);
    void messageRequested(const QString& title, const QString& text, bool error);
    void incompleteTriggerRequested();
    void autoStartEnabledChanged(bool enabled);
    void accountQueueChanged(int total, int remaining, int assigned);

private:
    int parsePort(const QString& portText) const;
    void processAutoStartTick();
    void processCompensationSync();
    void finishCompensationSync(bool timedOut);

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
    QTimer compensationSyncTimer_;
    QTimer autoExecuteTimer_;
    QString autoStartCountdownText_;
    QHash<quint64, AutoStartClientState> autoStartClientStates_;
    QHash<int, quint64> pendingOcrRequests_;
    QHash<int, quint64> pendingSelfThunderRequests_;
    QHash<int, quint64> pendingLoadoutRequests_;
    struct SemiAutoState
    {
        QString listingText;
        QString listingPrice;
        int purchaseCount = 1;
        bool loadoutMode = false;
        bool relistedAfterMail = false;
    };
    QHash<int, SemiAutoState> pendingSemiLoadoutRequests_;
    QHash<quint64, SemiAutoState> semiAutoStates_;
    qint64 autoStartCycleStartedAtMs_ = 0;
    int nextOcrRequestId_ = 1;
    int nextListingRequestId_ = 1;
    int nextMailRequestId_ = 1;
    int nextSaveLoadoutRequestId_ = 1;
    int nextTimeSyncRequestId_ = 1000;
    struct CompensationRound
    {
        bool active = false;
        int seconds = 0;
        int leadMs = 1500;
        int groupDelayMs = 0;
        QString bossPrice;
        ClientRole firstRole = ClientRole::Coin;
        ClientRole secondRole = ClientRole::Boss;
        qint64 deadlineMs = 0;
        QHash<quint64, int> requestIds;
        QHash<quint64, qint64> sendTimesMs;
        QHash<quint64, qint64> offsetsMs;
        QVector<quint64> selectedClientIds;
    } compensationRound_;
    bool autoStartEnabled_ = false;
    bool autoStartCanTrigger_ = true;
    int autoExecuteBossThreshold_ = 1;
    int autoExecuteCoinThreshold_ = 1;
    int highPriceStopThreshold_ = 3;
    bool autoExecuteEnabled_ = false;
    bool autoExecuteTriggered_ = false;
    struct AccountCredential { QString username; QString password; bool assigned = false; quint64 clientId = 0; };
    QVector<AccountCredential> accounts_;
    int nextAccountLoginRequestId_ = 20000;
    bool preparingExecution_ = false;
    bool dispatchingPreparedExecution_ = false;
    QHash<int, quint64> pendingPrepareRequests_;
    QString pendingCountdownText_;
    QString pendingGroupDelayText_;
    bool pendingCoinFirst_ = true;
    bool pendingCompensationEnabled_ = false;
    QString pendingCompensationLeadText_;
    QString pendingBossPriceText_;
    bool pendingAllowIncomplete_ = false;
    bool pendingAutomatic_ = false;
    QString bossListingPrice_;
    QString lastTriggerCountdownText_ = QStringLiteral("5");
    QString lastTriggerGroupDelayText_ = QStringLiteral("100");
    bool lastTriggerCoinFirst_ = true;
    bool lastTriggerCompensationEnabled_ = true;
    QString lastTriggerCompensationLeadText_ = QStringLiteral("1500");
    void processAutoExecuteTick();
    void handlePrepareResponse(quint64 clientId, int requestId, int status, const QString& text);
};
