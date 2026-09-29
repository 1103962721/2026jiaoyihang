#include "ClientConfig.h"

#include <windows.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QUuid>
#include <QStringList>
#include <QtGlobal>

QString ClientConfig::configPath() const
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("DF_PlayClick_Client.ini"));
}

QString ClientConfig::loadLastIp() const
{
    wchar_t buffer[128]{};
    GetPrivateProfileStringW(L"Server", L"IP", L"127.0.0.1", buffer, 128, reinterpret_cast<LPCWSTR>(configPath().utf16()));
    return QString::fromWCharArray(buffer);
}

void ClientConfig::saveLastIp(const QString& ip) const
{
    WritePrivateProfileStringW(L"Server", L"IP", reinterpret_cast<LPCWSTR>(ip.utf16()), reinterpret_cast<LPCWSTR>(configPath().utf16()));
}

QString ClientConfig::loadOrCreateDeviceId() const
{
    wchar_t buffer[128]{};
    const QString path = configPath();
    GetPrivateProfileStringW(L"Client", L"DeviceId", L"", buffer, 128,
                             reinterpret_cast<LPCWSTR>(path.utf16()));
    const QString existing = QString::fromWCharArray(buffer).trimmed();
    if (!existing.isEmpty())
    {
        return existing;
    }

    const QString generated = QUuid::createUuid().toString(QUuid::WithoutBraces);
    WritePrivateProfileStringW(L"Client", L"DeviceId",
                               reinterpret_cast<LPCWSTR>(generated.utf16()),
                               reinterpret_cast<LPCWSTR>(path.utf16()));
    return generated;
}

int ClientConfig::loadClickQueueDelayMs() const
{
    const int delayMs = GetPrivateProfileIntW(L"ClickQueue", L"DelayMs", DefaultClickQueueDelayMs, reinterpret_cast<LPCWSTR>(configPath().utf16()));
    return qBound(MinClickQueueDelayMs, delayMs, MaxClickQueueDelayMs);
}

void ClientConfig::saveClickQueueDelayMs(int delayMs) const
{
    const int boundedDelayMs = qBound(MinClickQueueDelayMs, delayMs, MaxClickQueueDelayMs);
    WritePrivateProfileStringW(L"ClickQueue", L"DelayMs", reinterpret_cast<LPCWSTR>(QString::number(boundedDelayMs).utf16()), reinterpret_cast<LPCWSTR>(configPath().utf16()));
}

QString ClientConfig::loadUmiOcrPath() const
{
    wchar_t buffer[MAX_PATH]{};
    GetPrivateProfileStringW(L"OCR", L"UmiOcrPath", L"", buffer, MAX_PATH, reinterpret_cast<LPCWSTR>(configPath().utf16()));
    const QString configuredPath = QString::fromWCharArray(buffer).trimmed();
    if (!configuredPath.isEmpty() && QFileInfo::exists(configuredPath))
    {
        return configuredPath;
    }

    const QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList candidates{
        appDir.filePath(QStringLiteral("Umi-OCR.exe")),
        appDir.filePath(QStringLiteral("Umi-OCR_Paddle_v2.1.5/Umi-OCR.exe")),
        appDir.filePath(QStringLiteral("Umi-OCR/Umi-OCR.exe"))
    };
    for (const QString& candidate : candidates)
    {
        if (QFileInfo::exists(candidate))
        {
            return candidate;
        }
    }

    return configuredPath.isEmpty() ? appDir.filePath(QStringLiteral("Umi-OCR_Paddle_v2.1.5/Umi-OCR.exe")) : configuredPath;
}

int ClientConfig::loadUmiOcrPort() const
{
    return GetPrivateProfileIntW(L"OCR", L"Port", 1224, reinterpret_cast<LPCWSTR>(configPath().utf16()));
}

int ClientConfig::loadTeamReadyOcrIntervalMs() const
{
    const int intervalMs = GetPrivateProfileIntW(L"TeamReadyOCR", L"IntervalMs", 1500, reinterpret_cast<LPCWSTR>(configPath().utf16()));
    return qBound(800, intervalMs, 10000);
}

OcrRegionConfig ClientConfig::loadTeamReadyRegion() const
{
    OcrRegionConfig region;
    region.enabled = GetPrivateProfileIntW(L"TeamReadyOCR", L"Enabled", 1, reinterpret_cast<LPCWSTR>(configPath().utf16())) != 0;
    region.x = GetPrivateProfileIntW(L"TeamReadyOCR", L"X", 2048, reinterpret_cast<LPCWSTR>(configPath().utf16()));
    region.y = GetPrivateProfileIntW(L"TeamReadyOCR", L"Y", 1123, reinterpret_cast<LPCWSTR>(configPath().utf16()));
    region.width = GetPrivateProfileIntW(L"TeamReadyOCR", L"Width", 400, reinterpret_cast<LPCWSTR>(configPath().utf16()));
    region.height = GetPrivateProfileIntW(L"TeamReadyOCR", L"Height", 80, reinterpret_cast<LPCWSTR>(configPath().utf16()));
    region.referenceWidth = GetPrivateProfileIntW(L"TeamReadyOCR", L"ReferenceWidth", 2560, reinterpret_cast<LPCWSTR>(configPath().utf16()));
    region.referenceHeight = GetPrivateProfileIntW(L"TeamReadyOCR", L"ReferenceHeight", 1440, reinterpret_cast<LPCWSTR>(configPath().utf16()));
    region.enabled = region.enabled && region.width > 0 && region.height > 0 && region.referenceWidth > 0 && region.referenceHeight > 0;
    return region;
}

