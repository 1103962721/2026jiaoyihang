#define main adbDemoApplicationMain
#include "main.cpp"
#undef main
#include <cstdio>
#include <cstdlib>
#include <QMimeData>
#include <QThread>

static void check(bool ok, const char* message)
{
    if (!ok) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
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
    WindowClientCapture::Geometry invalid;
    check(!WindowClientCapture::geometry(nullptr, invalid, error) && !error.isEmpty(),
        "invalid handle did not report failure");
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
    while (!demo.isVisible() && wait.elapsed() < 3000)
    {
        app.processEvents();
        QThread::msleep(1);
    }
    check(demo.isVisible(), "Windows screenshot did not restore demo window");
    auto* preview = static_cast<ScreenshotLabel*>(demo.findChild<QLabel*>("screenshotPreview"));
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
    std::puts("Selection checks passed");
    return 0;
}
