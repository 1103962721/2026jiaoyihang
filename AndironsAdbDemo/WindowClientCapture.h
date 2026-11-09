#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>

namespace WindowClientCapture
{
class DpiScope
{
public:
    DpiScope() : previous_(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
    ~DpiScope() { if (previous_) SetThreadDpiAwarenessContext(previous_); }
private:
    DPI_AWARENESS_CONTEXT previous_;
};

struct Geometry
{
    QSize size;
    QPoint screenOrigin;
};

inline QString handleText(HWND hwnd)
{
    return QStringLiteral("0x%1").arg(reinterpret_cast<quintptr>(hwnd), 0, 16).toUpper();
}

inline bool geometry(HWND hwnd, Geometry& result, QString& error)
{
    error.clear();
    DpiScope dpi;
    if (!hwnd || !IsWindow(hwnd))
    {
        error = QStringLiteral("窗口句柄无效，请重新选择或刷新窗口列表");
        return false;
    }
    RECT client{};
    POINT origin{};
    if (!GetClientRect(hwnd, &client) || !ClientToScreen(hwnd, &origin))
    {
        error = QStringLiteral("无法获取客户区尺寸或屏幕位置");
        return false;
    }
    result = { QSize(client.right - client.left, client.bottom - client.top), QPoint(origin.x, origin.y) };
    if (result.size.isEmpty())
    {
        error = QStringLiteral("窗口客户区大小为零");
        return false;
    }
    return true;
}

// Capture visible desktop pixels at the client rectangle. Handles GPU-rendered VM windows.
inline QImage capture(HWND hwnd, Geometry& result, QString& error, bool fullDesktop = false)
{
    DpiScope dpi;
    error.clear();
    if (!fullDesktop && !geometry(hwnd, result, error)) return {};
    if (!fullDesktop && (!IsWindowVisible(hwnd) || IsIconic(GetAncestor(hwnd, GA_ROOT))))
    {
        error = QStringLiteral("请先显示目标窗口并取消最小化");
        return {};
    }
    const QRect desktop(GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
    if (fullDesktop) result = {desktop.size(), desktop.topLeft()};
    if (!desktop.contains(QRect(result.screenOrigin, result.size)))
    {
        error = QStringLiteral("客户区部分位于屏幕外，请将目标窗口移入屏幕");
        return {};
    }
    HDC screen = GetDC(nullptr);
    HDC memory = screen ? CreateCompatibleDC(screen) : nullptr;
    HBITMAP bitmap = memory ? CreateCompatibleBitmap(screen, result.size.width(), result.size.height()) : nullptr;
    QImage image;
    if (bitmap)
    {
        HGDIOBJ previous = SelectObject(memory, bitmap);
        const BOOL copied = BitBlt(memory, 0, 0, result.size.width(), result.size.height(), screen,
            result.screenOrigin.x(), result.screenOrigin.y(), SRCCOPY | CAPTUREBLT);
        SelectObject(memory, previous);
        if (copied)
        {
            image = QImage(result.size, QImage::Format_RGB32);
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = result.size.width();
            info.bmiHeader.biHeight = -result.size.height();
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            if (image.isNull() || GetDIBits(memory, bitmap, 0, result.size.height(),
                image.bits(), &info, DIB_RGB_COLORS) != static_cast<int>(result.size.height()))
                image = {};
        }
        DeleteObject(bitmap);
    }
    if (memory) DeleteDC(memory);
    if (screen) ReleaseDC(nullptr, screen);
    if (image.isNull()) error = QStringLiteral("读取客户区桌面画面失败");
    return image;
}

// Windows Graphics Capture returns the window frame. Restore client-only pixels.
inline QImage cropCapturedWindow(HWND hwnd, const QImage& frame, Geometry& result, QString& error)
{
    DpiScope dpi;
    if (!geometry(hwnd, result, error) || frame.isNull()) return {};
    if (frame.size() == result.size) return frame;

    RECT windowRect{};
    RECT extendedRect{};
    const bool hasWindow = GetWindowRect(hwnd, &windowRect) != FALSE;
    const bool hasExtended = SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
        &extendedRect, sizeof(extendedRect)));
    const auto candidate = [&](const RECT& rect) {
        return QRect(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
    };
    const QRect window = hasWindow ? candidate(windowRect) : QRect();
    const QRect extended = hasExtended ? candidate(extendedRect) : QRect();
    const auto difference = [&](const QRect& rect) {
        return qAbs(rect.width() - frame.width()) + qAbs(rect.height() - frame.height());
    };
    const QRect first = hasExtended && (!hasWindow || difference(extended) <= difference(window))
        ? extended : window;
    const QRect second = first == extended ? window : extended;
    for (const QRect& bounds : {first, second})
    {
        if (bounds.isEmpty()) continue;
        const QRect clientInFrame(result.screenOrigin - bounds.topLeft(), result.size);
        if (frame.rect().contains(clientInFrame)) return frame.copy(clientInFrame);
    }
    error = QStringLiteral("无遮挡截图的客户区边界不匹配");
    return {};
}
}
