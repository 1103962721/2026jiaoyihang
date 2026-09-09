#include <cassert>
#include <cmath>

#include "../DF_PlayClick_Client_Qt/BulletOcrRules.h"

int main()
{
    const auto matched = EvaluateBulletOcr(120000.0, 120, 1000.0);
    assert(matched.valid);
    assert(std::abs(matched.averagePrice - 1000.0) < 0.000001);
    assert(matched.meetsUnitPrice);

    const auto expensive = EvaluateBulletOcr(120001.0, 120, 1000.0);
    assert(expensive.valid);
    assert(!expensive.meetsUnitPrice);

    assert(!EvaluateBulletOcr(100.0, 0, 1.0).valid);
    assert(!EvaluateBulletOcr(100.0, 10, -1.0).valid);

    const QRect nativeOcrRect = ScaleReferenceRect(QRect(2047, 1126, 400, 80), QSize(2560, 1440), QSize(2560, 1440));
    assert(nativeOcrRect == QRect(2047, 1126, 400, 80));

    const QRect scaledOcrRect = ScaleReferenceRect(QRect(2047, 1126, 400, 80), QSize(2560, 1440), QSize(1920, 1080));
    assert(scaledOcrRect == QRect(1535, 845, 300, 60));

    assert(DispatchReferencePoint(ClientRole::Coin) == QPoint(1756, 1000));
    assert(DispatchReferencePoint(ClientRole::Boss) == QPoint(2262, 1159));
    assert(ScaleDispatchPoint(ClientRole::Coin, QSize(1920, 1080)) == QPoint(1317, 750));
    assert(ScaleDispatchPoint(ClientRole::Boss, QSize(1920, 1080)) == QPoint(1697, 869));

    assert(!IsRectInsideClient(QRect(-1, 0, 10, 10), QSize(2560, 1440)));
    assert(IsRectInsideClient(nativeOcrRect, QSize(2560, 1440)));
    return 0;
}
