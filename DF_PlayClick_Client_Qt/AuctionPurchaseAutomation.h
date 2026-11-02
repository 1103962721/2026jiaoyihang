#pragma once

#include <windows.h>
#include <QObject>
#include <QThread>
#include <QHash>
#include <atomic>
#include "../Shared/AuctionPurchaseConfig.h"

class AuctionPurchaseAutomation : public QObject
{
    Q_OBJECT
public:
    explicit AuctionPurchaseAutomation(QObject* parent = nullptr);
    ~AuctionPurchaseAutomation() override;
    bool readCatalog(HWND hwnd);
    bool apply(HWND hwnd, int revision, const AuctionPurchase::Config& config);
    bool busy() const { return worker_ != nullptr; }
    void cancel();
    void shutdown();
signals:
    void completed(int revision, bool success, const QString& catalog, const QString& text);
private:
    bool start(HWND hwnd, int revision, const AuctionPurchase::Config& config);
    QThread* worker_ = nullptr;
    HWND catalogWindow_ = nullptr;
    QHash<QString, quintptr> comboPopups_;
    std::atomic_bool canceled_{false};
};
