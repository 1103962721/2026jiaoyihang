#include "AuctionPurchaseCoordinator.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QScopedValueRollback>
#include <QSettings>
#include <QSet>

namespace
{
QString settingsPath() { return QCoreApplication::applicationDirPath() + QStringLiteral("/DF_PlayClick_Server.ini"); }
}

AuctionPurchaseCoordinator::AuctionPurchaseCoordinator(ServerConnectionManager& connections, QObject* parent)
    : QObject(parent), connections_(connections)
{
    QSettings settings(settingsPath(), QSettings::IniFormat);
    QJsonObject object;
    if (AuctionPurchase::decode(settings.value(QStringLiteral("AuctionPurchase/Config")).toString(), object))
        AuctionPurchase::Config::fromJson(object, config_);
    if (AuctionPurchase::decode(settings.value(QStringLiteral("AuctionPurchase/Catalog")).toString(), object))
        AuctionPurchase::Catalog::fromJson(object, cachedCatalog_);
    connect(&connections_, &ServerConnectionManager::clientsChanged, this, &AuctionPurchaseCoordinator::process);
    connect(&connections_, &ServerConnectionManager::auctionBindingChanged, this, [this](quint64 clientId, bool bound) {
        if (!bound)
        {
            if (states_.remove(clientId)) { emit catalogChanged(); emit statesChanged(); }
        }
        else process();
    });
    connect(&connections_, &ServerConnectionManager::auctionCatalogReceived, this, &AuctionPurchaseCoordinator::receiveCatalog);
    connect(&connections_, &ServerConnectionManager::auctionConfigResultReceived, this, &AuctionPurchaseCoordinator::receiveResult);
    timer_.setInterval(250);
    connect(&timer_, &QTimer::timeout, this, &AuctionPurchaseCoordinator::process);
    timer_.start();
}

void AuctionPurchaseCoordinator::save() const
{
    QSettings settings(settingsPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("AuctionPurchase/Config"), AuctionPurchase::encode(config_.toJson()));
    if (!cachedCatalog_.purchaseModes.isEmpty())
        settings.setValue(QStringLiteral("AuctionPurchase/Catalog"), AuctionPurchase::encode(cachedCatalog_.toJson()));
    settings.sync();
}

void AuctionPurchaseCoordinator::updateConfig(const AuctionPurchase::Config& config)
{
    AuctionPurchase::Config checked;
    if (!AuctionPurchase::Config::fromJson(config.toJson(), checked) || checked.toJson() == config_.toJson()) return;
    config_ = checked;
    ++revision_;
    for (auto& state : states_) { state.needsApply = true; state.sentRevision = 0; state.attempts = 0; state.acknowledged = false; state.error.clear(); }
    save();
    emit configChanged();
    process();
}

AuctionPurchase::Catalog AuctionPurchaseCoordinator::catalog() const
{
    AuctionPurchase::Catalog common;
    bool first = true;
    for (auto it = states_.cbegin(); it != states_.cend(); ++it)
    {
        if (!it->hasCatalog) continue;
        if (first) { common = it->catalog; first = false; continue; }
        for (auto index = common.purchaseModes.size(); index-- > 0;)
            if (!it->catalog.purchaseModes.contains(common.purchaseModes[index])) common.purchaseModes.removeAt(index);
        for (int slot = 0; slot < 5; ++slot)
            for (auto index = common.ammunition[slot].size(); index-- > 0;)
                if (!it->catalog.ammunition[slot].contains(common.ammunition[slot][index])) common.ammunition[slot].removeAt(index);
    }
    return first ? cachedCatalog_ : common;
}

int AuctionPurchaseCoordinator::catalogClientCount() const
{
    int count = 0;
    for (const auto& state : states_) if (state.hasCatalog) ++count;
    return count;
}

QString AuctionPurchaseCoordinator::syncStatus(quint64 clientId) const { return states_.value(clientId).status; }
QString AuctionPurchaseCoordinator::syncError(quint64 clientId) const { return states_.value(clientId).error; }
bool AuctionPurchaseCoordinator::syncSuccessful(quint64 clientId) const
{ return states_.contains(clientId) && states_.value(clientId).appliedRevision == revision_ && !states_.value(clientId).needsApply; }

