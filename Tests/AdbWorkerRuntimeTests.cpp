#include <winsock2.h>
#include <windows.h>
#include <QtWidgets>
#include <atomic>
#include <thread>
#include <mutex>
#define private public
#include "../DF_PlayClick_Client_Qt/ClientController.h"
#include "../DF_PlayClick_Client_Qt/MainWindow.h"
#undef private
#include "../DF_PlayClick_Client_Qt/ClientController.cpp"
#include "../DF_PlayClick_Client_Qt/MainWindow.cpp"
#include <cstdio>

// Isolated mock test: this executable also acts as adb, never using a real device.
int main(int argc, char** argv)
{
    if (argc > 1 && QByteArray(argv[1]) == "-s")
    {
        QCoreApplication app(argc, argv);
        const QString tracePath = qEnvironmentVariable("MOCK_ADB_TRACE");
        if (!tracePath.isEmpty())
        {
            QFile trace(tracePath);
            if (!trace.open(QIODevice::WriteOnly | QIODevice::Append)) return 98;
            trace.write((app.arguments().mid(3).join(' ') + '\n').toUtf8());
        }
        const QString timingPath = qEnvironmentVariable("MOCK_ADB_TIMING");
        if (!timingPath.isEmpty())
        {
            QFile timing(timingPath);
            if (!timing.open(QIODevice::WriteOnly | QIODevice::Append)) return 97;
            timing.write((QString::number(QDateTime::currentMSecsSinceEpoch()) + ' '
                + app.arguments().mid(3).join(' ') + '\n').toUtf8());
        }
        const int delay = qEnvironmentVariableIntValue("MOCK_ADB_DELAY");
        QThread::msleep(delay > 0 ? delay : 350);
        if (app.arguments().contains("screencap"))
        {
            return 99; // Local OCR must never fall back to an ADB screenshot.
        }
        return 0;
    }
    QApplication app(argc, argv);
    if (NormalizeAmountNumericToken(NormalizeAmountOcrText(QStringLiteral("哈夫币 35.247.070 可在交易行！")))
        != QStringLiteral("35247070")
        || NormalizeAmountNumericToken(NormalizeAmountOcrText(QStringLiteral("哈夫币 35.247.07 可在交易行！")))
        != QStringLiteral("3524707"))
    { fprintf(stderr,"FAIL amount normalization changed trailing zero\n"); return 1; }
    printf("PASS amount normalization preserves supplied trailing zero; never invents missing digits\n");
    {
        MainWindow ui;
        ui.bindingModeCombo_->setCurrentIndex(1);
        QWidget target; target.setWindowTitle("ordinary binding test"); target.resize(800, 500); target.show();
        const HWND hwnd = reinterpret_cast<HWND>(target.winId());
        const LONG_PTR originalStyle = GetWindowLongPtrW(hwnd, GWL_STYLE);
        const QString originalPort = ui.cloudAdbPortEdit_->text();
        int dialogs = 0;
        QTimer dismiss; dismiss.setInterval(10);
        QObject::connect(&dismiss, &QTimer::timeout, [&] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
            { ++dialogs; box->done(QMessageBox::No); }
        });
        dismiss.start();
        ui.bindDraggedCloudWindow(reinterpret_cast<quintptr>(hwnd));
        dismiss.stop(); dismiss.disconnect();
        if (dialogs != 1 || !ui.cloudSessions_.isEmpty()) return 81;
        QObject::connect(&dismiss, &QTimer::timeout, [&] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
            { ++dialogs; box->done(QMessageBox::Yes); }
            else if (auto* input = qobject_cast<QInputDialog*>(QApplication::activeModalWidget()))
            { ++dialogs; input->reject(); }
        });
        dismiss.start();
        ui.bindDraggedCloudWindow(reinterpret_cast<quintptr>(hwnd));
        dismiss.stop();
        if (dialogs != 3 || !ui.cloudSessions_.isEmpty()
            || originalPort != ui.cloudAdbPortEdit_->text()
            || originalStyle != GetWindowLongPtrW(hwnd, GWL_STYLE)
            || target.size() != QSize(800, 500)) return 82;
        printf("PASS drag confirmation No and Yes/input cancel: no session, port mutation or size lock\n");
    }
    {
        const QString keyword = QStringLiteral("一键配装");
        const QString prefix = QStringLiteral("adb_quick_loadout");
        for (const QString& sample : {keyword, QStringLiteral("键配装 公"), QStringLiteral("键 配 装")})
            if (!MatchesAdbTapText(sample,keyword,prefix))
            { fprintf(stderr,"FAIL quick loadout OCR variant rejected\n"); return 1; }
        for (const QString& sample : {QString(),QStringLiteral("配装"),QStringLiteral("键配"),QStringLiteral("装备配置")})
            if (MatchesAdbTapText(sample,keyword,prefix))
            { fprintf(stderr,"FAIL incomplete quick loadout OCR accepted\n"); return 1; }
        if (MatchesAdbTapText(QStringLiteral("键配装 公"),keyword,QStringLiteral("other_step"))
            || MatchesAdbTapText(QStringLiteral("键配装 公"),QStringLiteral("方案"),prefix)
            || !MatchesAdbTapText(QStringLiteral("仓库部门"),QStringLiteral("仓库"),QStringLiteral("adb_warehouse")))
        { fprintf(stderr,"FAIL quick loadout compatibility affected other keywords/steps\n"); return 1; }
        printf("PASS quick loadout missed leading character accepted only in its ADB step\n");
    }
    {
        const std::pair<QString, int> cases[] = {
            {QStringLiteral("上架数量日/7"), 0}, {QStringLiteral("日/7"), 0},
            {QStringLiteral("+ 上架数量日/15"), 0}, {QStringLiteral("＋ 上架数量：０／１５"), 0},
            {QStringLiteral("+日/15+"), 0}, {QStringLiteral("+ 上架数量0/15"), 0},
            {QStringLiteral("+ 上架数量11/15"), 11},
            {QStringLiteral("上架数量 0/7 +"), 0}, {QStringLiteral("上架数量：０／７"), 0},
            {QStringLiteral("上架数量1/7"), 1}, {QStringLiteral("2/7"), 2},
            {QStringLiteral("上架数量7/7"), 7}, {QStringLiteral("上架数量10/17"), 10}
        };
        for (const auto& sample : cases)
        {
            int current = -1, total = -1;
            if (!TryParseAdbListingQuantity(sample.first,current,total) || current != sample.second
                || total != (sample.second == 10 ? 17
                    : sample.first.startsWith('+') || sample.first.startsWith(QChar(0xFF0B)) ? 15 : 7))
            { fprintf(stderr,"FAIL ADB listing quantity parser\n"); return 1; }
        }
        for (const QString& sample : {QString(),QStringLiteral("上架数量日"),QStringLiteral("上架数量/7"),
            QStringLiteral("上架数量1日/7"),QStringLiteral("日期日/7"),QStringLiteral("上架数量B/7"),
            QStringLiteral("+上架数量/15"),QStringLiteral("+上架数量日/0"),QStringLiteral("+上架数量1日/15"),
            QStringLiteral("8/7"),QStringLiteral("0/0"),QStringLiteral("日/日"),
            QStringLiteral("0/99999999999999999"),QStringLiteral("-1/7")})
        {
            int current = -1, total = -1;
            if (TryParseAdbListingQuantity(sample,current,total) || current != -1 || total != -1)
            { fprintf(stderr,"FAIL invalid listing quantity accepted as zero\n"); return 1; }
        }
        printf("PASS ADB listing quantity: misread zero corrected, positive counts preserved, invalid text rejected\n");
    }
    {
        const std::pair<QString, int> cases[] = {
            {QStringLiteral("2/7"), 2}, {QStringLiteral("使用售位：2"), 2},
            {QStringLiteral("使用槽位: 2"), 2}, {QStringLiteral("使用 售 位 ： ２"), 2},
            {QStringLiteral("1/7"), 1}, {QStringLiteral("使用售位：1"), 1},
            {QStringLiteral("0/7"), 0}, {QStringLiteral("使用售位：0"), 0},
            {QStringLiteral("使用售位：6/"), 6}, {QStringLiteral("6/"), 6},
            {QStringLiteral("使用槽位：１／"), 1}, {QStringLiteral("1/"), 1}
        };
        for (const auto& sample : cases)
        {
            int current = -1;
            if (!TryParseAdbPanelCurrentValue(sample.first, current) || current != sample.second)
            { fprintf(stderr,"FAIL panel parser: %s\n",sample.first.toUtf8().constData()); return 1; }
        }
        for (const QString& sample : {QString(), QStringLiteral("使用售位"), QStringLiteral("价格：12345"),
            QStringLiteral("2"), QStringLiteral("使用售位：999999999999999999"),
            QStringLiteral("使用售位：/7"), QStringLiteral("/"), QStringLiteral("价格：6/"),
            QStringLiteral("999999999999999999/")})
        {
            int current = -1;
            if (TryParseAdbPanelCurrentValue(sample,current))
            { fprintf(stderr,"FAIL panel parser accepted invalid input\n"); return 1; }
        }
        printf("PASS ADB panel parser accepts fraction/labeled current; X=1 stays blocked, invalid OCR stays blocked\n");
    }
    QSettings config(app.applicationDirPath() + "/DF_PlayClick_Client.ini", QSettings::IniFormat);
    config.setValue("ADB/Path", app.applicationFilePath());
    config.setValue("OCR/Port", qEnvironmentVariableIntValue("MOCK_OCR_PORT"));
    config.setValue("Client/DeviceId", "isolated-adb-worker-test");
    config.sync();
    const QString ini = app.applicationDirPath() + "/DF_PlayClick_Client.ini";
    const QString fakeAdb = "P:/build/adb-worker-runtime/AdbWorkerRuntimeTests.exe";
    WritePrivateProfileStringW(L"ADB", L"Path", reinterpret_cast<LPCWSTR>(fakeAdb.utf16()), reinterpret_cast<LPCWSTR>(ini.utf16()));
    QObject owner;
    QWidget target;
    target.setWindowFlag(Qt::Tool, true);
    target.setWindowFlag(Qt::WindowStaysOnTopHint, true);
    target.setWindowTitle(QStringLiteral("Isolated local boss OCR target"));
    target.setStyleSheet(QStringLiteral("background: rgb(20,80,160)"));
    target.resize(938, 552);
    target.move(20, 20);
    target.show(); target.raise(); target.activateWindow();
    const HWND targetHwnd = reinterpret_cast<HWND>(target.winId());
    const LONG_PTR originalStyle = GetWindowLongPtrW(targetHwnd, GWL_STYLE);
    auto settle = [&]() {
        QEventLoop loop;
        QTimer::singleShot(500, &loop, &QEventLoop::quit);
        loop.exec();
    };
    // Real child HWND: resize the owning frame to obtain the selected child's client size.
    {
        QWidget first, second; first.resize(850,520); second.resize(910,560);
        first.show(); second.show(); settle();
        const HWND a = reinterpret_cast<HWND>(first.winId()), b = reinterpret_cast<HWND>(second.winId());
        ClientController one, two;
        one.initializeExternalBossSession(GetDesktopWindow());
        two.initializeExternalBossSession(GetDesktopWindow());
        if (!one.bindExternalWindow(a) || !two.bindExternalWindow(b))
        { fprintf(stderr,"FAIL bind two independent ADB controllers\n"); return 1; }
        for (int i=0;i<20;++i)
        {
            first.resize(850+i,520); second.resize(910+i,560);
            QMetaObject::invokeMethod(&one.adbWindowSizeTimer_, "timeout", Qt::DirectConnection);
            QMetaObject::invokeMethod(&two.adbWindowSizeTimer_, "timeout", Qt::DirectConnection);
            QCoreApplication::processEvents();
        }
        settle();
        AdbWindowOcr::Geometry ga,gb; QString error;
        if (!one.adbWindowSizeTimer_.isActive() || !two.adbWindowSizeTimer_.isActive()
            || !AdbWindowOcr::geometry(a,ga,error) || !AdbWindowOcr::geometry(b,gb,error)
            || ga.size != QSize(737,440) || gb.size != QSize(737,440))
        { fprintf(stderr,"FAIL second ADB binding disables size enforcement\n"); return 1; }
        two.shutdownForExit(); first.resize(900,550); settle();
        if (!one.adbWindowSizeTimer_.isActive() || !AdbWindowOcr::geometry(a,ga,error) || ga.size != QSize(737,440))
        { fprintf(stderr,"FAIL removing second ADB binding unlocks first\n"); return 1; }
        one.shutdownForExit();
        printf("PASS dual ADB controller timers, repeated resize and independent shutdown\n");
    }
    {
        QWidget frame;
        frame.resize(940,560); frame.move(20,20);
        auto* layout = new QVBoxLayout(&frame);
        layout->setContentsMargins(18,30,18,12);
        QWidget surface(&frame); layout->addWidget(&surface);
        frame.show(); settle();
        const HWND frameHwnd = reinterpret_cast<HWND>(frame.winId());
        const HWND surfaceHwnd = reinterpret_cast<HWND>(surface.winId());
        const LONG_PTR style = GetWindowLongPtrW(frameHwnd,GWL_STYLE);
        AdbWindowOcr::SizeLock lock, duplicate;
        QString error;
        if (!lock.bind(surfaceHwnd,error) || duplicate.bind(surfaceHwnd,error))
        { fprintf(stderr,"FAIL child HWND lock ownership\n"); return 1; }
        QTimer correction;
        QObject::connect(&correction,&QTimer::timeout,&frame,[&]() { lock.enforce(); });
        correction.start(100); settle();
        AdbWindowOcr::Geometry child;
        if (!AdbWindowOcr::geometry(surfaceHwnd,child,error) || child.size != QSize(737,440))
        { fprintf(stderr,"FAIL child HWND client size: %dx%d\n",child.size.width(),child.size.height()); return 1; }
        correction.stop(); lock.release();
        if ((GetWindowLongPtrW(frameHwnd,GWL_STYLE) & (WS_THICKFRAME | WS_MAXIMIZEBOX))
            != (style & (WS_THICKFRAME | WS_MAXIMIZEBOX)))
        { fprintf(stderr,"FAIL child frame resize style restoration\n"); return 1; }
        printf("PASS child HWND size lock, duplicate session rejection and frame style restoration\n");
    }
    auto makeController = [&]() {
        auto* c = new ClientController(&owner);
        c->initializeExternalBossSession(GetDesktopWindow());
        c->setSessionAdbPort(59999);
        c->currentRole_ = ClientRole::Boss;
        if (!c->bindExternalWindow(targetHwnd, QStringLiteral("synthetic target"))) std::abort();
        c->connection_.connected_.store(true);
        return c;
    };
    QPointer<ClientController> c = makeController();
    target.raise(); target.activateWindow();
    SetWindowPos(targetHwnd, HWND_TOPMOST, 0,0,0,0,SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(targetHwnd); target.repaint(); settle();
    if (qEnvironmentVariableIsSet("MOCK_DISMOUNT_FLOW") || qEnvironmentVariableIsSet("MOCK_PROMPT_FLOW"))
    {
        bool ok = false;
        QString flowError;
        SOCKET listener = socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        sockaddr_in address{}; address.sin_family=AF_INET;
        address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        if (bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address)) || listen(listener,1)) return 83;
        int addressSize=sizeof(address);
        getsockname(listener,reinterpret_cast<sockaddr*>(&address),&addressSize);
        c->connection_.socket_=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        if (::connect(c->connection_.socket_,reinterpret_cast<sockaddr*>(&address),sizeof(address))) return 84;
        SOCKET receiver=accept(listener,nullptr,nullptr); closesocket(listener);
        const DWORD receiveTimeout=1000;
        setsockopt(receiver,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&receiveTimeout),sizeof(receiveTimeout));
        QObject::connect(c, &ClientController::connectionLogAppended, &app, [](const QString& log) {
            fprintf(stderr,"%s\n",log.toUtf8().constData());
        });
        if (qEnvironmentVariableIsSet("MOCK_CLAIM_CANCEL"))
            QObject::connect(c,&ClientController::connectionLogAppended,&app,[c](const QString& log) {
                if (log.contains(QStringLiteral("将重新点击领取")))
                    QTimer::singleShot(100,c,[c]() { c->automationCancelRequested_.store(true); });
            });
        if (qEnvironmentVariableIsSet("MOCK_RETURN_CANCEL"))
            QObject::connect(c,&ClientController::connectionLogAppended,&app,[c](const QString& log) {
                if (log.contains(QStringLiteral("检查交易行")))
                    c->automationCancelRequested_.store(true);
            });
        QTimer visibility;
        QObject::connect(&visibility, &QTimer::timeout, &target, [&]() {
            if (IsIconic(targetHwnd))
            {
                fprintf(stderr,"TEST restoring minimized synthetic OCR target\n");
                target.showNormal();
            }
            AdbWindowOcr::Geometry geometry;
            QString geometryError;
            const QRect desktop(GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
            if (AdbWindowOcr::geometry(targetHwnd, geometry, geometryError)
                && !desktop.contains(QRect(geometry.origin, geometry.size)))
                target.move(desktop.topLeft() + QPoint(30, 30));
        });
        visibility.start(50);
        QTimer::singleShot(110000,&app,[c]() { if(c) c->automationCancelRequested_.store(true); });
        QThread* worker = QThread::create([&]() {
            ok = c->executeAdbBossListingSequence(QStringLiteral("100"),flowError);
            QMetaObject::invokeMethod(&app,&QCoreApplication::quit,Qt::QueuedConnection);
        });
        worker->start();
        app.exec(); worker->wait(); delete worker;
        QCoreApplication::processEvents();
        QByteArray reports;
        char buffer[512];
        for (int count; (count=recv(receiver,buffer,sizeof(buffer),0))>0;) reports.append(buffer,count);
        closesocket(receiver);
        c->shutdownForExit();
        if (qEnvironmentVariableIsSet("MOCK_CLAIM_CANCEL") || qEnvironmentVariableIsSet("MOCK_RETURN_CANCEL"))
        {
            if (ok || !flowError.contains(QStringLiteral("强制停止"))) return 85;
            printf("PASS claim retry cancellation stops before another tap\n");
            return 0;
        }
        if (qEnvironmentVariableIsSet("MOCK_PROMPT_FAILURE"))
        {
            if (ok || !reports.isEmpty() || !flowError.contains(qEnvironmentVariable("MOCK_PROMPT_FAILURE")))
                return 87;
            printf("PASS failed prompt navigation stops without funds/ready reports\n");
            return 0;
        }
        if (!ok) { fprintf(stderr,"FAIL dismount flow: %s\n",flowError.toUtf8().constData()); return 1; }
        const QByteArray expectedAmount=qEnvironmentVariableIsSet("MOCK_PROMPT_FLOW") ? "35247070" : "10543903";
        if (!reports.contains("AMOUNT_REPORT " + expectedAmount + "\n")
            || !reports.contains("TEAM_READY 1 1 1\n"))
        { fprintf(stderr,"FAIL amount/ready protocol reports: %s\n",reports.constData()); return 86; }
        printf("PASS actual ADB boss dismount/relist sequence completed\n");
        return 0;
    }
    QString captureError;
    AdbWindowOcr::Geometry g;
    QRect actual;
    const auto initial = AdbWindowOcr::capture(targetHwnd, QRect(495,64,192,72), g, actual, captureError);
    if (g.size != QSize(737,440) || initial.size() != QSize(192,72)
        || actual != QRect(495,64,192,72)
        || initial.pixelColor(55,29) != QColor(20,80,160)
        || (GetWindowLongPtrW(targetHwnd, GWL_STYLE) & (WS_THICKFRAME | WS_MAXIMIZEBOX)))
    { initial.save(QStringLiteral("capture-failure.png")); fprintf(stderr,"FAIL fixed client capture: %s (%dx%d) pixel=%08x style=%llx\n", captureError.toUtf8().constData(),g.size.width(),g.size.height(),initial.isNull()?0:initial.pixel(55,29),GetWindowLongPtrW(targetHwnd,GWL_STYLE)); return 1; }
    const QPoint oldOrigin = g.origin;
    target.move(120,120); target.resize(880,510); settle();
    const auto moved = AdbWindowOcr::capture(targetHwnd, QRect(), g, actual, captureError);
    if (g.size != QSize(737,440) || moved.size() != g.size || oldOrigin == g.origin)
    { fprintf(stderr,"FAIL moved/resized client not corrected\n"); return 1; }
    if (!AdbWindowOcr::capture(nullptr, QRect(), g, actual, captureError).isNull()
        || !AdbWindowOcr::capture(targetHwnd, QRect(730,430,20,20), g, actual, captureError).isNull())
    { fprintf(stderr,"FAIL invalid capture accepted\n"); return 1; }
    target.showMinimized(); settle();
    if (!AdbWindowOcr::capture(targetHwnd, QRect(), g, actual, captureError).isNull())
    { fprintf(stderr,"FAIL minimized target captured\n"); return 1; }
    target.showNormal(); target.raise(); settle();
    QString text;
    if (!c->adbReadText(70,70,103,29,QStringLiteral("quantity_enlarge_test"),text,captureError,3))
    { fprintf(stderr,"FAIL X/N local enlargement: %s\n",captureError.toUtf8().constData()); return 1; }
    // Normal PC binding keeps its original size and style; no external size lock.
    {
        QWidget pc;
        pc.resize(910,510); pc.show(); settle();
        const HWND pcHwnd = reinterpret_cast<HWND>(pc.winId());
        AdbWindowOcr::Geometry before, after;
        AdbWindowOcr::geometry(pcHwnd,before,captureError);
        const LONG_PTR style = GetWindowLongPtrW(pcHwnd,GWL_STYLE);
        ClientController normal;
        TargetWindowInfo item; item.hwnd=pcHwnd;
        normal.windows_.push_back(item);
        normal.bindWindow(0,false);
        AdbWindowOcr::geometry(pcHwnd,after,captureError);
        if (after.size != before.size || GetWindowLongPtrW(pcHwnd,GWL_STYLE) != style)
        { fprintf(stderr,"FAIL normal PC binding changed\n"); return 1; }
        normal.shutdownForExit();
    }
    target.raise(); target.activateWindow(); settle();
    printf("PASS local crop/full capture, move, resize lock, invalid/minimized target, PC binding\n");
    bool success = false;
    bool screenshotTimingVerified = false;
    QObject::connect(c, &ClientController::connectionLogAppended, &app, [&](const QString& message) {
        const auto match = QRegularExpression(QStringLiteral(
            R"(桌面截图 (\d+)ms，保存/放大 (\d+)ms，OCR (\d+)ms，本次总计 (\d+)ms)"))
            .match(message);
        if (match.hasMatch())
        {
            const qint64 captureMs = match.captured(1).toLongLong();
            const qint64 processingMs = match.captured(2).toLongLong();
            const qint64 ocrMs = match.captured(3).toLongLong();
            const qint64 totalMs = match.captured(4).toLongLong();
            if (ocrMs >= 900 && totalMs >= captureMs + processingMs + ocrMs)
                screenshotTimingVerified = true;
        }
    });
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
            if (!screenshotTimingVerified) { fail("OCR timing omitted screenshot command or processing"); return; }
            printf("PASS OCR timing includes local capture, processing and slow OCR\n");
            printf("PASS local screenshot/slow OCR: ticks=%d max_ui_gap_ms=%lld\n", ticks, maxGap);
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
                phaseStart = now; phase = 2;
                c = makeController();
                QTimer::singleShot(500, c, [c]() { if (c) c->handlePrepareRequest(2); });
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
    if ((GetWindowLongPtrW(targetHwnd,GWL_STYLE) & (WS_THICKFRAME | WS_MAXIMIZEBOX))
        != (originalStyle & (WS_THICKFRAME | WS_MAXIMIZEBOX)))
    { fprintf(stderr,"FAIL window resize style not restored on shutdown\n"); return 1; }
    return result;
}
