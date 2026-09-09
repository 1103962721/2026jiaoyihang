#include "OcrNumberParser.h"

#include <QRegularExpression>
#include <QStringList>

namespace
{
QString CompactOcrText(QString text)
{
    text.remove(QRegularExpression(QStringLiteral("\\s")));
    text.remove(QStringLiteral(","));
    text.remove(QStringLiteral("，"));
    return text;
}

QStringList ExtractNumericRuns(const QString& text)
{
    QStringList runs;
    const QRegularExpression expression(QStringLiteral("[0-9]+(?:\\.[0-9]+)?"));
    auto matches = expression.globalMatch(text);
    while (matches.hasNext())
    {
        runs.append(matches.next().captured(0));
    }
    return runs;
}

QString NormalizeNumericRun(QString run)
{
    if (run.length() > 1 && run.startsWith(QLatin1Char('0')))
    {
        // 金币图标偶尔会识别为前导 0，只移除一个，避免误伤真实数值。
        run.remove(0, 1);
    }

    static const QRegularExpression ThousandsDotPattern(QStringLiteral("^[0-9]{1,3}\\.[0-9]{3}$"));
    if (ThousandsDotPattern.match(run).hasMatch())
    {
        // 例如 194,400 被识别成 194.400 时，按整数价格恢复。
        run.remove(QLatin1Char('.'));
    }
    return run;
}
}

OcrPriceParseResult ParseOcrPrice(const QString& rawText)
{
    OcrPriceParseResult result;
    const QString compactText = CompactOcrText(rawText);
    const QStringList runs = ExtractNumericRuns(compactText);
    if (runs.isEmpty())
    {
        result.debugText = QStringLiteral("未找到数字：%1").arg(rawText);
        return result;
    }

    QString bestRun = runs.first();
    for (const QString& run : runs)
    {
        if (run.length() > bestRun.length())
        {
            bestRun = run;
        }
    }

    const QString normalizedText = NormalizeNumericRun(bestRun);
    bool ok = false;
    const double price = normalizedText.toDouble(&ok);
    if (!ok)
    {
        result.debugText = QStringLiteral("数字转换失败：%1").arg(normalizedText);
        return result;
    }

    result.ok = true;
    result.price = price;
    result.normalizedText = normalizedText;
    result.debugText = QStringLiteral("原文：%1；清洗：%2；候选：%3；采用：%4")
        .arg(rawText, compactText, runs.join(QStringLiteral("/")), normalizedText);
    return result;
}
