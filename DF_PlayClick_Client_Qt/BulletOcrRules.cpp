#include "BulletOcrRules.h"

#include <QtGlobal>

#include <cmath>

BulletOcrEvaluation EvaluateBulletOcr(double recognizedPrice, int bulletTotal, double unitPrice)
{
    BulletOcrEvaluation result;
    if (!std::isfinite(recognizedPrice) || recognizedPrice < 0.0)
    {
        result.errorText = QStringLiteral("OCR 价格无效");
        return result;
    }
    if (bulletTotal <= 0)
    {
        result.errorText = QStringLiteral("子弹总数必须是正整数");
        return result;
    }
    if (!std::isfinite(unitPrice) || unitPrice < 0.0)
    {
        result.errorText = QStringLiteral("设定单价不能为负数");
        return result;
    }

    result.valid = true;
    result.averagePrice = recognizedPrice / static_cast<double>(bulletTotal);
    result.meetsUnitPrice = result.averagePrice <= unitPrice;
    return result;
}

QPoint ScaleReferencePoint(const QPoint& point, const QSize& referenceSize, const QSize& clientSize)
{
    if (referenceSize.width() <= 0 || referenceSize.height() <= 0
        || clientSize.width() <= 0 || clientSize.height() <= 0)
    {
        return {};
    }

    // 坐标始终以参考客户区为基准，按当前游戏客户区宽高分别缩放。
    return QPoint(qRound(static_cast<double>(point.x()) * clientSize.width() / referenceSize.width()),
        qRound(static_cast<double>(point.y()) * clientSize.height() / referenceSize.height()));
}

QPoint DispatchReferencePoint(ClientRole role)
{
    // 出发坐标统一以 2560x1440 游戏客户区为基准，客户端只根据服务端角色选择固定点。
    return role == ClientRole::Boss ? QPoint(2262, 1159) : QPoint(1756, 1000);
}

QPoint ScaleDispatchPoint(ClientRole role, const QSize& clientSize)
{
    return ScaleReferencePoint(DispatchReferencePoint(role), QSize(2560, 1440), clientSize);
}

QRect ScaleReferenceRect(const QRect& rect, const QSize& referenceSize, const QSize& clientSize)
{
    if (referenceSize.width() <= 0 || referenceSize.height() <= 0
        || clientSize.width() <= 0 || clientSize.height() <= 0)
    {
        return {};
    }

    const QPoint topLeft = ScaleReferencePoint(rect.topLeft(), referenceSize, clientSize);
    const int width = qMax(1, qRound(static_cast<double>(rect.width()) * clientSize.width() / referenceSize.width()));
    const int height = qMax(1, qRound(static_cast<double>(rect.height()) * clientSize.height() / referenceSize.height()));
    return QRect(topLeft, QSize(width, height));
}

bool IsRectInsideClient(const QRect& rect, const QSize& clientSize)
{
    return rect.isValid() && clientSize.width() > 0 && clientSize.height() > 0
        && rect.left() >= 0 && rect.top() >= 0
        && rect.right() < clientSize.width() && rect.bottom() < clientSize.height();
}
