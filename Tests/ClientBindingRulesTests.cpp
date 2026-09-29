#include <cassert>
#include <cstdint>
#include <windows.h>
#include "../Shared/ClientBindingRules.h"

int main()
{
    struct Case
    {
        int width;
        int height;
        int x;
        int y;
    };

    const Case cases[] = {
        {2560, 1440, 2313, 1287},
        {2048, 1152, 1850, 1029},
        {1920, 1440, 1734, 1287},
        {1920, 1200, 1734, 1072},
        {1920, 1080, 1734, 965},
        {1680, 1050, 1517, 938},
        {1600, 1200, 1445, 1072},
        {1600, 1024, 1445, 915},
        {1600, 900, 1445, 804},
        {1440, 1080, 1301, 965},
        {1366, 768, 1234, 686},
        {1360, 768, 1228, 686},
        {1280, 1024, 1156, 915},
        {1280, 960, 1156, 858},
        {1280, 800, 1156, 715},
        {1280, 768, 1156, 686},
        {1024, 768, 925, 686},
        {1111, 777, 1003, 694},
    };

    for (const auto& item : cases)
    {
        POINT point{};
        assert(TryGetAutoClickPoint(item.width, item.height, point));
        assert(point.x == item.x);
        assert(point.y == item.y);
    }

    POINT point{ 11, 22 };
    assert(!TryGetAutoClickPoint(0, 777, point));
    assert(point.x == 11);
    assert(point.y == 22);

    assert(IsDeltaForceGameBodyProcessName(L"DeltaForceClient-Win64-Shipping.exe"));
    assert(IsDeltaForceGameBodyProcessName(L"eltaForceClient-Win64-Shipping.exe"));
    assert(IsDeltaForceGameBodyProcessName(L"DeltaForceClient.exe"));
    assert(!IsDeltaForceGameBodyProcessName(L"UnrealCEFSubProcess.exe"));

    assert(IsDeltaForceGameBodyTitle(L"三角洲行动  "));
    assert(IsDeltaForceGameBodyTitle(L" 三角洲行动"));
    assert(!IsDeltaForceGameBodyTitle(L"三角洲行动启动器"));

    assert(FormatHwndLow8Hex(reinterpret_cast<HWND>(static_cast<uintptr_t>(0x120030))) == L"00120030");
    assert(FormatHwndLow8Hex(reinterpret_cast<HWND>(static_cast<uintptr_t>(0x123456789ABCDEF0))) == L"9ABCDEF0");

    return 0;
}
