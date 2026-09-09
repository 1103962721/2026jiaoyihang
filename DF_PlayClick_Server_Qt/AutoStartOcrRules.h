#pragma once

#include <QString>

namespace AutoStartOcrRules
{
    inline QString NormalizeOcrText(QString text)
    {
        text.remove(QChar::Space);
        text.remove(QChar('\t'));
        text.remove(QChar('\r'));
        text.remove(QChar('\n'));
        return text;
    }

    inline bool ContainsCancelActionMarker(const QString& text)
    {
        return NormalizeOcrText(text).contains(QStringLiteral("取消行动"));
    }

    inline bool ContainsActionRegionWaitingMarker(const QString& text)
    {
        const QString normalized = NormalizeOcrText(text);
        return normalized.contains(QStringLiteral("配装"))
            || normalized.contains(QStringLiteral("出发"))
            || normalized.contains(QStringLiteral("取消行动"));
    }

    inline bool ContainsMapTypeMarker(const QString& text)
    {
        const QString normalized = NormalizeOcrText(text);
        return normalized.contains(QStringLiteral("机密"))
            || normalized.contains(QStringLiteral("普通"))
            || normalized.contains(QStringLiteral("绝密"));
    }
}
