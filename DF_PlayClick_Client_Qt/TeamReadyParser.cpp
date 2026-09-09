#include "TeamReadyParser.h"

#include <QRegularExpression>

TeamReadyResult TeamReadyParser::parse(const QString& text) const
{
    TeamReadyResult result;
    QString normalized = text;
    normalized.remove(QRegularExpression(QStringLiteral("\\s+")));
    normalized.replace(QStringLiteral("【"), QStringLiteral("["));
    normalized.replace(QStringLiteral("】"), QStringLiteral("]"));
    normalized.replace(QStringLiteral("（"), QStringLiteral("("));
    normalized.replace(QStringLiteral("）"), QStringLiteral(")"));

    const int startIndex = normalized.indexOf(QStringLiteral("出发"));
    const QString searchText = startIndex >= 0 ? normalized.mid(startIndex) : normalized;
    const QRegularExpression pattern(QStringLiteral("(\\d{1,2})/(\\d{1,2})"));
    const QRegularExpressionMatch match = pattern.match(searchText);
    if (!match.hasMatch())
    {
        if (startIndex >= 0)
        {
            result.state = 0;
            result.current = 0;
            result.total = 0;
            result.displayText = QStringLiteral("[0/0]");
        }
        return result;
    }

    const int current = match.captured(1).toInt();
    const int total = match.captured(2).toInt();
    if (current < 0 || total <= 0 || current > total)
    {
        return result;
    }

    result.current = current;
    result.total = total;
    result.state = current == total ? 1 : 0;
    result.displayText = QStringLiteral("[%1/%2]").arg(current).arg(total);
    return result;
}
