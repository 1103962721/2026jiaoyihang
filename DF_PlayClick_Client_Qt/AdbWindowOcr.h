#pragma once

#include <windows.h>
#include <QImage>
#include <QRect>
#include <QSize>
#include <QString>
#include "BulletOcrRules.h"

// Only the external ADB boss uses this backend. Coordinates are physical client pixels.
namespace AdbWindowOcr
{
inline QSize referenceSize() { return QSize(737, 440); }
class DpiScope
{
public:
    DpiScope() : previous_(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
    ~DpiScope() { if (previous_) SetThreadDpiAwarenessContext(previous_); }
private:
    DPI_AWARENESS_CONTEXT previous_;
};
struct Geometry { QSize size; QPoint origin; };
inline bool geometry(HWND hwnd, Geometry& result, QString& error)
{
    error.clear();
    DpiScope dpi;
    RECT client{};
    POINT origin{};
    if (!hwnd || !IsWindow(hwnd) || !GetClientRect(hwnd, &client) || !ClientToScreen(hwnd, &origin))
    {
        error = QStringLiteral("未绑定有效云手机窗口，请重新绑定");
        return false;
    }
    result = { QSize(client.right - client.left, client.bottom - client.top), QPoint(origin.x, origin.y) };
    if (result.size.isEmpty())
    {
        error = QStringLiteral("云手机客户区大小为零，请恢复窗口");
        return false;
    }
    return true;
}
inline QImage capture(HWND hwnd, const QRect& referenceRegion, Geometry& g, QRect& actualRegion, QString& error)
{
    DpiScope dpi;
    if (!geometry(hwnd, g, error)) return {};
    if (!IsWindowVisible(hwnd) || IsIconic(GetAncestor(hwnd, GA_ROOT)))
    {
        error = QStringLiteral("云手机窗口不可见或已最小化，请恢复窗口");
        return {};
    }
    // A resized frame could place text outside the user's rectangles: wait for the size lock.
    if (g.size != referenceSize())
    {
        error = QStringLiteral("云手机客户区尚未固定为 737×440，当前 %1×%2").arg(g.size.width()).arg(g.size.height());
        return {};
    }
    actualRegion = referenceRegion.isNull() ? QRect(QPoint(), g.size)
        : ScaleReferenceRect(referenceRegion, referenceSize(), g.size);
    if (!IsRectInsideClient(actualRegion, g.size))
    {
        error = QStringLiteral("OCR 区域超出云手机客户区");
        return {};
    }
    const QRect desktop(GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
    if (!desktop.contains(QRect(g.origin, g.size)))
    {
        error = QStringLiteral("云手机客户区部分位于屏幕外，请移入屏幕");
        return {};
    }
    HDC screen = GetDC(nullptr);
    HDC memory = screen ? CreateCompatibleDC(screen) : nullptr;
    HBITMAP bitmap = memory ? CreateCompatibleBitmap(screen, actualRegion.width(), actualRegion.height()) : nullptr;
    QImage image;
    if (bitmap)
    {
        HGDIOBJ previous = SelectObject(memory, bitmap);
        const BOOL copied = BitBlt(memory, 0, 0, actualRegion.width(), actualRegion.height(), screen,
            g.origin.x() + actualRegion.x(), g.origin.y() + actualRegion.y(), SRCCOPY | CAPTUREBLT);
        SelectObject(memory, previous);
        if (copied)
        {
            image = QImage(actualRegion.size(), QImage::Format_RGB32);
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = actualRegion.width();
            info.bmiHeader.biHeight = -actualRegion.height();
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            if (image.isNull() || GetDIBits(memory, bitmap, 0, actualRegion.height(), image.bits(), &info,
                DIB_RGB_COLORS) != actualRegion.height()) image = {};
        }
        DeleteObject(bitmap);
    }
    if (memory) DeleteDC(memory);
    if (screen) ReleaseDC(nullptr, screen);
    if (image.isNull()) error = QStringLiteral("读取云手机桌面客户区截图失败");
    return image;
}

// External processes cannot be subclassed here. Remove standard resize controls and
// enforce custom window/layout resizing using an asynchronous size request on a timer.
class SizeLock
{
public:
    ~SizeLock() { release(); }
    bool bind(HWND target, QString& error)
    {
        const HWND root = GetAncestor(target, GA_ROOT);
        if (!root || root == GetDesktopWindow() || root == GetShellWindow())
        {
            error = QStringLiteral("请选择云手机窗口，不能绑定桌面");
            return false;
        }
        if (GetPropW(root, property()) && GetPropW(root, property()) != this)
        {
            error = QStringLiteral("该云手机窗口已由另一个 ADB 会话绑定");
            return false;
        }
        release();
        target_ = target;
        root_ = root;
        GetWindowThreadProcessId(root_, &pid_);
        originalBits_ = GetWindowLongPtrW(root_, GWL_STYLE) & ResizeBits;
        if (!SetPropW(root_, property(), this))
        {
            root_ = nullptr;
            error = QStringLiteral("无法锁定窗口，请检查进程权限");
            return false;
        }
        SetLastError(0);
        const LONG_PTR style = GetWindowLongPtrW(root_, GWL_STYLE);
        if (!SetWindowLongPtrW(root_, GWL_STYLE, style & ~ResizeBits) && GetLastError())
        {
            error = QStringLiteral("禁止窗口缩放失败，请检查进程权限");
            release();
            return false;
        }
        if (IsZoomed(root_) || IsIconic(root_)) ShowWindowAsync(root_, SW_RESTORE);
        framePending_ = true;
        if (!enforce())
        {
            error = QStringLiteral("设置云手机客户区尺寸失败");
            release();
            return false;
        }
        return true;
    }
    bool enforce()
    {
        DpiScope dpi;
        Geometry g;
        QString error;
        RECT outer{};
        if (!valid() || !IsWindow(target_) || GetAncestor(target_, GA_ROOT) != root_) return false;
        if (IsIconic(root_)) return true;
        if (IsZoomed(root_))
        {
            framePending_ = true;
            ShowWindowAsync(root_, SW_RESTORE);
            return true;
        }
        if (!geometry(target_, g, error) || !GetWindowRect(root_, &outer)) return false;
        const LONG_PTR style = GetWindowLongPtrW(root_, GWL_STYLE);
        if (style & ResizeBits)
        {
            SetLastError(0);
            if (!SetWindowLongPtrW(root_, GWL_STYLE, style & ~ResizeBits) && GetLastError()) return false;
            framePending_ = true;
        }
        if (!framePending_ && g.size == referenceSize())
        {
            return true;
        }
        // Slow/custom layouts must not exhaust a retry budget and unlock resizing.
        framePending_ = false;
        // Fractional DPI layouts may round a child to 738 instead of 737 physical
        // pixels. Correct that final pixel on the child without shifting its origin.
        if (target_ != root_ && qAbs(g.size.width() - 737) <= 1 && qAbs(g.size.height() - 440) <= 1)
        {
            RECT childOuter{};
            if (!GetWindowRect(target_, &childOuter)) return false;
            return SetWindowPos(target_, nullptr, 0, 0,
                childOuter.right - childOuter.left + 737 - g.size.width(),
                childOuter.bottom - childOuter.top + 440 - g.size.height(),
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS) != FALSE;
        }
        return SetWindowPos(root_, nullptr, 0, 0,
            outer.right - outer.left + 737 - g.size.width(),
            outer.bottom - outer.top + 440 - g.size.height(),
            SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_ASYNCWINDOWPOS) != FALSE;
    }
    void release()
    {
        if (valid() && GetPropW(root_, property()) == this)
        {
            const LONG_PTR style = GetWindowLongPtrW(root_, GWL_STYLE);
            SetWindowLongPtrW(root_, GWL_STYLE, (style & ~ResizeBits) | originalBits_);
            RemovePropW(root_, property());
            SetWindowPos(root_, nullptr, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_ASYNCWINDOWPOS);
        }
        root_ = target_ = nullptr;
    }
private:
    bool valid() const
    {
        DWORD pid = 0;
        return root_ && IsWindow(root_) && GetWindowThreadProcessId(root_, &pid) && pid == pid_;
    }
    static const wchar_t* property() { return L"DFPlayClick.AdbBossSizeLock"; }
    static constexpr LONG_PTR ResizeBits = WS_THICKFRAME | WS_MAXIMIZEBOX;
    HWND target_ = nullptr;
    HWND root_ = nullptr;
    DWORD pid_ = 0;
    LONG_PTR originalBits_ = 0;
    bool framePending_ = false;
};
}
