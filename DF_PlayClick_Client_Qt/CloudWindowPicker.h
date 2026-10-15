#pragma once
#include <QPushButton>
#include <QTimer>
#include "Win32WindowBinder.h"

class CloudWindowPicker : public QPushButton
{
    Q_OBJECT
public:
    explicit CloudWindowPicker(QWidget* parent = nullptr);
    ~CloudWindowPicker() override;
signals:
    void windowPicked(quintptr hwnd);
    void pickFailed(const QString& reason);
protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool event(QEvent* event) override;
private:
    void cancelPick();
    quintptr targetAtCursor() const;
    void updateOutline();
    QTimer outlineTimer_;
    quintptr outline_ = 0;
    QVector<TargetWindowInfo> candidates_;
    bool picking_ = false;
};