void AuctionPurchaseCoordinator::process()
{
    if (processing_) return;
    QScopedValueRollback<bool> guard(processing_, true);
    const auto clients = connections_.clients();
    QSet<quint64> bound;
    bool changed = false, catalogsChanged = false;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QString invalid;
    const bool ready = config_.ready(invalid);
    for (const auto& client : clients)
    {
        if (!client.connected || !client.auctionBound) continue;
        bound.insert(client.id);
        if (!states_.contains(client.id)) { states_.insert(client.id, {}); changed = true; }
        auto& state = states_[client.id];
        const auto previous = state.status;
        if (client.actionRunning && state.needsApply)
            state.status = QStringLiteral("本轮结束后应用");
        else if (!state.hasCatalog)
        {
            if (state.attempts < 3 && (state.sentAt == 0 || now - state.sentAt > 12000))
            {
                ++state.attempts; state.sentAt = now;
                QString error;
                if (!connections_.requestAuctionCatalog(client.id, error)) state.error = error;
                state.status = QStringLiteral("正在读取 Qt 选项");
            }
            else if (state.attempts >= 3 && now - state.sentAt > 12000)
            { state.status = QStringLiteral("读取 Qt 选项超时"); state.error = QStringLiteral("请检查客户端版本或重新绑定 Qt 窗口"); }
        }
        else if (!ready)
        { state.status = QStringLiteral("参数待补全"); state.error = invalid; }
        else if (state.needsApply)
        {
            const auto& entry = config_.entries[config_.selected - 1];
            if (!state.catalog.purchaseModes.contains(config_.mode) || !state.catalog.ammunition[config_.selected - 1].contains(entry.ammo))
            { state.status = QStringLiteral("目标 Qt 不支持所选参数"); state.error = QStringLiteral("目标购买方式或子弹下拉列表与服务端不同"); }
            else if (state.sentRevision != revision_ && state.attempts < 3)
            {
                QString error;
                if (connections_.sendAuctionConfig(client.id, revision_, AuctionPurchase::encode(config_.toJson()), error))
                { state.sentRevision = revision_; state.sentAt = now; ++state.attempts; state.status = QStringLiteral("正在同步并回读"); state.error.clear(); }
                else { state.status = QStringLiteral("发送失败"); state.error = error; }
            }
            else if (state.sentRevision == revision_ && !state.acknowledged && now - state.sentAt > 12000)
            {
                state.status = QStringLiteral("等待客户端同步结果");
                // A busy client explicitly acknowledges pending work. Retry only
                // requests that never produced an acknowledgement, at most twice.
                if (state.attempts < 3) state.sentRevision = 0;
            }
        }
        changed = changed || previous != state.status;
    }
    for (auto it = states_.begin(); it != states_.end();)
    {
        if (!bound.contains(it.key())) { it = states_.erase(it); changed = catalogsChanged = true; }
        else ++it;
    }
    if (catalogsChanged) emit catalogChanged();
    if (changed) emit statesChanged();
}

void AuctionPurchaseCoordinator::receiveCatalog(quint64 clientId, const QString& data)
{
    if (!states_.contains(clientId)) return;
    QJsonObject json;
    AuctionPurchase::Catalog catalog;
    if (!AuctionPurchase::decode(data, json) || !AuctionPurchase::Catalog::fromJson(json, catalog))
    { emit logAppended(QStringLiteral("客户端 %1 返回的 Qt 下拉列表格式无效").arg(clientId)); return; }
    auto& state = states_[clientId];
    state.catalog = catalog; state.hasCatalog = true; state.attempts = 0; state.sentAt = 0;
    cachedCatalog_ = this->catalog();
    save();
    emit catalogChanged();
    emit logAppended(QStringLiteral("客户端 %1：已读取 Qt 五款子弹列表，第 1 款 %2 项").arg(clientId).arg(catalog.ammunition[0].size()));
    process();
    emit statesChanged();
}

void AuctionPurchaseCoordinator::receiveResult(quint64 clientId, int revision, int status, const QString& data)
{
    if (!states_.contains(clientId) || revision != revision_ || states_[clientId].sentRevision != revision) return;
    QJsonObject json;
    if (!AuctionPurchase::decode(data, json) || !json.value(QStringLiteral("text")).isString()) return;
    auto& state = states_[clientId];
    const auto text = json.value(QStringLiteral("text")).toString();
    if (status == 2)
    { state.acknowledged = true; state.status = QStringLiteral("已接收，等待本轮结束后应用"); state.sentAt = QDateTime::currentMSecsSinceEpoch(); }
    else
    {
        state.needsApply = false;
        state.appliedRevision = status == 1 ? revision : 0;
        state.status = status == 1 ? QStringLiteral("已同步") : QStringLiteral("同步失败");
        state.error = status == 1 ? QString() : text;
        emit logAppended(QStringLiteral("客户端 %1 Qt 参数%2：%3").arg(clientId).arg(state.status, text));
    }
    emit statesChanged();
}
