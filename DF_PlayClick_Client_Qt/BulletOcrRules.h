#pragma once

#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>

#include "../Shared/ClientRole.h"

struct BulletOcrEvaluation
{
    bool valid = false;
    double averagePrice = 0.0;
    bool meetsUnitPrice = false;
    QString errorText;
};

BulletOcrEvaluation EvaluateBulletOcr(double recognizedPrice, int bulletTotal, double unitPrice);
QPoint ScaleReferencePoint(const QPoint& point, const QSize& referenceSize, const QSize& clientSize);
QPoint DispatchReferencePoint(ClientRole role);
QPoint ScaleDispatchPoint(ClientRole role, const QSize& clientSize);
QRect ScaleReferenceRect(const QRect& rect, const QSize& referenceSize, const QSize& clientSize);
bool IsRectInsideClient(const QRect& rect, const QSize& clientSize);
