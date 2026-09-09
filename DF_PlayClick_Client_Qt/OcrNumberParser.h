#pragma once

#include <QString>

struct OcrPriceParseResult
{
    bool ok = false;
    double price = 0.0;
    QString normalizedText;
    QString debugText;
};

OcrPriceParseResult ParseOcrPrice(const QString& rawText);
