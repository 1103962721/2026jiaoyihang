#include "WindowBackgroundInput.h"

#include <QRect>

namespace WindowBackgroundInput
{
namespace
{
bool send(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, Mode mode, QString& error)
{
    if (!IsWindow(hwnd))
    {
        error = QStringLiteral("后台输入目标窗口已失效");
        return false;
    }
    if (mode == Mode::Post)
    {
        if (PostMessageW(hwnd, message, wParam, lParam)) return true;
    }
    else
    {
        DWORD_PTR result = 0;
        if (SendMessageTimeoutW(hwnd, message, wParam, lParam,
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 400, &result)) return true;
    }
    error = QStringLiteral("后台窗口消息发送失败或目标无响应（错误 %1）").arg(GetLastError());
    return false;
}

HWND childAt(HWND hwnd, POINT& point)
{
    HWND target = hwnd;
    for (int depth = 0; depth < 8; ++depth)
    {
        HWND child = ChildWindowFromPointEx(target, point, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED);
        if (!child || child == target) break;
        MapWindowPoints(target, child, &point, 1);
        target = child;
    }
    return target;
}

HWND keyboardTarget(HWND hwnd)
{
    GUITHREADINFO info{sizeof(info)};
    const DWORD thread = GetWindowThreadProcessId(hwnd, nullptr);
    if (thread && GetGUIThreadInfo(thread, &info) && info.hwndFocus
        && (info.hwndFocus == hwnd || IsChild(hwnd, info.hwndFocus)))
        return info.hwndFocus;
    return hwnd;
}

LPARAM keyFlags(WORD virtualKey, bool released)
{
    const auto scanCode = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
    LPARAM flags = 1 | (static_cast<LPARAM>(scanCode & 0xff) << 16);
    if (virtualKey == VK_LEFT || virtualKey == VK_RIGHT || virtualKey == VK_UP || virtualKey == VK_DOWN
        || virtualKey == VK_HOME || virtualKey == VK_END || virtualKey == VK_INSERT || virtualKey == VK_DELETE
        || virtualKey == VK_PRIOR || virtualKey == VK_NEXT)
        flags |= 1L << 24;
    if (released) flags |= (1L << 30) | (1L << 31);
    return flags;
}
}

bool click(HWND hwnd, QPoint clientPoint, Mode mode, QString& error)
{
    error.clear();
    RECT client{};
    if (!IsWindow(hwnd) || !GetClientRect(hwnd, &client)
        || !QRect(0, 0, client.right - client.left, client.bottom - client.top).contains(clientPoint))
    {
        error = QStringLiteral("后台点击坐标超出目标客户区，或窗口已失效");
        return false;
    }
    POINT local{clientPoint.x(), clientPoint.y()};
    const HWND target = childAt(hwnd, local);
    const LPARAM position = MAKELPARAM(static_cast<WORD>(local.x), static_cast<WORD>(local.y));
    return send(target, WM_MOUSEMOVE, 0, position, mode, error)
        && send(target, WM_LBUTTONDOWN, MK_LBUTTON, position, mode, error)
        && send(target, WM_LBUTTONUP, 0, position, mode, error);
}

bool key(HWND hwnd, WORD virtualKey, Mode mode, QString& error)
{
    error.clear();
    if (!virtualKey) { error = QStringLiteral("请选择有效按键"); return false; }
    const HWND target = keyboardTarget(hwnd);
    return send(target, WM_KEYDOWN, virtualKey, keyFlags(virtualKey, false), mode, error)
        && send(target, WM_KEYUP, virtualKey, keyFlags(virtualKey, true), mode, error);
}

bool text(HWND hwnd, const QString& value, Mode mode, QString& error)
{
    error.clear();
    const HWND target = keyboardTarget(hwnd);
    for (const QChar character : value)
    {
        if (!send(target, WM_CHAR, character.unicode(), 1, mode, error)) return false;
    }
    return true;
}
}
