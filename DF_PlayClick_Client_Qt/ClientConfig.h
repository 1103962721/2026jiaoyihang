#pragma once

#include <QString>

struct OcrRegionConfig
{
    bool enabled = false;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int referenceWidth = 2560;
    int referenceHeight = 1440;
};

struct BulletOcrConfig
{
    OcrRegionConfig region;
    int retryX = 418;
    int retryY = 937;
    int retryReferenceWidth = 1650;
    int retryReferenceHeight = 1080;
    int escDelayMs = 100;
    int nextRoundDelayMs = 0;
};

class ClientConfig
{
public:
    static constexpr int DefaultClickQueueDelayMs = 50;
    static constexpr int MinClickQueueDelayMs = 0;
    static constexpr int MaxClickQueueDelayMs = 5000;
    static constexpr double DefaultUnitPrice = 0.0;
    static constexpr int DefaultBulletTotal = 1;

    QString loadLastIp() const;
    void saveLastIp(const QString& ip) const;
    QString loadOrCreateDeviceId() const;
    int loadClickQueueDelayMs() const;
    void saveClickQueueDelayMs(int delayMs) const;
    QString loadUmiOcrPath() const;
    QString loadAdbPath() const;
    int loadUmiOcrPort() const;
    int loadTeamReadyOcrIntervalMs() const;
    OcrRegionConfig loadTeamReadyRegion() const;
    BulletOcrConfig loadBulletOcrConfig() const;
    double loadUnitPrice() const;
    void saveUnitPrice(double unitPrice) const;
    int loadBulletTotal() const;
    void saveBulletTotal(int bulletTotal) const;

private:
    QString configPath() const;
};
