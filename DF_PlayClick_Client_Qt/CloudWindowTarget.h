#pragma once
#include "Win32WindowBinder.h"
#include "../Shared/ClientBindingRules.h"

namespace CloudWindowTarget
{
inline bool isSurface(HWND hwnd)
{
    wchar_t title[256]{}, name[128]{};
    GetWindowTextW(hwnd, title, 256);
    GetClassNameW(hwnd, name, 128);
    return IsCloudPhoneWindowTitle(title) || lstrcmpiW(name, L"PlayUI") == 0;
}
// Prefer the actual phone surface within EACH top-level window, not globally.
inline QVector<TargetWindowInfo> surfaces(const QVector<TargetWindowInfo>& windows)
{
    QVector<TargetWindowInfo> result;
    for (const auto& item : windows)
    {
        if (!IsWindow(item.hwnd) || item.hwnd == GetDesktopWindow() || item.hwnd == GetShellWindow()) continue;
        if (GetAncestor(item.hwnd, GA_ROOT) != item.hwnd && !isSurface(item.hwnd)) continue;
        if (item.className == QStringLiteral("DFPlayClick.PickerOutline")) continue;
        bool hasSurfaceChild = false;
        for (const auto& child : windows)
            if (child.hwnd != item.hwnd && IsChild(item.hwnd, child.hwnd) && isSurface(child.hwnd))
            { hasSurfaceChild = true; break; }
        if (!hasSurfaceChild) result.append(item);
    }
    return result;
}
inline HWND resolve(HWND hit, HWND excluded, const QVector<TargetWindowInfo>& windows)
{
    if (!hit || !IsWindow(hit)) return nullptr;
    const HWND root = GetAncestor(hit, GA_ROOT);
    if (!root || root == GetAncestor(excluded, GA_ROOT) || root == GetDesktopWindow() || root == GetShellWindow())
        return nullptr;
    const auto candidates = surfaces(windows);
    for (HWND ancestor = hit; ancestor; ancestor = GetParent(ancestor))
    {
        for (const auto& item : candidates)
            if (item.hwnd == ancestor) return ancestor;
        if (ancestor == root) break;
    }
    // A title-bar drop is unambiguous only when this frame has one surface.
    HWND selected = nullptr;
    for (const auto& item : candidates)
    {
        if (GetAncestor(item.hwnd, GA_ROOT) != root) continue;
        if (selected) return nullptr;
        selected = item.hwnd;
    }
    return selected;
}
}
