#pragma once

#include <windows.h>

#include <QObject>
#include <QString>
#include <atomic>
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
    void monitorStatus(const QString& text);
    void monitorStopped();

private:
    void run(HWND gameWindow, QString ocrExecutable, int ocrPort);

    std::atomic_bool stopRequested_{false};
    std::atomic_bool running_{false};
    std::thread worker_;
    std::atomic_bool detachedStop_{false};
};
