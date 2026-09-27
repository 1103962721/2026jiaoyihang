#pragma once

#include <windows.h>

#include <atomic>
#include <QObject>
#include <QTimer>
#include <QVector>
#include <QAbstractNativeEventFilter>
#include <QDateTime>

#include "ClientConfig.h"
#include "ClientConnection.h"
#include "HotkeyManager.h"
#include "TeamReadyParser.h"
#include "UmiOcrRunner.h"
#include "Win32InputDispatcher.h"
#include "Win32WindowBinder.h"
#include "AccountLoginAutomation.h"

struct ClickTask
{
    HWND hwnd = nullptr;
    QString title;
    QString processName;
    QString hwndText;
    int clientWidth = 0;
    int clientHeight = 0;
    int x = 0;
    int y = 0;
    bool enabled = true;
};

class ClientController : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT

public:
    explicit ClientController(QObject* parent = nullptr);

    QString lastServerIp() const;
    QVector<TargetWindowInfo> windows() const;
    QVector<ClickTask> tasks() const;
    int boundWindowIndex() const;
    int selectedTaskIndex() const;
    bool isConnected() const;
    bool isReconnecting() const;
    bool isReady() const;
    HWND boundHwnd() const;
    int clickQueueDelayMs() const;
    double unitPrice() const;
    int bulletTotal() const;
    bool isBulletOcrRunning() const;
    ClientRole clientRole() const;

    bool initialize(HWND mainWindowHandle);
    bool nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) override;
    void refreshWindows();
    bool autoBindGameWindow(bool showMessage);
    void rebindGameWindow();
    void forceStopCurrentAction();
    void bindWindow(int index, bool bringToFront);
    void addTaskFromWindow(int index, bool bringToFront);
    void selectTask(int index);
    void removeSelectedTask();
    void moveSelectedTaskUp();
    void moveSelectedTaskDown();
    void toggleSelectedTaskEnabled();
    void connectOrDisconnect(const QString& ip, int port);
    void toggleReady();
    void autoPickPoint(bool showMessage);
    void startHotkeyPick();
    void setClickPoint(int x, int y);
    void setClickQueueDelayMs(int delayMs);
    void setOcrParameters(double unitPrice, int bulletTotal);
    void toggleBulletOcrLoop();

signals:
    void windowsChanged();
    void tasksChanged();
    void selectedTaskChanged(int taskIndex);
    void boundWindowChanged(const QString& text);
    void clickPointChanged(int x, int y);
    void connectedChanged(bool connected);
    void reconnectingChanged(bool reconnecting);
    void readyChanged(bool ready);
    void countdownChanged(const QString& text);
    void serverTimeChanged(const QString& text);
    void clickQueueDelayChanged(int delayMs);
    void ocrParametersChanged(double unitPrice, int bulletTotal);
    void bulletOcrRunningChanged(bool running);
    void bulletOcrResultChanged(const QString& rawText, double recognizedPrice, double averagePrice, bool meetsUnitPrice);
    void bulletOcrLogAppended(const QString& text);
    void clientRoleChanged(ClientRole role);
    void statusChanged(const QString& text);
    void connectionLogAppended(const QString& text);
    void messageRequested(const QString& title, const QString& text, bool error);

