#pragma once

#include <QWidget>

#ifdef _WIN32
#include <Windows.h>
#include <windowsx.h>
#endif

inline bool handleFramelessResize(QWidget* window, void* message, qintptr* result, int borderWidth = 7)
{
#ifdef _WIN32
    if (!window || !message || !result || window->isMaximized())
    {
        return false;
    }

    auto* nativeMessage = static_cast<MSG*>(message);
    if (nativeMessage->message != WM_NCHITTEST)
    {
        return false;
    }

    RECT rect{};
    if (!GetWindowRect(reinterpret_cast<HWND>(window->winId()), &rect))
    {
        return false;
    }

    const int physicalBorder = qMax(1, qRound(borderWidth * window->devicePixelRatioF()));
    const long x = GET_X_LPARAM(nativeMessage->lParam);
    const long y = GET_Y_LPARAM(nativeMessage->lParam);
    const bool left = x >= rect.left && x < rect.left + physicalBorder;
    const bool right = x <= rect.right && x > rect.right - physicalBorder;
    const bool top = y >= rect.top && y < rect.top + physicalBorder;
    const bool bottom = y <= rect.bottom && y > rect.bottom - physicalBorder;

    if (top && left) *result = HTTOPLEFT;
    else if (top && right) *result = HTTOPRIGHT;
    else if (bottom && left) *result = HTBOTTOMLEFT;
    else if (bottom && right) *result = HTBOTTOMRIGHT;
    else if (left) *result = HTLEFT;
    else if (right) *result = HTRIGHT;
    else if (top) *result = HTTOP;
    else if (bottom) *result = HTBOTTOM;
    else return false;

    return true;
#else
    Q_UNUSED(window);
    Q_UNUSED(message);
    Q_UNUSED(result);
    Q_UNUSED(borderWidth);
    return false;
#endif
}