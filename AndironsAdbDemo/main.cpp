#include <QApplication>
#include <QByteArray>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QElapsedTimer>
#include <QPainter>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPixmap>
#include <QProcess>
#include <QPushButton>
#include <QSpinBox>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QFutureWatcher>
#include <QtConcurrent>
#include "AsyncAdb.h"
#include "../DF_PlayClick_Client_Qt/BundledAdb.h"

#include <functional>
#include <limits>

#include "WindowClientCapture.h"

namespace
{
QString encodeAdbText(QString text)
{
    text.replace(QStringLiteral("%"), QStringLiteral("%25"));
    text.replace(QStringLiteral(" "), QStringLiteral("%s"));
    text.replace(QStringLiteral("&"), QStringLiteral("\\&"));
    text.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    text.replace(QStringLiteral(";"), QStringLiteral("\\;"));
    text.replace(QStringLiteral("("), QStringLiteral("\\("));
    text.replace(QStringLiteral(")"), QStringLiteral("\\)"));
    text.replace(QStringLiteral("<"), QStringLiteral("\\<"));
    text.replace(QStringLiteral(">"), QStringLiteral("\\>"));
    text.replace(QStringLiteral("|"), QStringLiteral("\\|"));
    return text;
}

class ScreenshotLabel final : public QLabel
{
public:
    explicit ScreenshotLabel(QWidget* parent = nullptr) : QLabel(parent)
    {
        setAlignment(Qt::AlignCenter);
        setMinimumSize(360, 300);
        setFrameShape(QFrame::Box);
        setText(QStringLiteral("No screenshot"));
        setMouseTracking(true);
    }

    void setImage(const QImage& image, bool preserveSelection = false)
    {
        if (dragging_ && preserveSelection) return;
        const bool sameSize = image_.size() == image.size();
        image_ = image;
        if (!preserveSelection || !sameSize) clearSelection();
        if (image_.isNull()) { clear(); setText(QStringLiteral("No screenshot")); }
        else refreshPixmap();
    }

    void setSelectionMode(bool enabled)
    {
        selectionMode_ = enabled;
        dragging_ = false;
        setCursor(enabled ? Qt::CrossCursor : Qt::ArrowCursor);
    }

    void clearSelection()
    {
        dragging_ = false;
        selection_ = {};
        if (selected) selected(selection_);
        update();
    }

    std::function<void(QPoint)> clicked;
    std::function<void(QPoint)> moved;
    std::function<void(QRect)> selected;

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QLabel::resizeEvent(event);
        refreshPixmap();
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        const auto point = imagePoint(event->position().toPoint());
        if (point.x() >= 0 && moved) moved(point);
        if (dragging_) updateSelection(event->position().toPoint());
        QLabel::mouseMoveEvent(event);
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton) {
            const auto point = imagePoint(event->position().toPoint());
            if (point.x() >= 0) {
                if (selectionMode_) {
                    anchor_ = point;
                    dragging_ = true;
                    updateSelection(event->position().toPoint());
                } else if (clicked) clicked(point);
            }
        }
        QLabel::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (event->button() == Qt::LeftButton && dragging_) {
            updateSelection(event->position().toPoint());
            dragging_ = false;
        }
        QLabel::mouseReleaseEvent(event);
    }

    void paintEvent(QPaintEvent* event) override
    {
        QLabel::paintEvent(event);
        if (selection_.isEmpty() || image_.isNull()) return;
        const QRect display = displayRect();
        const double sx = double(display.width()) / image_.width();
        const double sy = double(display.height()) / image_.height();
        const QRectF overlay(display.x() + selection_.x() * sx,
            display.y() + selection_.y() * sy, selection_.width() * sx, selection_.height() * sy);
        QPainter painter(this);
        painter.setPen(QPen(Qt::red, 2));
        painter.setBrush(QColor(255, 0, 0, 45));
        painter.drawRect(overlay);
    }

private:
    QRect displayRect() const
    {
        const QSize displayed = pixmap().size();
        const QRect content = contentsRect();
        return QRect(content.x() + (content.width() - displayed.width()) / 2,
            content.y() + (content.height() - displayed.height()) / 2,
            displayed.width(), displayed.height());
    }

    QPoint imagePoint(const QPoint& widgetPoint, bool clamp = false) const
    {
        if (image_.isNull()) return {-1, -1};
        const QRect display = displayRect();
        if (display.isEmpty() || (!clamp && !display.contains(widgetPoint))) return {-1, -1};
        const int localX = qBound(0, widgetPoint.x() - display.x(), display.width() - 1);
        const int localY = qBound(0, widgetPoint.y() - display.y(), display.height() - 1);
        // 将预览图两端精确映射到原始截图首末像素，支持完整框选。
        return QPoint(display.width() > 1 ? qRound(double(localX) * (image_.width() - 1) / (display.width() - 1)) : 0,
            display.height() > 1 ? qRound(double(localY) * (image_.height() - 1) / (display.height() - 1)) : 0);
    }

    void updateSelection(const QPoint& widgetPoint)
    {
        const QPoint end = imagePoint(widgetPoint, true);
        if (end.x() < 0) return;
        selection_ = QRect(QPoint(qMin(anchor_.x(), end.x()), qMin(anchor_.y(), end.y())),
            QPoint(qMax(anchor_.x(), end.x()), qMax(anchor_.y(), end.y())));
        if (selected) selected(selection_);
        update();
    }

    void refreshPixmap()
    {
        if (image_.isNull()) return;
        QImage preview = image_;
        preview.setDevicePixelRatio(1);
        setPixmap(QPixmap::fromImage(preview).scaled(contentsRect().size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    }

    QImage image_;
    QRect selection_;
    QPoint anchor_;
    bool selectionMode_ = false;
    bool dragging_ = false;
};
}

