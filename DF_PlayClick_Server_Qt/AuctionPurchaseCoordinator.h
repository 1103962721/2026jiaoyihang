#pragma once

#include <QObject>
#include <QHash>
#include <QTimer>
#include "ServerConnectionManager.h"
#include "../Shared/AuctionPurchaseConfig.h"

class AuctionPurchaseCoordinator : public QObject
{
    Q_OBJECT
public:
    explicit AuctionPurchaseCoordinator(ServerConnectionManager& connections, QObject* parent = nullptr);
    const AuctionPurchase::Config& config() const { return config_; }
    AuctionPurchase::Catalog catalog() const;
    void updateConfig(const AuctionPurchase::Config& config);
    QString syncStatus(quint64 clientId) const;
    QString syncError(quint64 clientId) const;
    bool syncSuccessful(quint64 clientId) const;
    int catalogClientCount() const;
signals:
    void configChanged();
    void catalogChanged();
    void statesChanged();
    void logAppended(const QString& text);
private:
    void process();
    void receiveCatalog(quint64 clientId, const QString& data);
    void receiveResult(quint64 clientId, int revision, int status, const QString& data);
    void save() const;
    struct State
    {
        AuctionPurchase::Catalog catalog;
        bool hasCatalog = false;
        bool needsApply = true;
        int sentRevision = 0;
        int appliedRevision = 0;
        int attempts = 0;
        bool acknowledged = false;
        qint64 sentAt = 0;
        QString status = QStringLiteral("正在读取 Qt 选项");
        QString error;
    };
    ServerConnectionManager& connections_;
    AuctionPurchase::Config config_;
    AuctionPurchase::Catalog cachedCatalog_;
    QHash<quint64, State> states_;
    QTimer timer_;
    int revision_ = 1;
    bool processing_ = false;
};
