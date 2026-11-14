#pragma once

#include <QObject>
#include <QString>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

class AccountLoginAutomation : public QObject
{
    Q_OBJECT
public:
    explicit AccountLoginAutomation(QObject* parent = nullptr);
    ~AccountLoginAutomation() override;
    void start(const QString& username, const QString& password);
    void cancel();
    void cancelAndStopProcesses(const QString& reason = QString());
signals:
    void progress(const QString& text);
    void logMessage(const QString& text);
    void finished(bool success, const QString& text);
private:
    struct Task
    {
        QString username;
        QString password;
        QString stopReason;
        bool stopOnly = false;
        quint64 id = 0;
        std::atomic_bool cancelled{false};
        std::atomic_bool completed{false};
    };
    void submit(const std::shared_ptr<Task>& task);
    void workerLoop();
    void runLogin();
    void postResult(const std::function<void()>& deliver);
    void reportProgress(const QString& text);
    void reportLog(const QString& text);
    void reportFinished(bool success, const QString& text);
    bool cancelled() const;
    bool stopProcesses(QString& errorText, const QString& reason = QString());
    bool launchSteam(QString& errorText) const;
    bool launchGame(QString& errorText) const;
    bool performSteamLogin(QString& errorText, bool& steamExited);
    bool acceptSteamAgreementIfPresent() const;
    bool clickSteamOpenGameIfPresent() const;
    bool prepareAndStartLauncher(QString& errorText, bool& stopRetrying, bool& steamExited);
    bool waitForProcess(const QString& processName, int timeoutMs) const;
    bool waitForGameWindow(int timeoutMs);
    std::mutex taskMutex_;
    std::condition_variable taskChanged_;
    std::shared_ptr<Task> latestTask_;
    std::shared_ptr<Task> pendingTask_;
    // Only the single worker accesses workerTask_; callers cancel via latestTask_.
    std::shared_ptr<Task> workerTask_;
    quint64 nextTaskId_ = 0;
    bool stopping_ = false;
    std::thread worker_;
};
