#pragma once

#include <windows.h>
#include <QObject>
#include <QThread>
#include <atomic>

class AuctionControlAutomation : public QObject
{
    Q_OBJECT
public:
    enum Mode { CheckReady, Prepare, Round };
    explicit AuctionControlAutomation(QObject* parent = nullptr);
    ~AuctionControlAutomation() override;
    static bool isTargetWindow(HWND hwnd);
    bool start(HWND hwnd, Mode mode, int requestId, const std::atomic_bool* externalCancel);
    bool busy() const { return worker_ != nullptr; }
    void cancel();
    void shutdown();
signals:
    void logAppended(const QString& text);
    void completed(int mode, int requestId, bool success, bool ready, bool canceled, const QString& text);
private:
    QThread* worker_ = nullptr;
    std::atomic_bool canceled_{false};
};
