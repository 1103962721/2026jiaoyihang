#pragma once

#include <QRect>
#include <QString>

#include <functional>

class QJsonObject;

class UmiOcrRunner
{
public:
    QString recognizeImage(const QString& imagePath, const QString& executablePath, int port, QString& errorText,
        int cpuThreads = 12, const std::function<bool()>& isCancelled = {}) const;
    bool findTextBounds(const QString& imagePath, const QString& executablePath, int port, const QString& keyword,
        QRect& bounds, QString& errorText, bool exactMatch = false, QString* recognizedText = nullptr) const;
    bool findBulletTextBounds(const QString& imagePath, const QString& executablePath, int port,
        const QString& keyword, QRect& bounds, QString& errorText, QString* recognizedText = nullptr) const;

private:
    bool requestOcr(const QString& imagePath, const QString& executablePath, int port, const QString& dataFormat,
        QJsonObject& responseObject, QString& errorText, int cpuThreads = 12,
        const std::function<bool()>& isCancelled = {}) const;
    bool ensureServerRunning(const QString& executablePath, int port, QString& errorText,
        const std::function<bool()>& isCancelled = {}) const;
    bool isServerReady(int port, int timeoutMs = 5000) const;
};
