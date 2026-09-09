#pragma once

#include <QWidget>

class QMouseEvent;
class QString;

class FramelessTitleBar final : public QWidget
{
public:
    explicit FramelessTitleBar(const QString& title, QWidget* targetWindow, QWidget* parent = nullptr);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    void toggleMaximized();

    QWidget* targetWindow_ = nullptr;
};