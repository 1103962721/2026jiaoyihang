#pragma once

#include <windows.h>

#include <QObject>
#include <QString>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

class AccountBanMonitor : public QObject
{
    Q_OBJECT
public:
    static std::atomic_bool loginOcrInFlight;
    explicit AccountBanMonitor(QObject* parent = nullptr);
    ~AccountBanMonitor() override;

    void start(HWND gameWindow, const QString& ocrExecutable, int ocrPort);
    void stop();
    bool isRunning() const;

signals:
    void bannedDetected(const QString& recognizedText);
    void startupFailedDetected(const QString& recognizedText);
    void monitorStatus(const QString& text);
    void monitorStopped();

private:
    using StopToken = std::shared_ptr<std::atomic_bool>;
    void run(HWND gameWindow, QString ocrExecutable, int ocrPort, const StopToken& token);

    std::atomic_bool running_{false};
    std::thread worker_;
    std::mutex stateMutex_;
    StopToken activeToken_;
};
