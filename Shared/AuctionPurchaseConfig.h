#pragma once

#include <array>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>

namespace AuctionPurchase
{
struct Slot
{
    QString ammo;
    QString threshold;
};

inline QStringList modes()
{
    return { QStringLiteral("等待报价低于阈值"), QStringLiteral("服务端限价尝试（不等待公开价降低）"),
        QStringLiteral("原生最低价（按游戏串行流程）"), QStringLiteral("备战弹药串行（仅购买子弹）") };
}

struct Config
{
    int selected = 1;
    QString mode = modes().at(2);
    std::array<Slot, 5> entries;

    QJsonObject toJson() const
    {
        QJsonArray items;
        for (const auto& item : entries)
            items.append(QJsonObject{{QStringLiteral("ammo"), item.ammo}, {QStringLiteral("threshold"), item.threshold}});
        return {{QStringLiteral("selected"), selected}, {QStringLiteral("mode"), mode}, {QStringLiteral("slots"), items}};
    }

    static bool fromJson(const QJsonObject& json, Config& config)
    {
        Config value;
        value.selected = json.value(QStringLiteral("selected")).toInt(0);
        value.mode = json.value(QStringLiteral("mode")).toString();
        const auto items = json.value(QStringLiteral("slots")).toArray();
        if (value.selected < 1 || value.selected > 5 || !modes().contains(value.mode) || items.size() != 5)
            return false;
        for (int index = 0; index < 5; ++index)
        {
            if (!items[index].isObject()) return false;
            const auto item = items[index].toObject();
            if (!item.value(QStringLiteral("ammo")).isString() || !item.value(QStringLiteral("threshold")).isString()) return false;
            value.entries[index] = {item.value(QStringLiteral("ammo")).toString(), item.value(QStringLiteral("threshold")).toString()};
            if (value.entries[index].ammo.size() > 512 || value.entries[index].threshold.size() > 20) return false;
        }
        config = value;
        return true;
    }

    bool ready(QString& error) const
    {
        if (selected < 1 || selected > 5 || !modes().contains(mode))
        { error = QStringLiteral("购买款式或购买方式无效"); return false; }
        const auto& item = entries[selected - 1];
        if (item.ammo.trimmed().isEmpty())
        { error = QStringLiteral("请选择子弹名称"); return false; }
        if (mode != modes().at(2) && !QRegularExpression(QStringLiteral("^\\d+(?:\\.\\d+)?$")).match(item.threshold).hasMatch())
        { error = QStringLiteral("请填写有效的购买阈值"); return false; }
        return true;
    }
};

struct Catalog
{
    QStringList purchaseModes;
    std::array<QStringList, 5> ammunition;

    QJsonObject toJson() const
    {
        QJsonArray items;
        for (const auto& names : ammunition) items.append(QJsonArray::fromStringList(names));
        return {{QStringLiteral("modes"), QJsonArray::fromStringList(purchaseModes)}, {QStringLiteral("ammunition"), items}};
    }

    static bool fromJson(const QJsonObject& json, Catalog& catalog)
    {
        const auto items = json.value(QStringLiteral("ammunition")).toArray();
        if (items.size() != 5 || !json.value(QStringLiteral("modes")).isArray()) return false;
        Catalog value;
        auto readNames = [](const QJsonArray& source, QStringList& names) {
            if (source.isEmpty() || source.size() > 1000) return false;
            for (const auto& entry : source)
            {
                if (!entry.isString() || entry.toString().isEmpty() || entry.toString().size() > 512) return false;
                if (!names.contains(entry.toString())) names.append(entry.toString());
            }
            return true;
        };
        if (!readNames(json.value(QStringLiteral("modes")).toArray(), value.purchaseModes)) return false;
        for (int index = 0; index < 5; ++index)
            if (!items[index].isArray() || !readNames(items[index].toArray(), value.ammunition[index])) return false;
        catalog = value;
        return true;
    }
};

inline QString encode(const QJsonObject& value)
{
    return QString::fromLatin1(QJsonDocument(value).toJson(QJsonDocument::Compact).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

inline bool decode(const QString& text, QJsonObject& value)
{
    if (text.isEmpty() || text.size() > 512000) return false;
    const auto result = QByteArray::fromBase64Encoding(text.toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!result) return false;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(result.decoded, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    value = document.object();
    return true;
}
}
