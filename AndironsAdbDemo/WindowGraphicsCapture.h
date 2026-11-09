#pragma once

#include <QImage>
#include <QString>

#include <memory>
#include <windows.h>

// Keeps a Windows Graphics Capture session open while previewing one window.
class WindowGraphicsCapture
{
public:
    WindowGraphicsCapture();
    ~WindowGraphicsCapture();

    bool start(HWND hwnd, QString& error);
    void stop();
    bool latest(QImage& image) const;
    HWND target() const;

private:
    struct State;
    std::shared_ptr<State> state_;
};
