#include "BundledAdb.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>
#include <mutex>

namespace BundledAdb
{
QString executablePath(QString& error)
{
    error.clear();
    static std::mutex mutex;
    const std::lock_guard<std::mutex> guard(mutex);
    static QString verifiedDirectory;
    static qint64 verifiedSizes[4]{};
    const QStringList names{ QStringLiteral("adb.exe"), QStringLiteral("AdbWinApi.dll"),
        QStringLiteral("AdbWinUsbApi.dll"), QStringLiteral("NOTICE.txt") };
    const QDir directory(QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath(QStringLiteral("DF_PlayClick/adb/33.0.2")));
    const auto filesIntact = [&]() {
        if (verifiedDirectory != directory.absolutePath()) return false;
        for (int i = 0; i < names.size(); ++i)
        {
            const QFileInfo file(directory.filePath(names[i]));
            if (!file.isFile() || file.size() != verifiedSizes[i]) return false;
        }
        return true;
    };
    if (filesIntact()) return directory.filePath(names[0]);
    verifiedDirectory.clear();
    if (!QDir().mkpath(directory.absolutePath()))
    {
        error = QStringLiteral("无法创建内置 ADB 目录：%1").arg(directory.absolutePath());
        return {};
    }
    // Separate client processes may initialize the same packaged ADB simultaneously.
    QLockFile lock(directory.filePath(QStringLiteral("extract.lock")));
    if (!lock.tryLock(5000))
    {
        error = QStringLiteral("内置 ADB 正在初始化或目录无法写入，请稍后重试：%1")
            .arg(directory.absolutePath());
        return {};
    }
    for (int i = 0; i < names.size(); ++i)
    {
        QFile resource(QStringLiteral(":/bundled-adb/") + names[i]);
        if (!resource.open(QIODevice::ReadOnly))
        {
            error = QStringLiteral("客户端缺少内置 ADB 资源：%1").arg(names[i]);
            return {};
        }
        const QByteArray bytes = resource.readAll();
        QFile existing(directory.filePath(names[i]));
        if (existing.open(QIODevice::ReadOnly)
            && existing.size() == bytes.size()
            && QCryptographicHash::hash(existing.readAll(), QCryptographicHash::Sha256)
                == QCryptographicHash::hash(bytes, QCryptographicHash::Sha256))
        {
            verifiedSizes[i] = bytes.size();
            continue;
        }
        existing.close();
        QSaveFile output(directory.filePath(names[i]));
        if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit())
        {
            error = QStringLiteral("无法释放内置 ADB 文件 %1：%2").arg(names[i], output.errorString());
            return {};
        }
        verifiedSizes[i] = bytes.size();
    }
    verifiedDirectory = directory.absolutePath();
    return directory.filePath(names[0]);
}
}
