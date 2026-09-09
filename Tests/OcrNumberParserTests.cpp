#include <cassert>

#include "../DF_PlayClick_Client_Qt/OcrNumberParser.h"

int main()
{
    const auto commaPrice = ParseOcrPrice(QStringLiteral("120,668"));
    assert(commaPrice.ok && commaPrice.price == 120668.0);

    const auto leadingIcon = ParseOcrPrice(QStringLiteral("金币 0 120,668"));
    assert(leadingIcon.ok && leadingIcon.price == 120668.0);

    const auto plainPrice = ParseOcrPrice(QStringLiteral("120660"));
    assert(plainPrice.ok && plainPrice.price == 120660.0);

    const auto dottedThousands = ParseOcrPrice(QStringLiteral("194.400"));
    assert(dottedThousands.ok && dottedThousands.price == 194400.0);

    const auto missing = ParseOcrPrice(QStringLiteral("没有价格"));
    assert(!missing.ok);
    return 0;
}
