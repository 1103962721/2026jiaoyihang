#include <winsock2.h>
#include <windows.h>
#include <QtWidgets>
#include <atomic>
#include <thread>
#include <mutex>
#define private public
#include "../DF_PlayClick_Client_Qt/ClientController.h"
#undef private
#include "../DF_PlayClick_Client_Qt/ClientController.cpp"
#include <cstdio>

// Isolated mock test: this executable also acts as adb, never using a real device.
int main(int argc, char** argv)
{
    if (argc > 1 && QByteArray(argv[1]) == "-s")
    {
        QCoreApplication app(argc, argv);
        const int delay = qEnvironmentVariableIntValue("MOCK_ADB_DELAY");
        QThread::msleep(delay > 0 ? delay : 350);
        if (app.arguments().contains("screencap"))
        {
            QImage image(1280, 720, QImage::Format_RGB32);
            image.fill(Qt::white);
            QByteArray png;
            QBuffer buffer(&png);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "PNG");
            DWORD written = 0;
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), png.constData(), DWORD(png.size()), &written, nullptr);
        }
        return 0;
    }
    QApplication app(argc, argv);
    QSettings config(app.applicationDirPath() + "/DF_PlayClick_Client.ini", QSettings::IniFormat);
    config.setValue("ADB/Path", app.applicationFilePath());
    config.setValue("OCR/Port", qEnvironmentVariableIntValue("MOCK_OCR_PORT"));
    config.setValue("Client/DeviceId", "isolated-adb-worker-test");
    config.sync();
    const QString ini = app.applicationDirPath() + "/DF_PlayClick_Client.ini";
    const QString fakeAdb = "P:/build/adb-worker-runtime/AdbWorkerRuntimeTests.exe";
    WritePrivateProfileStringW(L"ADB", L"Path", reinterpret_cast<LPCWSTR>(fakeAdb.utf16()), reinterpret_cast<LPCWSTR>(ini.utf16()));
    QObject owner;
    auto makeController = [&]() {
        auto* c = new ClientController(&owner);
        c->initializeExternalBossSession(GetDesktopWindow());
        c->setSessionAdbPort(59999);
        c->currentRole_ = ClientRole::Boss;
        c->boundHwnd_ = GetDesktopWindow();
        c->connection_.connected_.store(true);
        return c;
    };
    QPointer<ClientController> c = makeController();
    bool success = false;
    QObject::connect(c, &ClientController::statusChanged, &app, [&](const QString& s) {
        fprintf(stderr, "status: %s\n", s.toUtf8().constData());
        if (s == QStringLiteral("ADB 老板流程完成")) success = true;
    });
    QElapsedTimer clock;
    clock.start();
    qint64 last = 0, maxGap = 0, phaseStart = 0;
    int phase = 0, ticks = 0;
    bool stopped = false;
    auto fail = [&](const char* why) {
        fprintf(stderr, "FAIL %s\n", why);
        if (c) c->shutdownForExit();
        app.exit(1);
    };
    QTimer heartbeat;
    heartbeat.setInterval(20);
    QObject::connect(&heartbeat, &QTimer::timeout, &app, [&]() {
        const qint64 now = clock.elapsed();
        maxGap = qMax(maxGap, now - last);
        last = now; ++ticks;
        if (now > 20000) { fail("runtime timeout"); return; }
        if (phase == 0 && !c->hasAdbWorker())
        {
            if (!success || ticks < 50 || maxGap > 300) { fprintf(stderr, "ticks=%d gap=%lld\n", ticks,maxGap); fail("UI blocked or prepare failed"); return; }
            printf("PASS slow screenshot/OCR: ticks=%d max_ui_gap_ms=%lld\n", ticks, maxGap);
            qputenv("MOCK_ADB_DELAY", "10000");
            c->triggerBossPrice_ = "12345";
            c->clickEnabledTasks();
            phaseStart = now; phase = 1;
        }
        else if (phase == 1)
        {
            if (!stopped && now - phaseStart > 500)
            {
                c->stopAllActionsImmediately(false);
                c->startCountdown(0, "12345");
                c->startScheduledTrigger(0, QDateTime::currentMSecsSinceEpoch(), "12345");
                if (!c->automationCancelRequested_.load()) { fail("trigger reset cancellation"); return; }
                stopped = true;
            }
            if (stopped && !c->hasAdbWorker())
            {
                if (now - phaseStart > 1800) { fail("ADB cancellation too slow"); return; }
                printf("PASS hung ADB cancelled; new triggers preserve cancellation (%lld ms)\n", now-phaseStart);
                qunsetenv("MOCK_ADB_DELAY");
                c->shutdownForExit(); delete c.data();
                c = makeController();
                c->handlePrepareRequest(2);
                phaseStart = now; phase = 2;
            }
        }
        else if (phase == 2 && now - phaseStart > 2200)
        {
            QElapsedTimer shutdown; shutdown.start();
            c->shutdownForExit();
            if (shutdown.elapsed() > 300) { fail("session removal blocked UI"); return; }
            if (!c->hasAdbWorker()) delete c.data();
            phase = 3;
        }
        else if (phase == 3 && !c)
        {
            if (maxGap > 300) { fail("UI blocked during cancellation/removal"); return; }
            printf("PASS session deleted after OCR worker; max_ui_gap_ms=%lld\n", maxGap);
            app.exit(0);
        }
    });
    heartbeat.start();
    c->handlePrepareRequest(1);
    const int result = app.exec();
    if (c) c->shutdownForExit();
    return result;
}
