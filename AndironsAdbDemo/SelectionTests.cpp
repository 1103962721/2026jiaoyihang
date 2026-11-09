#define main adbDemoApplicationMain
#include "main.cpp"
#undef main
#include <cstdio>
#include <cstdlib>
#include <QMimeData>
#include <QThread>
#include <QBuffer>
#include <QTemporaryDir>
#include <QProcessEnvironment>
#include <QFile>
#include <QStandardPaths>
#include <fcntl.h>
#include <io.h>

static void check(bool ok, const char* message)
{
    if (!ok) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    if (argc > 1)
    {
        const auto args = app.arguments();
        if (args.contains("--probe-bundled"))
        {
            QStandardPaths::setTestModeEnabled(true);
            QString error; const auto path = BundledAdb::executablePath(error);
            QProcess process; process.start(path, {"version"});
            check(!path.isEmpty() && process.waitForFinished(5000) && process.exitCode() == 0
                && process.readAllStandardOutput().contains("33.0.2"), "portable embedded ADB failed");
            return 0;
        }
        QThread::msleep(250);
        if (args.contains("devices")) { puts("List of devices attached\nUSB123 device usb:1-2 model:Test\nUSB456 unauthorized\n127.0.0.1:5555 device\nemulator-5554 device"); return 0; }
        if (args.contains("get-state")) { puts("device"); return 0; }
        if (args.contains("screencap"))
        {
            QImage image(1280,720,QImage::Format_RGB32); image.fill(Qt::green);
            QByteArray png; QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly); image.save(&buffer,"PNG");
            _setmode(_fileno(stdout), _O_BINARY); fwrite(png.constData(),1,png.size(),stdout); return 0;
        }
        return 0;
    }
    const auto waitUntil = [&](const std::function<bool()>& condition, int timeout = 5000) {
        QElapsedTimer timer; timer.start();
        while (!condition() && timer.elapsed() < timeout) { app.processEvents(); QThread::msleep(5); }
        return condition();
    };
    QTemporaryDir portable; check(portable.isValid(), "portable directory failed");
    const auto copied = portable.filePath("PortableDemoTest.exe");
    check(QFile::copy(app.applicationFilePath(),copied), "copy only test exe failed");
    QProcess probe; auto environment = QProcessEnvironment::systemEnvironment(); environment.insert("PATH", "");
    probe.setProcessEnvironment(environment); probe.setWorkingDirectory(portable.path());
    probe.start(copied, {"--probe-bundled"});
    check(probe.waitForFinished(15000) && probe.exitCode()==0, "embedded ADB failed without installed ADB or DLLs");
    puts("PASS portable embedded ADB with empty PATH");
    ScreenshotLabel label;
    label.resize(642, 482);
    label.show();
    QImage image(1280, 720, QImage::Format_RGB32);
    image.fill(Qt::white);
    label.setImage(image);
    app.processEvents();
    QRect selected;
    int clicks = 0;
    label.selected = [&](QRect r) { selected = r; };
    label.clicked = [&](QPoint) { ++clicks; };
    auto send = [&](QEvent::Type type, QPoint p) {
        QMouseEvent event(type, QPointF(p), QPointF(p),
            type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&label, &event);
    };
    label.setSelectionMode(true);
    // Centered 640x360 preview: corners (1,61) and (640,420).
    send(QEvent::MouseButtonPress, {1,61});
    send(QEvent::MouseMove, {700,500});
    send(QEvent::MouseButtonRelease, {700,500});
    check(selected == QRect(0,0,1280,720), "full image / clamping failed");
    check(clicks == 0, "selection triggered click");
    send(QEvent::MouseButtonPress, {640,420});
    send(QEvent::MouseButtonRelease, {1,61});
    check(selected == QRect(0,0,1280,720), "reverse selection failed");
    label.clearSelection();
    send(QEvent::MouseButtonPress, {10,10});
    send(QEvent::MouseButtonRelease, {100,100});
    check(selected.isEmpty(), "letterbox started selection");
    label.resize(1282, 722);
    app.processEvents();
    send(QEvent::MouseButtonPress, {101,51});
    send(QEvent::MouseButtonRelease, {200,100});
    check(selected == QRect(100,50,100,50), "native pixel selection failed");
    label.resize(642,482);
    app.processEvents();
    check(selected == QRect(100,50,100,50), "resize changed device region");
    label.setImage(image, true);
    check(selected == QRect(100,50,100,50), "live frame cleared selected region");
    label.setImage(image);
    check(selected.isEmpty(), "new screenshot retained stale region");
    label.setSelectionMode(false);
    send(QEvent::MouseButtonPress, {1,61});
    send(QEvent::MouseButtonRelease, {1,61});
    check(clicks == 1, "coordinate picking broken");
    // The same preview selection must use native client pixels, not phone pixels.
    QImage clientImage(704, 430, QImage::Format_RGB32);
    clientImage.fill(Qt::blue);
    label.setImage(clientImage);
    label.resize(706, 432);
    app.processEvents();
    label.setSelectionMode(true);
    send(QEvent::MouseButtonPress, {1,1});
    send(QEvent::MouseButtonRelease, {704,430});
    check(selected == QRect(0,0,704,430), "native Windows full-client selection failed");
    send(QEvent::MouseButtonPress, {101,51});
    send(QEvent::MouseButtonRelease, {200,100});
    check(selected == QRect(100,50,100,50), "Windows client region uses wrong resolution");
    label.setSelectionMode(false);
    QPoint beforeZoom, afterZoom, afterPan;
    label.clicked = [&](QPoint point) { beforeZoom = point; };
    send(QEvent::MouseButtonPress, {351,216});
    QWheelEvent zoomEvent(QPointF(351,216), QPointF(351,216), QPoint(), QPoint(0,120),
        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&label, &zoomEvent);
    check(label.zoom() > 1.0, "wheel did not zoom preview");
    label.clicked = [&](QPoint point) { afterZoom = point; };
    send(QEvent::MouseButtonPress, {351,216});
    check(beforeZoom == afterZoom, "wheel zoom shifted cursor image coordinate");
    QMouseEvent panDown(QEvent::MouseButtonPress, QPointF(351,216), QPointF(351,216),
        Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
    QApplication::sendEvent(&label, &panDown);
    QMouseEvent panMove(QEvent::MouseMove, QPointF(411,216), QPointF(411,216),
        Qt::NoButton, Qt::MiddleButton, Qt::NoModifier);
    QApplication::sendEvent(&label, &panMove);
    QMouseEvent panUp(QEvent::MouseButtonRelease, QPointF(411,216), QPointF(411,216),
        Qt::MiddleButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&label, &panUp);
    label.clicked = [&](QPoint point) { afterPan = point; };
    send(QEvent::MouseButtonPress, {351,216});
    check(afterPan.x() < afterZoom.x(), "middle drag did not pan zoomed image");
    label.resetZoom();
    check(label.zoom() == 1.0, "fit preview did not reset zoom");
    label.setImage({});
    check(label.pixmap().isNull(), "clearing screenshot left stale preview");

    QWidget target;
    target.setWindowTitle(QStringLiteral("Andirons synthetic client capture"));
    target.setStyleSheet(QStringLiteral("background: rgb(20,80,160)"));
    target.resize(704,430);
    target.move(20,20);
    target.show();
    target.raise();
    target.activateWindow();
    SetWindowPos(reinterpret_cast<HWND>(target.winId()), HWND_TOPMOST, 0,0,0,0, SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    app.processEvents();
    QEventLoop painted;
    QTimer::singleShot(250, &painted, &QEventLoop::quit);
    painted.exec();
    const HWND hwnd = reinterpret_cast<HWND>(target.winId());
    WindowClientCapture::Geometry geometry;
    QString error;
    check(WindowClientCapture::geometry(hwnd, geometry, error), "window geometry failed");
    const QImage capture = WindowClientCapture::capture(hwnd, geometry, error);
    check(!capture.isNull() && capture.size() == geometry.size, "capture dimensions do not match physical client");
    std::printf("client=%dx%d origin=%d,%d center=%08x\n", geometry.size.width(), geometry.size.height(),
        geometry.screenOrigin.x(), geometry.screenOrigin.y(), capture.pixel(capture.width()/2, capture.height()/2));
    check(capture.pixelColor(capture.width()/2, capture.height()/2) == QColor(20,80,160),
        "capture did not use client-area desktop origin");
    WindowGraphicsCapture graphics;
    QString graphicsError;
    const bool graphicsStarted = graphics.start(hwnd, graphicsError);
    if (!graphicsStarted) std::fprintf(stderr, "Windows Graphics Capture: %s\n", graphicsError.toUtf8().constData());
    check(graphicsStarted, "Windows Graphics Capture did not start");
    QWidget cover;
    cover.setStyleSheet(QStringLiteral("background: rgb(200,20,40)"));
    cover.setGeometry(geometry.screenOrigin.x(), geometry.screenOrigin.y(),
        geometry.size.width(), geometry.size.height());
    cover.show();
    cover.raise();
    SetWindowPos(reinterpret_cast<HWND>(cover.winId()), HWND_TOPMOST, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    app.processEvents();
    QThread::msleep(100);
    const QImage coveredDesktop = WindowClientCapture::capture(hwnd, geometry, graphicsError);
    check(!coveredDesktop.isNull() && coveredDesktop.pixelColor(coveredDesktop.width()/2,
        coveredDesktop.height()/2) != QColor(20,80,160), "test cover did not occlude client");
    QImage windowFrame;
    check(waitUntil([&] { return graphics.latest(windowFrame); }, 3000),
        "Windows Graphics Capture produced no frame");
    const QImage uncovered = WindowClientCapture::cropCapturedWindow(hwnd, windowFrame, geometry, graphicsError);
    check(!uncovered.isNull() && uncovered.size() == geometry.size
        && uncovered.pixelColor(uncovered.width()/2, uncovered.height()/2) == QColor(20,80,160),
        "occluded Windows client preview was not preserved");
    graphics.stop();
    cover.hide();
    QWidget inputTarget;
    inputTarget.resize(140, 80);
    inputTarget.show();
    app.processEvents();
    QString inputError;
    const HWND inputHwnd = reinterpret_cast<HWND>(inputTarget.winId());
    check(WindowBackgroundInput::click(inputHwnd, {20, 20}, WindowBackgroundInput::Mode::Post, inputError),
        "background PostMessage click failed");
    check(WindowBackgroundInput::key(inputHwnd, VK_RETURN, WindowBackgroundInput::Mode::SendTimeout, inputError),
        "background SendMessage key failed");
    check(WindowBackgroundInput::text(inputHwnd, QStringLiteral("test"), WindowBackgroundInput::Mode::Post, inputError),
        "background PostMessage text failed");
    puts("PASS occluded client capture and background message dispatch");
    WindowClientCapture::Geometry invalid;
    check(!WindowClientCapture::geometry(nullptr, invalid, error) && !error.isEmpty(),
        "invalid handle did not report failure");
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0,0,0,0, SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    AdbDemo demo;
    demo.show();
    app.processEvents();
    auto* source = demo.findChild<QComboBox*>("screenshotSource");
    source->setCurrentIndex(1);
    auto* handle = demo.findChild<QLineEdit*>("windowHandle");
    handle->setText(WindowClientCapture::handleText(hwnd));
    demo.findChild<QPushButton*>("bindWindowHandle")->click();
    demo.findChild<QPushButton*>("takeScreenshot")->click();
    QElapsedTimer wait;
    wait.start();
    auto* preview = static_cast<ScreenshotLabel*>(demo.findChild<QLabel*>("screenshotPreview"));
    while ((preview->pixmap().isNull() || !demo.isVisible()) && wait.elapsed() < 3000)
    {
        app.processEvents();
        QThread::msleep(1);
    }
    check(demo.isVisible(), "Windows screenshot did not restore demo window");
    check(!preview->pixmap().isNull(), "Windows preview capture failed");
    preview->clicked(QPoint(100,50));
    check(demo.findChild<QSpinBox*>("pcClientX")->value() == 100, "PC point was not picked");
    check(demo.findChild<QSpinBox*>("adbX")->value() == 0, "PC point changed ADB coordinates");
    preview->selected(QRect(10,20,50,60));
    auto* previousClipboard = new QMimeData;
    if (const auto* current = QApplication::clipboard()->mimeData())
        for (const QString& format : current->formats())
            previousClipboard->setData(format, current->data(format));
    demo.findChild<QPushButton*>("copySnapshotDetails")->click();
    const auto details = QApplication::clipboard()->text();
    QApplication::clipboard()->setMimeData(previousClipboard);
    check(details.contains(WindowClientCapture::handleText(hwnd))
        && details.contains(QStringLiteral("OCR 区域：(10,20,50,60)"))
        && details.contains(QStringLiteral("客户区大小：%1×%2").arg(geometry.size.width()).arg(geometry.size.height())),
        "copied report omitted handle, client size or region");
    source->setCurrentIndex(0);
    check(preview->pixmap().isNull(), "source switch retained Windows pixels in ADB mode");
    source->setCurrentIndex(1);
    handle->setText(QStringLiteral("0xDEADBEEF"));
    demo.findChild<QPushButton*>("bindWindowHandle")->click();
    demo.findChild<QPushButton*>("takeScreenshot")->click();
    check(preview->pixmap().isNull(), "invalid handle retained or captured stale image");
    const auto parsed = parseDemoDevices("List of devices attached\nUSB123 device usb:1-2\nUSB456 unauthorized\n127.0.0.1:5555 device\nemulator-5554 device\n");
    check(parsed.size()==4 && parsed[0].usb && parsed[1].usb && !parsed[2].usb && !parsed[3].usb, "USB list parsing failed");
    source->setCurrentIndex(0);
    demo.findChild<QLineEdit*>("adbPath")->setText(app.applicationFilePath());
    demo.findChild<QComboBox*>("adbTransport")->setCurrentIndex(1);
    auto* devices = demo.findChild<QComboBox*>("usbDevices");
    check(waitUntil([&] { return devices->count()==2; }), "async USB enumeration failed");
    auto* live = demo.findChild<QCheckBox*>("livePreview");
    int ticks=0; QTimer heartbeat; heartbeat.setInterval(10);
    QObject::connect(&heartbeat,&QTimer::timeout,[&] { ++ticks; }); heartbeat.start();
    live->setChecked(true);
    check(waitUntil([&] { return !preview->pixmap().isNull(); }), "USB live preview failed");
    check(ticks > 5, "ADB blocked UI event loop");
    preview->clicked({1279,719});
    check(demo.findChild<QSpinBox*>("adbX")->value()==1279, "USB point mapping failed");
    live->setChecked(false);
    devices->setCurrentIndex(1); live->setChecked(true);
    check(!live->isChecked() && preview->pixmap().isNull(), "unauthorized USB allowed capture");
    devices->setCurrentIndex(0); live->setChecked(true);
    source->setCurrentIndex(2);
    QElapsedTimer cancelWait; cancelWait.start();
    waitUntil([&] { return cancelWait.elapsed() > 500; });
    check(preview->pixmap().isNull() && !live->isChecked(), "old ADB frame appeared after source switch");
    demo.findChild<QPushButton*>("takeScreenshot")->click();
    check(waitUntil([&] { return demo.isVisible() && !preview->pixmap().isNull(); }), "desktop capture failed");
    { WindowClientCapture::DpiScope dpi; preview->clicked({0,0});
      demo.findChild<QPushButton*>("copySnapshotDetails")->click();
      check(QApplication::clipboard()->text().contains(QStringLiteral("坐标：(%1,%2)").arg(GetSystemMetrics(SM_XVIRTUALSCREEN)).arg(GetSystemMetrics(SM_YVIRTUALSCREEN))), "desktop origin conversion failed"); }
    puts("PASS USB authorization, asynchronous live preview, source-switch cancellation and desktop coordinates");
    std::puts("Selection checks passed");
    return 0;
}
