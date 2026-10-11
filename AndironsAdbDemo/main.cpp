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

    void setImage(const QImage& image)
    {
        image_ = image;
        clearSelection();
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
        source_->addItems({QStringLiteral("ADB 设备截图 / 手机坐标"), QStringLiteral("Windows 客户区截图 / 客户区坐标")});
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
        root->addLayout(connection);

        auto* connectButton = new QPushButton(QStringLiteral("Connect"), this);
        auto* screenshotButton = new QPushButton(QStringLiteral("Screenshot"), this);
        screenshotButton->setObjectName(QStringLiteral("takeScreenshot"));
        auto* connectionRow = new QHBoxLayout;
        connectionRow->addWidget(connectButton);
        connectionRow->addWidget(screenshotButton);
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
            selectedRegion_ = region;
            regionText->setText(region.isEmpty() ? QString()
                : QStringLiteral("(%1,%2,%3,%4)").arg(region.x()).arg(region.y()).arg(region.width()).arg(region.height()));
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
            windowPanel_->setVisible(index == 1);
            pcActions_->setVisible(index == 1);
            clearScreenshot();
            if (index == 1) refreshWindowList();
        });
        connect(copyPoint, &QPushButton::clicked, this, [this]() {
            if (pickedPoint_.x() >= 0)
                QApplication::clipboard()->setText(QStringLiteral("(%1,%2)").arg(pickedPoint_.x()).arg(pickedPoint_.y()));
            else status_->setText(QStringLiteral("请先在截图上取点"));
        });
        connect(copyDetails, &QPushButton::clicked, this, [this]() {
            if (lastImage_.isNull()) { status_->setText(QStringLiteral("请先截图")); return; }
            QString details = snapshotDescription_;
            if (pickedPoint_.x() >= 0)
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
            pickedPoint_ = point;
            if (source_->currentIndex() == 0) { x_->setValue(point.x()); y_->setValue(point.y()); }
            else { pcX_->setValue(point.x()); pcY_->setValue(point.y()); }
            coordinate_->setText(QStringLiteral("%1坐标：(%2,%3)").arg(source_->currentIndex() == 0 ? QStringLiteral("手机") : QStringLiteral("客户区")).arg(point.x()).arg(point.y()));
        };
        screenshot_->moved = [this](const QPoint& point) {
            coordinate_->setText(QStringLiteral("%1坐标：(%2,%3)").arg(source_->currentIndex() == 0 ? QStringLiteral("手机") : QStringLiteral("客户区")).arg(point.x()).arg(point.y()));
        };
    }

private:
    void clearScreenshot()
    {
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
        if (!WindowClientCapture::geometry(pcHwnd_, geometry, error))
        {
            clearScreenshot(); status_->setText(error); return;
        }
        captureInProgress_ = true;
        hide();
        // Give the compositor time to remove this tool from the desktop image.
        QTimer::singleShot(180, this, [this]() {
            WindowClientCapture::Geometry geometry;
            QString error;
            QElapsedTimer timer;
            timer.start();
            const QImage image = WindowClientCapture::capture(pcHwnd_, geometry, error);
            const qint64 elapsed = timer.elapsed();
            clearScreenshot();
            if (!image.isNull())
            {
                lastImage_ = image;
                snapshotDescription_ = QStringLiteral("来源：Windows 客户区（桌面截图）\nHWND：%1\n客户区大小：%2×%3\n客户区屏幕原点：(%4,%5)\n坐标原点：客户区左上角 (0,0)，单位为原始物理像素")
                    .arg(WindowClientCapture::handleText(pcHwnd_)).arg(image.width()).arg(image.height())
                    .arg(geometry.screenOrigin.x()).arg(geometry.screenOrigin.y());
                screenshot_->setImage(image);
                resolution_->setText(QStringLiteral("客户区原始截图：%1×%2；选区为客户区像素，与预览缩放无关")
                    .arg(image.width()).arg(image.height()));
                geometryLabel_->setText(QStringLiteral("HWND %1；客户区 %2×%3；屏幕原点 (%4,%5)")
                    .arg(WindowClientCapture::handleText(pcHwnd_)).arg(image.width()).arg(image.height())
                    .arg(geometry.screenOrigin.x()).arg(geometry.screenOrigin.y()));
                status_->setText(QStringLiteral("本机截图 %1ms；请检查画面是否完整、是否被其他窗口遮挡").arg(elapsed));
            }
            else status_->setText(error);
            captureInProgress_ = false;
            show();
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

    bool runAdb(const QStringList& arguments, const QString& success)
    {
        QProcess process;
        process.start(adbPath_->text().trimmed(), QStringList{QStringLiteral("-s"), serial_->text().trimmed()} + arguments);
        if (!process.waitForFinished(15000)) {
            status_->setText(QStringLiteral("ADB timeout: %1").arg(process.errorString()));
            return false;
        }
        const QString error = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        if (process.exitCode() != 0) {
            status_->setText(QStringLiteral("ADB failed: %1").arg(error.isEmpty() ? QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed() : error));
            return false;
        }
        status_->setText(success);
        return true;
    }

    void connectDevice()
    {
        QProcess process;
        process.start(adbPath_->text().trimmed(), {QStringLiteral("connect"), serial_->text().trimmed()});
        if (!process.waitForFinished(15000)) {
            status_->setText(QStringLiteral("Connect timeout: %1").arg(process.errorString()));
            return;
        }
        status_->setText(QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed());
        source_->setCurrentIndex(0);
        takeAdbScreenshot();
    }

    void takeAdbScreenshot()
    {
        clearScreenshot();
        QProcess process;
        process.start(adbPath_->text().trimmed(), {QStringLiteral("-s"), serial_->text().trimmed(), QStringLiteral("exec-out"), QStringLiteral("screencap"), QStringLiteral("-p")});
        if (!process.waitForFinished(20000)) {
            status_->setText(QStringLiteral("Screenshot timeout: %1").arg(process.errorString()));
            return;
        }
        QImage image;
        if (!image.loadFromData(process.readAllStandardOutput(), "PNG")) {
            status_->setText(QStringLiteral("Screenshot decode failed: %1").arg(QString::fromLocal8Bit(process.readAllStandardError()).trimmed()));
            return;
        }
        screenshot_->setImage(image);
        lastImage_ = image;
        snapshotDescription_ = QStringLiteral("来源：ADB 设备截图\n设备：%1\n设备截图大小：%2×%3\n坐标原点：手机截图左上角 (0,0)，单位为原始设备像素")
            .arg(serial_->text().trimmed()).arg(image.width()).arg(image.height());
        resolution_->setText(QStringLiteral("设备截图分辨率：%1 × %2；选区为手机原始像素，与预览大小无关")
            .arg(image.width()).arg(image.height()));
        status_->setText(QStringLiteral("Screenshot received: %1 x %2").arg(image.width()).arg(image.height()));
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
};

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    AdbDemo demo;
    demo.show();
    return app.exec();
}
