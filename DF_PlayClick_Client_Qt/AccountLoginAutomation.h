#pragma once

#include <QObject>
#include <QString>
#include <atomic>

class AccountLoginAutomation : public QObject
{
    Q_OBJECT
public:
    explicit AccountLoginAutomation(QObject* parent = nullptr);
    void start(const QString& username, const QString& password);
    void cancel();
    void cancelAndStopProcesses(const QString& reason = QString());
signals:
    void progress(const QString& text);
    void logMessage(const QString& text);
    void finished(bool success, const QString& text);
private:
    bool cancelled() const;
    bool stopProcesses(QString& errorText, const QString& reason = QString());
    bool launchSteam(QString& errorText) const;
    bool launchGame(QString& errorText) const;
    bool performSteamLogin(QString& errorText, bool& loginWindowTimedOut) const;
    bool stopSteamProcesses(QString& errorText, const QString& reason = QString());
    bool acceptSteamAgreementIfPresent() const;
    bool clickSteamOpenGameIfPresent() const;
    bool prepareAndStartLauncher(QString& errorText) const;
    QString username_;
    QString password_;
    bool waitForProcess(const QString& processName, int timeoutMs) const;
    bool waitForGameWindow(int timeoutMs) const;
    std::atomic_bool cancelled_{false};
};
