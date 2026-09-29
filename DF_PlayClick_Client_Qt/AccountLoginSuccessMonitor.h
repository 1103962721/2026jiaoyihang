#pragma once

#include <windows.h>
#include <QObject>
#include <QString>
#include <atomic>
#include <thread>

class AccountLoginSuccessMonitor : public QObject
{
    Q_OBJECT
public:
    explicit AccountLoginSuccessMonitor(QObject* parent = nullptr);
    ~AccountLoginSuccessMonitor() override;
    void start(HWND gameWindow, const QString& ocrExecutable, int ocrPort);
    void stop();
    void setStage(int stage);
    bool isRunning() const;
signals:
    void ocrRequested(int stage);
    void loginReady();
    void bannedDetected(const QString& recognizedText);
    void residentFound();
    void tabFound();
    void retrySpace();
    void monitorStatus(const QString& text);
    void monitorFinished();
private:
    void run(HWND gameWindow, QString ocrExecutable, int ocrPort);
    std::atomic_bool stopRequested_{false};
    std::atomic_bool running_{false};
    std::atomic_int stage_{0};
    std::thread worker_;
};
