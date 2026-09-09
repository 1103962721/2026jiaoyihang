#pragma once

#include <windows.h>
#include <cstdint>
#include <cwchar>
#include <string>

inline std::wstring FormatHwndLow8Hex(HWND hwnd)
{
    const wchar_t hex[] = L"0123456789ABCDEF";
    unsigned int value = static_cast<unsigned int>(reinterpret_cast<uintptr_t>(hwnd) & 0xFFFFFFFFu);
    std::wstring text(8, L'0');
    for (int index = 7; index >= 0; --index)
    {
        text[index] = hex[value & 0xFu];
        value >>= 4;
    }
    return text;
}

inline bool IsDeltaForceGameBodyProcessName(const wchar_t* processName)
{
    if (!processName)
    {
        return false;
    }

    // 游戏本体进程名以实际 Shipping 可执行文件为准，保留旧名称兼容测试环境。
    return lstrcmpiW(processName, L"DeltaForceClient-Win64-Shipping.exe") == 0
        || lstrcmpiW(processName, L"DeltaForceClient.exe") == 0;
}

inline bool IsDeltaForceGameBodyTitle(const wchar_t* title)
{
    if (!title)
    {
        return false;
    }

    while (*title == L' ' || *title == L'\t')
    {
        ++title;
    }

    const wchar_t expected[] = L"三角洲行动";
    size_t index = 0;
    while (expected[index] != L'\0')
    {
        if (title[index] != expected[index])
        {
            return false;
        }
        ++index;
    }

    const wchar_t* rest = title + index;
    while (*rest == L' ' || *rest == L'\t')
    {
        ++rest;
    }
    return *rest == L'\0';
}

inline bool TryGetAutoClickPoint(int hwndClientWidth, int hwndClientHeight, POINT& point)
{
    if (hwndClientWidth <= 0 || hwndClientHeight <= 0)
    {
        return false;
    }

    constexpr int ReferenceWidth = 2560;
    constexpr int ReferenceHeight = 1440;
    constexpr int ReferenceX = 2313;
    constexpr int ReferenceY = 1287;
    // 自动点击点以 2560x1440 客户区为参考，按当前绑定窗口客户区比例缩放。
    point.x = ReferenceX * hwndClientWidth / ReferenceWidth;
    point.y = ReferenceY * hwndClientHeight / ReferenceHeight;
    return true;
}