private:
    bool hasEnabledTasks() const;
    void setReady(bool ready);
    void clearLocalReadyState();
    void setStatus(const QString& text);
    void startCountdown(int seconds, const QString& bossPrice);
    void startScheduledTrigger(int seconds, qint64 targetClientMs, const QString& bossPrice);
    void fireScheduledTrigger();
    void updateServerTimeDisplay();
    void tickCountdown();
    void clickEnabledTasks();
    void reloadSharedClickQueueDelay();
    void broadcastClickQueueDelayChanged();
    void finishHotkeyPick();
    void syncBoundWindowFromSelectedTask();
    void runBulletOcrRound();
    void scheduleNextBulletOcrRound(bool completedDecision = false);
    void finishBulletOcrLoop(bool success, const QString& reason);
    void appendBulletOcrLog(const QString& text);
    void handleTeamCodeRequest();
    void handleTeamReadyRequest();
    void handlePrepareRequest(int requestId);
    void handleOcrRequest(int requestId, int x, int y, int width, int height, int referenceWidth, int referenceHeight);
    void handleSelfThunderRequest(int requestId);
    void handleLoadoutRequest(int requestId, const QString& loadoutType);
    void handleSaveLoadoutRequest(int requestId, const QString& weapon, const QString& backpack,
        const QString& egg, const QString& bulletName, int perClick, int bulletTotal);
    void handleListingRequest(int requestId, bool hasExchange, int purchaseCount, bool loadoutMode, const QString& listingPrice, const QString& listingText);
    void handleMailRequest(int requestId);
    void handleAccountLoginRequest(int requestId, const QString& username, const QString& password);
    bool executeSelfThunderSequence(QString& errorText);
    bool executeLoadoutSequence(const QString& loadoutType, QString& errorText);
    bool executeSaveLoadoutSequence(const QString& weapon, const QString& backpack, const QString& egg,
        const QString& bulletName, int perClick, int bulletTotal, QString& errorText);
    bool executeListingSequence(bool hasExchange, int purchaseCount, bool loadoutMode, const QString& listingPrice, const QString& listingText,
        QString& ocrText, QString& usageResult, QString& errorText);
    bool executeMailSequence(QString& errorText);
    bool executeCoinRevenueClaimSequence(bool& noRevenue, QString& errorText);
    bool waitForListingQuantityReady(QString& errorText, const QString& prefix,
        int timeoutMs = 10000, bool* performedDismount = nullptr);
    bool executeBossListingSequence(const QString& listingPrice, QString& errorText);
    bool executeBossPrepareCheck(int requestId, QString& errorText);
    bool waitForAutomation(int milliseconds, QString& errorText);
    bool beginAutomationAction(const QString& actionName);
    void finishAutomationAction();
    void sendTeamCodeResult(const QString& payload, const QString& statusText);
    QString captureOcrRegion(const OcrRegionConfig& region, const QString& filePrefix, QString& errorText) const;
    bool sendTeamReadyResult(const TeamReadyResult& result);

    ClientConfig config_;
    ClientConnection connection_;
    AccountLoginAutomation accountLoginAutomation_;
    Win32WindowBinder binder_;
    Win32InputDispatcher input_;
    HotkeyManager hotkey_;
    UmiOcrRunner umiOcrRunner_;
    TeamReadyParser teamReadyParser_;
    QTimer countdownTimer_;
    QTimer scheduledTriggerTimer_;
    QTimer serverTimeTimer_;
    QTimer bulletOcrActionTimer_;
    QTimer bulletOcrNextRoundTimer_;
    QVector<TargetWindowInfo> windows_;
    QVector<ClickTask> tasks_;
    HWND mainWindowHandle_ = nullptr;
    HWND boundHwnd_ = nullptr;
    int boundWindowIndex_ = -1;
    int selectedTaskIndex_ = -1;
    int clickX_ = 1741;
    int clickY_ = 965;
    int clickQueueDelayMs_ = ClientConfig::DefaultClickQueueDelayMs;
    double unitPrice_ = ClientConfig::DefaultUnitPrice;
    int bulletTotal_ = ClientConfig::DefaultBulletTotal;
    BulletOcrConfig bulletOcrConfig_;
    int bulletOcrRoundIndex_ = 0;
    int countdown_ = 0;
    int scheduledTriggerSeconds_ = 0;
    qint64 scheduledTriggerTargetMs_ = 0;
    QString triggerBossPrice_;
    QString lastListingPrice_;
    QString lastListingText_;
    bool lastListingHasExchange_ = false;
    int lastListingPurchaseCount_ = 0;
    bool lastListingLoadoutMode_ = false;
    int lastListingRequestId_ = 0;
    int accountLoginRequestId_ = 0;
    bool accountLoginRunning_ = false;
    bool hasLastListingRequest_ = false;
    qint64 serverClockOffsetMs_ = 0;
    bool serverTimeSynchronized_ = false;
    int lastTeamReadyState_ = -2;
    int lastTeamReadyCurrent_ = -1;
    int lastTeamReadyTotal_ = -1;
    bool ready_ = false;
    bool automationActionRunning_ = false;
    std::atomic_bool automationCancelRequested_ = false;
    bool teamReadyOcrRunning_ = false;
    bool bulletOcrRunning_ = false;
    bool bulletOcrRoundActive_ = false;
    bool bulletOcrWaitingForResult_ = false;
    bool hotkeyPickPending_ = false;
    ClientRole currentRole_ = ClientRoleUtil::DefaultRole;
};