class AdbDemo final : public QWidget
{
public:
    AdbDemo()
    {
        setWindowTitle(QStringLiteral("Andirons ADB / Windows 坐标工具"));
        resize(900, 940);
        auto* root = new QVBoxLayout(this);
        auto* sourceRow = new QHBoxLayout;
        source_ = new QComboBox(this);
        source_->setObjectName(QStringLiteral("screenshotSource"));
        source_->addItems({QStringLiteral("ADB 设备截图 / 手机坐标"), QStringLiteral("Windows 客户区截图 / 客户区坐标"), QStringLiteral("整个桌面 / 桌面坐标")});
        sourceRow->addWidget(new QLabel(QStringLiteral("截图来源"), this));
        sourceRow->addWidget(source_, 1);
        root->addLayout(sourceRow);

        windowPanel_ = new QWidget(this);
        auto* windowForm = new QFormLayout(windowPanel_);
        windowForm->setContentsMargins(0, 0, 0, 0);
        auto* windowRow = new QHBoxLayout;
        windowList_ = new QComboBox(this);
        windowList_->setObjectName(QStringLiteral("windowList"));
        windowList_->setMinimumContentsLength(28);
        windowList_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        auto* refreshWindows = new QPushButton(QStringLiteral("刷新窗口"), this);
        windowRow->addWidget(windowList_, 1);
        windowRow->addWidget(refreshWindows);
        windowForm->addRow(QStringLiteral("Windows 窗口"), windowRow);
        auto* handleRow = new QHBoxLayout;
        hwndEdit_ = new QLineEdit(this);
        hwndEdit_->setObjectName(QStringLiteral("windowHandle"));
        hwndEdit_->setPlaceholderText(QStringLiteral("窗口句柄：0x十六进制或十进制，也支持子窗口句柄"));
        auto* bindHandle = new QPushButton(QStringLiteral("读取句柄"), this);
        bindHandle->setObjectName(QStringLiteral("bindWindowHandle"));
        handleRow->addWidget(hwndEdit_, 1);
        handleRow->addWidget(bindHandle);
        windowForm->addRow(QStringLiteral("HWND"), handleRow);
        geometryLabel_ = new QLabel(QStringLiteral("请选择目标窗口；客户区原点为 (0,0)，不含标题栏和边框"), this);
        geometryLabel_->setWordWrap(true);
        windowForm->addRow(geometryLabel_);
        root->addWidget(windowPanel_);
        windowPanel_->hide();

        auto* connection = new QFormLayout;
        adbPath_ = new QLineEdit(QStringLiteral("adb.exe"), this);
        serial_ = new QLineEdit(QStringLiteral("127.0.0.1:63651"), this);
        auto* browse = new QPushButton(QStringLiteral("Browse"), this);
        auto* adbRow = new QHBoxLayout;
        adbRow->addWidget(adbPath_, 1);
        adbRow->addWidget(browse);
        connection->addRow(QStringLiteral("ADB path"), adbRow);
        connection->addRow(QStringLiteral("Device"), serial_);
        adbPath_->setObjectName(QStringLiteral("adbPath"));
        serial_->setObjectName(QStringLiteral("deviceSerial"));
        adbPath_->setPlaceholderText(QStringLiteral("留空使用内置 ADB"));
        adbPath_->clear();
        transport_ = new QComboBox(this);
        transport_->setObjectName(QStringLiteral("adbTransport"));
        transport_->addItems({QStringLiteral("网络 ADB"), QStringLiteral("USB 直连")});
        devices_ = new QComboBox(this);
        devices_->setObjectName(QStringLiteral("usbDevices"));
        auto* refreshDevices = new QPushButton(QStringLiteral("刷新设备"), this);
        refreshDevices->setObjectName(QStringLiteral("refreshDevices"));
        auto* deviceRow = new QHBoxLayout;
        deviceRow->addWidget(transport_); deviceRow->addWidget(devices_, 1); deviceRow->addWidget(refreshDevices);
        connection->addRow(QStringLiteral("连接方式"), deviceRow);
        root->addLayout(connection);

        auto* connectButton = new QPushButton(QStringLiteral("Connect"), this);
        auto* screenshotButton = new QPushButton(QStringLiteral("Screenshot"), this);
        screenshotButton->setObjectName(QStringLiteral("takeScreenshot"));
        auto* connectionRow = new QHBoxLayout;
        connectionRow->addWidget(connectButton);
        connectionRow->addWidget(screenshotButton);
        live_ = new QCheckBox(QStringLiteral("实时预览"), this);
        live_->setObjectName(QStringLiteral("livePreview"));
        interval_ = new QSpinBox(this); interval_->setRange(100, 5000); interval_->setValue(300);
        interval_->setSuffix(QStringLiteral(" ms"));
        connectionRow->addWidget(live_); connectionRow->addWidget(interval_);
        root->addLayout(connectionRow);

        screenshot_ = new ScreenshotLabel(this);
        screenshot_->setObjectName(QStringLiteral("screenshotPreview"));
        root->addWidget(screenshot_, 1);
        coordinate_ = new QLabel(QStringLiteral("Move or click the screenshot to get device coordinates"), this);
        root->addWidget(coordinate_);
        auto* resolution = new QLabel(QStringLiteral("设备截图分辨率：等待截图"), this);
        resolution_ = resolution;
        root->addWidget(resolution);
        auto* coordinateRow = new QHBoxLayout;
        auto* copyPoint = new QPushButton(QStringLiteral("复制坐标"), this);
        auto* copyDetails = new QPushButton(QStringLiteral("复制截图信息和选区"), this);
        copyDetails->setObjectName(QStringLiteral("copySnapshotDetails"));
        auto* saveImage = new QPushButton(QStringLiteral("保存原始截图"), this);
        coordinateRow->addWidget(copyPoint);
        coordinateRow->addWidget(copyDetails);
        coordinateRow->addWidget(saveImage);
        root->addLayout(coordinateRow);
        auto* selectionMode = new QCheckBox(QStringLiteral("框选 OCR 区域（拖动鼠标，不发送点击）"), this);
        auto* copyRegion = new QPushButton(QStringLiteral("复制区域"), this);
        auto* clearRegion = new QPushButton(QStringLiteral("清除框选"), this);
        auto* selectionRow = new QHBoxLayout;
        selectionRow->addWidget(selectionMode, 1);
        selectionRow->addWidget(copyRegion);
        selectionRow->addWidget(clearRegion);
        root->addLayout(selectionRow);
        auto* regionText = new QLineEdit(this);
        regionText->setReadOnly(true);
        regionText->setPlaceholderText(QStringLiteral("当前来源的原始像素区域：(x,y,宽,高)，包含起点和终点"));
        root->addWidget(regionText);
        copyRegion->setEnabled(false);
        screenshot_->selected = [this, regionText, copyRegion](const QRect& region) {
            selectedRegion_ = source_->currentIndex() == 2 && !region.isEmpty() ? region.translated(desktopOrigin_) : region;
            regionText->setText(region.isEmpty() ? QString()
                : QStringLiteral("(%1,%2,%3,%4)").arg(selectedRegion_.x()).arg(selectedRegion_.y()).arg(region.width()).arg(region.height()));
            copyRegion->setEnabled(!region.isEmpty());
        };
        connect(selectionMode, &QCheckBox::toggled, this, [this](bool enabled) { screenshot_->setSelectionMode(enabled); });
        connect(clearRegion, &QPushButton::clicked, this, [this]() { screenshot_->clearSelection(); });
        connect(copyRegion, &QPushButton::clicked, this, [regionText]() { QApplication::clipboard()->setText(regionText->text()); });

        auto* actionForm = new QFormLayout;
        x_ = new QSpinBox(this); x_->setRange(0, 10000);
        y_ = new QSpinBox(this); y_->setRange(0, 10000);
        x_->setObjectName(QStringLiteral("adbX"));
        y_->setObjectName(QStringLiteral("adbY"));
        auto* clickButton = new QPushButton(QStringLiteral("ADB tap"), this);
        auto* clickRow = new QHBoxLayout;
        clickRow->addWidget(x_); clickRow->addWidget(y_); clickRow->addWidget(clickButton);
        actionForm->addRow(QStringLiteral("Tap x / y"), clickRow);
        text_ = new QLineEdit(this);
        auto* inputButton = new QPushButton(QStringLiteral("Input text"), this);
        auto* inputRow = new QHBoxLayout;
        inputRow->addWidget(text_, 1); inputRow->addWidget(inputButton);
        actionForm->addRow(QStringLiteral("Text (ASCII)"), inputRow);
        key_ = new QLineEdit(QStringLiteral("KEYCODE_ENTER"), this);
        auto* keyButton = new QPushButton(QStringLiteral("Key event"), this);
        auto* keyRow = new QHBoxLayout;
        keyRow->addWidget(key_, 1); keyRow->addWidget(keyButton);
        actionForm->addRow(QStringLiteral("Key code"), keyRow);
        root->addLayout(actionForm);

        pcActions_ = new QWidget(this);
        auto* pcForm = new QFormLayout(pcActions_);
        pcForm->setContentsMargins(0, 0, 0, 0);
        pcX_ = new QSpinBox(this); pcX_->setRange(0, 32767);
        pcY_ = new QSpinBox(this); pcY_->setRange(0, 32767);
        pcX_->setObjectName(QStringLiteral("pcClientX"));
        pcY_->setObjectName(QStringLiteral("pcClientY"));
        auto* pcClick = new QPushButton(QStringLiteral("PC 点击"), this);
        auto* pcClickRow = new QHBoxLayout;
        pcClickRow->addWidget(pcX_); pcClickRow->addWidget(pcY_); pcClickRow->addWidget(pcClick);
        pcForm->addRow(QStringLiteral("客户区 x / y"), pcClickRow);
        auto* pcKeyRow = new QHBoxLayout;
        pcKey_ = new QComboBox(this);
        pcKey_->addItem(QStringLiteral("Enter"), VK_RETURN);
        pcKey_->addItem(QStringLiteral("Backspace"), VK_BACK);
        pcKey_->addItem(QStringLiteral("Esc"), VK_ESCAPE);
        pcKey_->addItem(QStringLiteral("Tab"), VK_TAB);
        pcKey_->addItem(QStringLiteral("Space"), VK_SPACE);
        pcKey_->addItem(QStringLiteral("Delete"), VK_DELETE);
        pcKey_->addItem(QStringLiteral("End"), VK_END);
        auto* pcKeyButton = new QPushButton(QStringLiteral("PC 按键"), this);
        auto* pcTextButton = new QPushButton(QStringLiteral("PC 输入上方文本"), this);
        pcKeyRow->addWidget(pcKey_, 1); pcKeyRow->addWidget(pcKeyButton); pcKeyRow->addWidget(pcTextButton);
        pcForm->addRow(QStringLiteral("PC 输入"), pcKeyRow);
        root->addWidget(pcActions_);
        pcActions_->hide();
        status_ = new QLabel(QStringLiteral("Ready"), this);
        status_->setWordWrap(true);
        root->addWidget(status_);

        connect(browse, &QPushButton::clicked, this, [this]() {
            const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Select adb.exe"), QString(), QStringLiteral("adb.exe (adb.exe)"));
            if (!path.isEmpty()) adbPath_->setText(path);
        });
        connect(connectButton, &QPushButton::clicked, this, [this]() { connectDevice(); });
        connect(screenshotButton, &QPushButton::clicked, this, [this]() { takeScreenshot(); });
        connect(refreshWindows, &QPushButton::clicked, this, [this]() { refreshWindowList(); });
        connect(windowList_, &QComboBox::currentIndexChanged, this, [this](int index) {
            if (index >= 0) selectWindow(reinterpret_cast<HWND>(static_cast<quintptr>(windowList_->itemData(index).toULongLong())));
        });
        connect(bindHandle, &QPushButton::clicked, this, [this]() {
            bool ok = false;
            const auto value = hwndEdit_->text().trimmed().toULongLong(&ok, 0);
            if (!ok || value > std::numeric_limits<quintptr>::max())
                status_->setText(QStringLiteral("请输入有效的十进制或 0x 开头的窗口句柄"));
            else selectWindow(reinterpret_cast<HWND>(static_cast<quintptr>(value)));
        });
        connect(source_, &QComboBox::currentIndexChanged, this, [this](int index) {
            stopPreview();
            windowPanel_->setVisible(index == 1);
            pcActions_->setVisible(index == 1);
            clearScreenshot();
            if (index == 1) refreshWindowList();
        });
        connect(copyPoint, &QPushButton::clicked, this, [this]() {
            if (hasPickedPoint_)
                QApplication::clipboard()->setText(QStringLiteral("(%1,%2)").arg(pickedPoint_.x()).arg(pickedPoint_.y()));
            else status_->setText(QStringLiteral("请先在截图上取点"));
        });
        connect(copyDetails, &QPushButton::clicked, this, [this]() {
            if (lastImage_.isNull()) { status_->setText(QStringLiteral("请先截图")); return; }
            QString details = snapshotDescription_;
            if (hasPickedPoint_)
                details += QStringLiteral("\n坐标：(%1,%2)").arg(pickedPoint_.x()).arg(pickedPoint_.y());
            if (!selectedRegion_.isEmpty())
                details += QStringLiteral("\nOCR 区域：(%1,%2,%3,%4)")
                    .arg(selectedRegion_.x()).arg(selectedRegion_.y()).arg(selectedRegion_.width()).arg(selectedRegion_.height());
            QApplication::clipboard()->setText(details);
            status_->setText(QStringLiteral("已复制截图来源、原始大小及坐标/选区"));
        });
        connect(saveImage, &QPushButton::clicked, this, [this]() {
            if (lastImage_.isNull()) { status_->setText(QStringLiteral("请先截图")); return; }
            const auto path = QFileDialog::getSaveFileName(this, QStringLiteral("保存原始截图"), QString(), QStringLiteral("PNG (*.png)"));
            if (!path.isEmpty()) status_->setText(lastImage_.save(path, "PNG") ? QStringLiteral("原始截图已保存") : QStringLiteral("保存失败"));
        });
        connect(pcClick, &QPushButton::clicked, this, [this]() { clickPcPoint(); });
        connect(pcKeyButton, &QPushButton::clicked, this, [this]() {
            if (!activatePcWindow()) return;
            INPUT events[2]{};
            events[0].type = events[1].type = INPUT_KEYBOARD;
            events[0].ki.wVk = events[1].ki.wVk = static_cast<WORD>(pcKey_->currentData().toInt());
            events[1].ki.dwFlags = KEYEVENTF_KEYUP;
            status_->setText(SendInput(2, events, sizeof(INPUT)) == 2 ? QStringLiteral("PC 按键已发送") : QStringLiteral("PC 按键发送失败"));
        });
        connect(pcTextButton, &QPushButton::clicked, this, [this]() {
            if (text_->text().isEmpty() || !activatePcWindow()) return;
            for (QChar character : text_->text()) {
                INPUT events[2]{};
                events[0].type = events[1].type = INPUT_KEYBOARD;
                events[0].ki.wScan = events[1].ki.wScan = character.unicode();
                events[0].ki.dwFlags = KEYEVENTF_UNICODE;
                events[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
                if (SendInput(2, events, sizeof(INPUT)) != 2) { status_->setText(QStringLiteral("PC 文本发送失败")); return; }
            }
            status_->setText(QStringLiteral("PC 文本已发送"));
        });
        connect(clickButton, &QPushButton::clicked, this, [this]() { runAdb({QStringLiteral("shell"), QStringLiteral("input"), QStringLiteral("tap"), QString::number(x_->value()), QString::number(y_->value())}, QStringLiteral("tap sent")); });
        connect(inputButton, &QPushButton::clicked, this, [this]() { runAdb({QStringLiteral("shell"), QStringLiteral("input"), QStringLiteral("text"), encodeAdbText(text_->text())}, QStringLiteral("text input sent")); });
        connect(keyButton, &QPushButton::clicked, this, [this]() { runAdb({QStringLiteral("shell"), QStringLiteral("input"), QStringLiteral("keyevent"), key_->text().trimmed()}, QStringLiteral("key event sent")); });
        screenshot_->clicked = [this](const QPoint& point) {
            hasPickedPoint_ = true;
            pickedPoint_ = source_->currentIndex() == 2 ? point + desktopOrigin_ : point;
            if (source_->currentIndex() == 0) { x_->setValue(point.x()); y_->setValue(point.y()); }
            else if (source_->currentIndex() == 1) { pcX_->setValue(point.x()); pcY_->setValue(point.y()); }
            coordinate_->setText(pointDescription(point));
        };
        screenshot_->moved = [this](const QPoint& point) {
            coordinate_->setText(pointDescription(point));
        };
        connect(refreshDevices, &QPushButton::clicked, this, [this] { refreshAdbDevices(); });
        connect(transport_, &QComboBox::currentIndexChanged, this, [this] {
            stopPreview(); clearScreenshot(); serial_->setEnabled(transport_->currentIndex() == 0);
            devices_->setEnabled(transport_->currentIndex() == 1); refreshAdbDevices();
        });
        connect(devices_, &QComboBox::currentIndexChanged, this, [this] { stopPreview(); clearScreenshot(); });
        connect(serial_, &QLineEdit::textChanged, this, [this] { stopPreview(); clearScreenshot(); });
        connect(adbPath_, &QLineEdit::textChanged, this, [this] { stopPreview(); clearScreenshot(); });
        connect(live_, &QCheckBox::toggled, this, [this](bool enabled) {
            if (enabled) { takeScreenshot(); if (live_->isChecked()) previewTimer_.start(interval_->value()); }
            else { ++generation_; previewTimer_.stop(); screenshotAdb_.cancel(); }
        });
        connect(interval_, &QSpinBox::valueChanged, this, [this](int ms) { if (live_->isChecked()) previewTimer_.start(ms); });
        connect(&previewTimer_, &QTimer::timeout, this, [this] { takeScreenshot(); });
        devices_->setEnabled(false);
        status_->setText(QStringLiteral("内置 ADB 已就绪；选择网络地址或 USB 设备。实时窗口预览需保持目标无遮挡。"));
    }
    ~AdbDemo() override { ++generation_; previewTimer_.stop(); screenshotAdb_.cancel(); commandsAdb_.cancel(); }

private:
    QString pointDescription(QPoint point) const
    {
        if (source_->currentIndex() == 2) point += desktopOrigin_;
        return QStringLiteral("%1坐标：(%2,%3)").arg(source_->currentIndex() == 0 ? QStringLiteral("手机")
            : source_->currentIndex() == 1 ? QStringLiteral("客户区") : QStringLiteral("桌面")).arg(point.x()).arg(point.y());
    }
    void stopPreview()
    {
        ++generation_; previewTimer_.stop(); screenshotAdb_.cancel();
        if (live_) { QSignalBlocker block(live_); live_->setChecked(false); }
    }
    void clearScreenshot()
    {
        hasPickedPoint_ = false;
        lastImage_ = {};
        pickedPoint_ = {-1, -1};
        snapshotDescription_.clear();
        screenshot_->setImage({});
        coordinate_->setText(QStringLiteral("请截图后移动鼠标取点，或拖动框选"));
        resolution_->setText(QStringLiteral("原始截图大小：等待截图"));
    }

    void refreshWindowList()
    {
        const QSignalBlocker block(windowList_);
        windowList_->clear();
        windowList_->addItem(QStringLiteral("请选择目标窗口"), QVariant::fromValue<qulonglong>(0));
        struct Context { AdbDemo* demo; HWND ownWindow; } context{this, reinterpret_cast<HWND>(winId())};
        EnumWindows([](HWND hwnd, LPARAM value) -> BOOL {
            auto* context = reinterpret_cast<Context*>(value);
            if (hwnd == context->ownWindow || !IsWindowVisible(hwnd)) return TRUE;
            const int length = GetWindowTextLengthW(hwnd);
            if (length <= 0) return TRUE;
            std::wstring title(static_cast<size_t>(length + 1), L'\0');
            GetWindowTextW(hwnd, title.data(), length + 1);
            WindowClientCapture::Geometry geometry;
            QString error;
            if (WindowClientCapture::geometry(hwnd, geometry, error))
                context->demo->windowList_->addItem(QStringLiteral("%1 | %2 | %3×%4")
                    .arg(QString::fromWCharArray(title.c_str()), WindowClientCapture::handleText(hwnd))
                    .arg(geometry.size.width()).arg(geometry.size.height()),
                    QVariant::fromValue<qulonglong>(reinterpret_cast<quintptr>(hwnd)));
            return TRUE;
        }, reinterpret_cast<LPARAM>(&context));
        const int boundIndex = windowList_->findData(QVariant::fromValue<qulonglong>(reinterpret_cast<quintptr>(pcHwnd_)));
        windowList_->setCurrentIndex(boundIndex >= 0 ? boundIndex : 0);
        if (pcHwnd_ && !IsWindow(pcHwnd_))
        {
            pcHwnd_ = nullptr;
            hwndEdit_->clear();
            geometryLabel_->setText(QStringLiteral("原窗口已关闭，请重新选择窗口"));
            clearScreenshot();
        }
    }

    void selectWindow(HWND hwnd)
    {
        stopPreview();
        clearScreenshot();
        WindowClientCapture::Geometry geometry;
        QString error;
        if (!WindowClientCapture::geometry(hwnd, geometry, error))
        {
            pcHwnd_ = nullptr;
            geometryLabel_->setText(error);
            status_->setText(error);
            return;
        }
        if (GetAncestor(hwnd, GA_ROOT) == reinterpret_cast<HWND>(winId()))
        {
            status_->setText(QStringLiteral("请选择工具以外的目标窗口"));
            pcHwnd_ = nullptr;
            return;
        }
        pcHwnd_ = hwnd;
        hwndEdit_->setText(WindowClientCapture::handleText(hwnd));
        geometryLabel_->setText(QStringLiteral("HWND %1；客户区 %2×%3 像素；客户区屏幕原点 (%4,%5)")
            .arg(WindowClientCapture::handleText(hwnd)).arg(geometry.size.width()).arg(geometry.size.height())
            .arg(geometry.screenOrigin.x()).arg(geometry.screenOrigin.y()));
        status_->setText(QStringLiteral("窗口已选择，点击 Screenshot 获取客户区；截图时工具会暂时隐藏"));
    }

    void takeScreenshot()
    {
        if (source_->currentIndex() == 0) { takeAdbScreenshot(); return; }
        if (captureInProgress_) return;
        WindowClientCapture::Geometry geometry;
        QString error;
        if (source_->currentIndex() == 1 && !WindowClientCapture::geometry(pcHwnd_, geometry, error))
        {
            stopPreview(); clearScreenshot(); status_->setText(error); return;
        }
        captureInProgress_ = true;
        const bool desktop = source_->currentIndex() == 2;
        const bool live = live_->isChecked();
        const auto version = generation_;
        const HWND target = pcHwnd_;
        if (!live) hide();
        // Single captures hide the tool. Live capture leaves it interactive and uses a worker.
        QTimer::singleShot(live ? 0 : 180, this, [this, desktop, live, version, target]() {
            auto* watcher = new QFutureWatcher<Frame>(this);
            connect(watcher, &QFutureWatcher<Frame>::finished, this, [this, watcher, desktop, live, version, target] {
                const Frame frame = watcher->result(); watcher->deleteLater();
                captureInProgress_ = false;
                if (!live) show();
                if (version != generation_) return;
                if (frame.image.isNull()) { stopPreview(); clearScreenshot(); status_->setText(frame.error); return; }
                if (desktopOrigin_ != frame.geometry.screenOrigin && desktop) clearScreenshot();
                desktopOrigin_ = frame.geometry.screenOrigin;
                if (lastImage_.size() != frame.image.size()) { hasPickedPoint_ = false; }
                lastImage_ = frame.image;
                snapshotDescription_ = desktop
                    ? QStringLiteral("来源：Windows 整个桌面\n桌面大小：%1×%2\n桌面原点：(%3,%4)\n坐标：Windows 桌面物理像素，可为负数")
                        .arg(frame.image.width()).arg(frame.image.height()).arg(desktopOrigin_.x()).arg(desktopOrigin_.y())
                    : QStringLiteral("来源：Windows 客户区（桌面截图）\nHWND：%1\n客户区大小：%2×%3\n客户区屏幕原点：(%4,%5)\n坐标原点：客户区左上角 (0,0)，单位为原始物理像素")
                        .arg(WindowClientCapture::handleText(target)).arg(frame.image.width()).arg(frame.image.height())
                        .arg(frame.geometry.screenOrigin.x()).arg(frame.geometry.screenOrigin.y());
                screenshot_->setImage(frame.image, live);
                resolution_->setText(QStringLiteral("原始画面：%1×%2；%3像素坐标；取帧 %4ms")
                    .arg(frame.image.width()).arg(frame.image.height()).arg(desktop ? QStringLiteral("桌面") : QStringLiteral("客户区")).arg(frame.elapsed));
                if (!desktop) geometryLabel_->setText(snapshotDescription_);
                status_->setText(live ? QStringLiteral("实时预览中：请保持目标窗口无遮挡；停止预览可冻结画面取坐标") : QStringLiteral("截图完成，可取点或框选"));
            });
            watcher->setFuture(QtConcurrent::run([target, desktop] {
                Frame frame; QElapsedTimer timer; timer.start();
                frame.image = WindowClientCapture::capture(target, frame.geometry, frame.error, desktop);
                frame.elapsed = timer.elapsed(); return frame;
            }));
        });
    }

    bool activatePcWindow()
    {
        WindowClientCapture::Geometry geometry;
        QString error;
        if (!WindowClientCapture::geometry(pcHwnd_, geometry, error)) { status_->setText(error); return false; }
        const HWND top = GetAncestor(pcHwnd_, GA_ROOT);
        if (IsIconic(top)) { status_->setText(QStringLiteral("请先取消目标窗口最小化")); return false; }
        SetForegroundWindow(top);
        if (GetForegroundWindow() != top)
        {
            status_->setText(QStringLiteral("目标窗口未取得焦点，本次未发送输入"));
            return false;
        }
        return true;
    }

    void clickPcPoint()
    {
        WindowClientCapture::DpiScope dpi;
        WindowClientCapture::Geometry geometry;
        QString error;
        if (!WindowClientCapture::geometry(pcHwnd_, geometry, error)) { status_->setText(error); return; }
        const QPoint point(pcX_->value(), pcY_->value());
        if (!QRect(QPoint(0, 0), geometry.size).contains(point))
        {
            status_->setText(QStringLiteral("点击坐标超出当前客户区"));
            return;
        }
        if (!activatePcWindow()) return;
        const auto screenPoint = geometry.screenOrigin + point;
        if (!SetCursorPos(screenPoint.x(), screenPoint.y())) { status_->setText(QStringLiteral("鼠标定位失败")); return; }
        INPUT events[2]{};
        events[0].type = events[1].type = INPUT_MOUSE;
        events[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        events[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        status_->setText(SendInput(2, events, sizeof(INPUT)) == 2
            ? QStringLiteral("PC 点击已发送：客户区 (%1,%2)").arg(point.x()).arg(point.y())
            : QStringLiteral("PC 点击发送失败"));
    }

    QString adbExecutable()
    {
        if (!adbPath_->text().trimmed().isEmpty()) return adbPath_->text().trimmed();
        QString error;
        const QString path = BundledAdb::executablePath(error);
        if (path.isEmpty()) status_->setText(error);
        return path;
    }
    QString deviceSerial()
    {
        if (transport_->currentIndex() == 0) return serial_->text().trimmed();
        if (devices_->currentData(Qt::UserRole + 1).toString() != QStringLiteral("device"))
        { status_->setText(QStringLiteral("USB 设备未就绪：请连接数据线，在手机允许 USB 调试授权，再刷新设备")); return {}; }
        return devices_->currentData().toString();
    }
    void refreshAdbDevices()
    {
        const auto path = adbExecutable();
        if (path.isEmpty()) return;
        const QString previous = devices_->currentData().toString();
        if (!commandsAdb_.start(path, {QStringLiteral("devices"), QStringLiteral("-l")},
            [this, previous](bool ok, QByteArray output, QString error) {
                stopPreview(); clearScreenshot();
                QSignalBlocker block(devices_); devices_->clear();
                if (!ok) { status_->setText(error); return; }
                for (const auto& device : parseDemoDevices(output))
                {
                    if (!device.usb) continue;
                    const auto state = device.state == "device" ? QStringLiteral("已连接")
                        : device.state == "unauthorized" ? QStringLiteral("未授权：请在手机确认")
                        : device.state == "offline" ? QStringLiteral("离线") : device.state;
                    devices_->addItem(device.serial + QStringLiteral(" · ") + state, device.serial);
                    devices_->setItemData(devices_->count() - 1, device.state, Qt::UserRole + 1);
                    devices_->setItemData(devices_->count() - 1, device.description, Qt::ToolTipRole);
                }
                const int previousIndex = devices_->findData(previous);
                if (previousIndex >= 0) devices_->setCurrentIndex(previousIndex);
                status_->setText(devices_->count() ? QStringLiteral("USB 设备已刷新，选择设备后点击连接或截图")
                    : QStringLiteral("未找到 USB 设备：检查数据线、USB 调试授权和设备驱动"));
            })) status_->setText(QStringLiteral("ADB 指令正在执行，请稍后再试"));
    }
    void runAdb(const QStringList& arguments, const QString& success)
    {
        const auto serial = deviceSerial(); const auto path = adbExecutable();
        if (path.isEmpty() || serial.isEmpty()) return;
        // Let the next preview tick capture after this command completes.
        if (!commandsAdb_.start(path, QStringList{QStringLiteral("-s"), serial} + arguments,
            [this, success](bool ok, QByteArray, QString error) { status_->setText(ok ? success : error); }))
            status_->setText(QStringLiteral("ADB 指令正在执行，请稍后再试"));
    }

    void connectDevice()
    {
        stopPreview();
        source_->setCurrentIndex(0);
        const auto serial = deviceSerial(); const auto path = adbExecutable();
        if (path.isEmpty() || serial.isEmpty()) return;
        const auto version = generation_;
        const bool usb = transport_->currentIndex() == 1;
        const QStringList args = usb ? QStringList{QStringLiteral("-s"), serial, QStringLiteral("get-state")}
            : QStringList{QStringLiteral("connect"), serial};
        if (!commandsAdb_.start(path, args, [this, version, usb](bool ok, QByteArray output, QString error) {
            if (version != generation_) return;
            const auto response = QString::fromLocal8Bit(output).trimmed();
            const bool connected = ok && (usb ? response == "device" : response.contains("connected to"));
            if (!connected) { status_->setText(QStringLiteral("连接失败：%1").arg(error.isEmpty() ? response : error)); return; }
            status_->setText(response); takeAdbScreenshot();
        })) status_->setText(QStringLiteral("ADB 指令正在执行，请稍后再试"));
    }

    void takeAdbScreenshot()
    {
        if (screenshotAdb_.busy() || commandsAdb_.busy() || captureInProgress_) return;
        const auto serial = deviceSerial(); const auto path = adbExecutable();
        if (path.isEmpty() || serial.isEmpty()) { stopPreview(); return; }
        const auto version = generation_; const bool live = live_->isChecked();
        QElapsedTimer timer; timer.start();
        screenshotAdb_.start(path, {QStringLiteral("-s"), serial, QStringLiteral("exec-out"), QStringLiteral("screencap"), QStringLiteral("-p")},
            [this, serial, version, live, timer](bool ok, QByteArray output, QString error) {
                if (version != generation_) return;
                if (!ok) { stopPreview(); clearScreenshot(); status_->setText(error); return; }
                captureInProgress_ = true;
                auto* watcher = new QFutureWatcher<QImage>(this);
                connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, serial, version, live, timer] {
                    const auto image = watcher->result(); watcher->deleteLater(); captureInProgress_ = false;
                    if (version != generation_) return;
                    if (image.isNull()) { stopPreview(); clearScreenshot(); status_->setText(QStringLiteral("ADB 截图解码失败")); return; }
                    if (lastImage_.size() != image.size()) hasPickedPoint_ = false;
                    screenshot_->setImage(image, live); lastImage_ = image;
                    snapshotDescription_ = QStringLiteral("来源：ADB 设备截图\n设备：%1\n设备截图大小：%2×%3\n坐标原点：手机截图左上角 (0,0)，单位为原始设备像素")
                        .arg(serial).arg(image.width()).arg(image.height());
                    resolution_->setText(QStringLiteral("设备原始画面：%1×%2；取帧 %3ms").arg(image.width()).arg(image.height()).arg(timer.elapsed()));
                    status_->setText(live ? QStringLiteral("ADB 实时预览中；取点和框选按手机原始像素") : QStringLiteral("设备截图完成"));
                });
                watcher->setFuture(QtConcurrent::run([output] { return QImage::fromData(output, "PNG"); }));
            }, 20000);
    }

    QLineEdit* adbPath_ = nullptr;
    QLineEdit* serial_ = nullptr;
    QLineEdit* text_ = nullptr;
    QLineEdit* key_ = nullptr;
    QSpinBox* x_ = nullptr;
    QSpinBox* y_ = nullptr;
    QLabel* coordinate_ = nullptr;
    QLabel* resolution_ = nullptr;
    QLabel* status_ = nullptr;
    ScreenshotLabel* screenshot_ = nullptr;
    QComboBox* source_ = nullptr;
    QComboBox* windowList_ = nullptr;
    QComboBox* pcKey_ = nullptr;
    QLineEdit* hwndEdit_ = nullptr;
    QLabel* geometryLabel_ = nullptr;
    QWidget* windowPanel_ = nullptr;
    QWidget* pcActions_ = nullptr;
    QSpinBox* pcX_ = nullptr;
    QSpinBox* pcY_ = nullptr;
    HWND pcHwnd_ = nullptr;
    QImage lastImage_;
    QString snapshotDescription_;
    QPoint pickedPoint_{-1, -1};
    QRect selectedRegion_;
    bool captureInProgress_ = false;
    struct Frame { QImage image; WindowClientCapture::Geometry geometry; QString error; qint64 elapsed = 0; };
    QComboBox* transport_ = nullptr;
    QComboBox* devices_ = nullptr;
    QCheckBox* live_ = nullptr;
    QSpinBox* interval_ = nullptr;
    QTimer previewTimer_;
    AsyncAdb commandsAdb_, screenshotAdb_;
    quint64 generation_ = 0;
    QPoint desktopOrigin_;
    bool hasPickedPoint_ = false;
};

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    AdbDemo demo;
    demo.show();
    return app.exec();
}
