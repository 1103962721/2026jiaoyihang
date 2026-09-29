#pragma once

#include <QRect>
#include <QString>

class QJsonObject;

class UmiOcrRunner
{
public:
    QString recognizeImage(const QString& imagePath, const QString& executablePath, int port, QString& errorText) const;
    bool findTextBounds(const QString& imagePath, const QString& executablePath, int port, const QString& keyword,
        QRect& bounds, QString& errorText, bool exactMatch = false, QString* recognizedText = nullptr) const;

private:
    bool requestOcr(const QString& imagePath, const QString& executablePath, int port, const QString& dataFormat,
        QJsonObject& responseObject, QString& errorText) const;
    bool ensureServerRunning(const QString& executablePath, int port, QString& errorText) const;
    bool isServerReady(int port) const;
};
