#include "FramelessTitleBar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QWindow>

FramelessTitleBar::FramelessTitleBar(const QString& title, QWidget* targetWindow, QWidget* parent)
    : QWidget(parent), targetWindow_(targetWindow)
{
    setObjectName(QStringLiteral("framelessTitleBar"));
    setFixedHeight(38);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(13, 0, 0, 0);
    layout->setSpacing(0);

    auto* titleLabel = new QLabel(title, this);
    titleLabel->setObjectName(QStringLiteral("windowTitleText"));
    layout->addWidget(titleLabel);
    layout->addStretch();

    auto createControlButton = [this](const QString& objectName, const QString& text, const QString& toolTip) {
        auto* button = new QPushButton(this);
        button->setObjectName(objectName);
        button->setText(text);
        button->setToolTip(toolTip);
        button->setAccessibleName(toolTip);
        button->setFixedSize(42, 38);
        button->setFocusPolicy(Qt::NoFocus);
        button->setCursor(Qt::PointingHandCursor);
        return button;
    };

    auto* minimizeButton = createControlButton(QStringLiteral("windowMinimizeButton"), QStringLiteral("−"), QStringLiteral("最小化"));
    auto* maximizeButton = createControlButton(QStringLiteral("windowMaximizeButton"), QStringLiteral("□"), QStringLiteral("最大化或还原"));
    auto* closeButton = createControlButton(QStringLiteral("windowCloseButton"), QStringLiteral("×"), QStringLiteral("关闭"));
    layout->addWidget(minimizeButton);
    layout->addWidget(maximizeButton);
    layout->addWidget(closeButton);

    connect(minimizeButton, &QPushButton::clicked, targetWindow_, &QWidget::showMinimized);
    connect(maximizeButton, &QPushButton::clicked, this, [this]() { toggleMaximized(); });
    connect(closeButton, &QPushButton::clicked, targetWindow_, &QWidget::close);
}

void FramelessTitleBar::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && targetWindow_ && targetWindow_->windowHandle())
    {
        // 使用系统移动保持多屏和高 DPI 下的原生拖动行为。
        targetWindow_->windowHandle()->startSystemMove();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void FramelessTitleBar::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        toggleMaximized();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void FramelessTitleBar::toggleMaximized()
{
    if (!targetWindow_)
    {
        return;
    }

    // 双击标题栏和中间圆点共用最大化/还原状态约定。
    if (targetWindow_->isMaximized())
    {
        targetWindow_->showNormal();
    }
    else
    {
        targetWindow_->showMaximized();
    }
}
