#include "../DF_PlayClick_Client_Qt/BundledAdb.h"
#include "../DF_PlayClick_Client_Qt/ClientConfig.h"
#include <windows.h>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cstdio>
#include <memory>
#include <vector>

static void require(bool ok, const char* message)
{
    if (!ok) { fprintf(stderr, "FAIL %s\n", message); std::exit(1); }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    ClientConfig config;
    QString error;
    if (app.arguments().contains(QStringLiteral("--probe"))
        || app.arguments().contains(QStringLiteral("--resolve")))
    {
        const QString path = config.loadAdbPath(&error);
        if (path.isEmpty()) { fprintf(stderr, "%s\n", error.toUtf8().constData()); return 2; }
        if (app.arguments().contains(QStringLiteral("--probe")))
        {
            QProcess adb;
            adb.start(path, { QStringLiteral("version") });
            require(adb.waitForFinished(4000) && adb.exitStatus() == QProcess::NormalExit
                && adb.exitCode() == 0 && adb.readAllStandardOutput().contains("33.0.2-8557947"),
                "embedded adb version must run without installed adb or external DLLs");
        }
        printf("%s\n", path.toUtf8().constData());
        return 0;
    }
    const QStringList names{ "adb.exe", "AdbWinApi.dll", "AdbWinUsbApi.dll", "NOTICE.txt" };
    const QDir extracted(QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
        .filePath("DF_PlayClick/adb/33.0.2"));
    for (const QString& name : names) QFile::remove(extracted.filePath(name));
    QTemporaryDir portable(QDir::tempPath() + QStringLiteral("/ADB 单文件测试-XXXXXX"));
    require(portable.isValid(), "portable test directory");
    const QString executable = portable.filePath(QStringLiteral("客户端测试.exe"));
    require(QFile::copy(app.applicationFilePath(), executable), "copy only the test EXE");
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert("PATH", "");
    const auto probe = [&](const QString& argument = QStringLiteral("--probe")) {
        QProcess process;
        process.setProcessEnvironment(environment);
        process.setWorkingDirectory(portable.path());
        process.start(executable, { argument });
        require(process.waitForFinished(10000) && process.exitStatus() == QProcess::NormalExit
            && process.exitCode() == 0, process.readAllStandardError().constData());
        return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    };
    require(QDir(portable.path()).entryList(QDir::Files).size() == 1, "only one EXE distributed");
    const QString adbPath = probe();
    require(adbPath == extracted.filePath("adb.exe"), "use embedded ADB, not developer path");
    const auto verify = [&]() {
        for (const QString& name : names)
        {
            QFile packaged(":/bundled-adb/" + name), actual(extracted.filePath(name));
            require(packaged.open(QIODevice::ReadOnly) && actual.open(QIODevice::ReadOnly), "all dependencies extracted");
            require(QCryptographicHash::hash(packaged.readAll(), QCryptographicHash::Sha256)
                == QCryptographicHash::hash(actual.readAll(), QCryptographicHash::Sha256), "extracted SHA256 matches resources");
        }
    };
    verify();
    printf("PASS only EXE in Unicode directory with empty PATH; real adb version and both DLLs work\n");
    const auto lastWrite = QFileInfo(adbPath).lastModified();
    require(probe() == adbPath && QFileInfo(adbPath).lastModified() == lastWrite, "unchanged files not rewritten");
    require(QFile::remove(extracted.filePath("AdbWinUsbApi.dll")), "remove isolated test DLL");
    require(probe() == adbPath, "missing dependency repaired");
    {
        QFile dll(extracted.filePath("AdbWinApi.dll"));
        require(dll.open(QIODevice::ReadWrite) && dll.write("BAD!") == 4, "corrupt test DLL without changing size");
    }
    require(probe() == adbPath, "corrupt dependency repaired on next launch");
    verify();
    printf("PASS cache reused without rewriting; missing and corrupted DLLs restored\n");
    const QString ini = portable.filePath("DF_PlayClick_Client.ini");
    const auto configure = [&](const QString& path) {
        require(WritePrivateProfileStringW(L"ADB", L"Path", reinterpret_cast<LPCWSTR>(path.utf16()),
            reinterpret_cast<LPCWSTR>(ini.utf16())), "write isolated INI");
    };
    configure(portable.filePath("not-installed/adb.exe"));
    require(probe() == adbPath, "stale configured path falls back to embedded ADB");
    configure(executable);
    require(probe("--resolve") == executable, "custom ADB override preserved");
    configure(QString());
    printf("PASS stale config repaired by fallback; valid custom ADB configuration preserved\n");
    for (const QString& name : names) QFile::remove(extracted.filePath(name));
    std::vector<std::unique_ptr<QProcess>> processes;
    for (int i = 0; i < 4; ++i)
    {
        auto process = std::make_unique<QProcess>();
        process->setProcessEnvironment(environment);
        process->start(executable, { "--probe" });
        processes.push_back(std::move(process));
    }
    for (const auto& process : processes)
        require(process->waitForFinished(10000) && process->exitStatus() == QProcess::NormalExit
            && process->exitCode() == 0, "concurrent extraction and launch");
    verify();
    printf("PASS four simultaneous client processes extract and launch successfully\n");
    // A failed extraction must produce an actionable error instead of an empty executable path.
    require(QFile::remove(extracted.filePath("AdbWinApi.dll"))
        && QDir().mkpath(extracted.filePath("AdbWinApi.dll")), "block isolated extraction path");
    require(BundledAdb::executablePath(error).isEmpty() && !error.isEmpty(), "extraction failure reported");
    require(QDir().rmdir(extracted.filePath("AdbWinApi.dll")), "remove isolated blocking directory");
    require(!BundledAdb::executablePath(error).isEmpty() && error.isEmpty(), "retry after extraction failure");
    require(QFile::remove(extracted.filePath("AdbWinUsbApi.dll")), "remove DLL after in-process caching");
    require(!BundledAdb::executablePath(error).isEmpty(), "cached resolver repairs missing DLL");
    verify();
    printf("PASS actionable extraction errors; failed initialization and cached missing DLL can retry\n");
    return 0;
}