BulletOcrConfig ClientConfig::loadBulletOcrConfig() const
{
    BulletOcrConfig config;
    config.region.enabled = GetPrivateProfileIntW(L"BulletOCR", L"Enabled", 1, reinterpret_cast<LPCWSTR>(configPath().utf16())) != 0;
    config.region.x = GetPrivateProfileIntW(L"BulletOCR", L"X", 2047, reinterpret_cast<LPCWSTR>(configPath().utf16()));
    config.region.y = GetPrivateProfileIntW(L"BulletOCR", L"Y", 1126, reinterpret_cast<LPCWSTR>(configPath().utf16()));
    config.region.width = GetPrivateProfileIntW(L"BulletOCR", L"Width", 400, reinterpret_cast<LPCWSTR>(configPath().utf16()));
    config.region.height = GetPrivateProfileIntW(L"BulletOCR", L"Height", 80, reinterpret_cast<LPCWSTR>(configPath().utf16()));
    // OCR 区域使用用户确认的 2560x1440 游戏客户区坐标，宽高分别按当前客户区缩放。
    config.region.referenceWidth = GetPrivateProfileIntW(L"BulletOCR", L"ReferenceWidth", 2560, reinterpret_cast<LPCWSTR>(configPath().utf16()));
    config.region.referenceHeight = GetPrivateProfileIntW(L"BulletOCR", L"ReferenceHeight", 1440, reinterpret_cast<LPCWSTR>(configPath().utf16()));
    config.retryX = GetPrivateProfileIntW(L"BulletOCR", L"RetryX", 418, reinterpret_cast<LPCWSTR>(configPath().utf16()));
    config.retryY = GetPrivateProfileIntW(L"BulletOCR", L"RetryY", 937, reinterpret_cast<LPCWSTR>(configPath().utf16()));
    // 重试点尚未重新取 2K 坐标，保留原 1650x1080 参考系，避免随 OCR 参考系变化而漂移。
    config.retryReferenceWidth = GetPrivateProfileIntW(L"BulletOCR", L"RetryReferenceWidth", 1650, reinterpret_cast<LPCWSTR>(configPath().utf16()));
    config.retryReferenceHeight = GetPrivateProfileIntW(L"BulletOCR", L"RetryReferenceHeight", 1080, reinterpret_cast<LPCWSTR>(configPath().utf16()));
    config.escDelayMs = qBound(0, static_cast<int>(GetPrivateProfileIntW(L"BulletOCR", L"EscDelayMs", 100, reinterpret_cast<LPCWSTR>(configPath().utf16()))), 10000);
    config.nextRoundDelayMs = qBound(0, static_cast<int>(GetPrivateProfileIntW(L"BulletOCR", L"NextRoundDelayMs", 0, reinterpret_cast<LPCWSTR>(configPath().utf16()))), 10000);
    config.region.enabled = config.region.enabled
        && config.region.x >= 0 && config.region.y >= 0
        && config.region.width > 0 && config.region.height > 0
        && config.region.referenceWidth > 0 && config.region.referenceHeight > 0
        && config.retryReferenceWidth > 0 && config.retryReferenceHeight > 0;
    return config;
}

double ClientConfig::loadUnitPrice() const
{
    wchar_t buffer[128]{};
    GetPrivateProfileStringW(L"Ocr", L"UnitPrice", L"0", buffer, 128,
                             reinterpret_cast<LPCWSTR>(configPath().utf16()));
    bool ok = false;
    const double value = QString::fromWCharArray(buffer).toDouble(&ok);
    return ok && qIsFinite(value) && value >= 0.0 ? value : DefaultUnitPrice;
}

void ClientConfig::saveUnitPrice(double unitPrice) const
{
    const double safeValue = qIsFinite(unitPrice) && unitPrice >= 0.0 ? unitPrice : DefaultUnitPrice;
    const QString text = QString::number(safeValue, 'g', 17);
    WritePrivateProfileStringW(L"Ocr", L"UnitPrice", reinterpret_cast<LPCWSTR>(text.utf16()),
                               reinterpret_cast<LPCWSTR>(configPath().utf16()));
}

int ClientConfig::loadBulletTotal() const
{
    wchar_t buffer[64]{};
    GetPrivateProfileStringW(L"Ocr", L"BulletTotal", L"1", buffer, 64,
                             reinterpret_cast<LPCWSTR>(configPath().utf16()));
    bool ok = false;
    const int value = QString::fromWCharArray(buffer).toInt(&ok);
    return ok && value >= 1 ? value : DefaultBulletTotal;
}

void ClientConfig::saveBulletTotal(int bulletTotal) const
{
    const int safeValue = bulletTotal >= 1 ? bulletTotal : DefaultBulletTotal;
    const QString text = QString::number(safeValue);
    WritePrivateProfileStringW(L"Ocr", L"BulletTotal", reinterpret_cast<LPCWSTR>(text.utf16()),
                               reinterpret_cast<LPCWSTR>(configPath().utf16()));
}
