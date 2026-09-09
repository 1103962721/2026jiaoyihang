#include <cassert>
#include "../DF_PlayClick_Server/ServerRules.h"

int main()
{
    assert(ParseCountdownSeconds(L"5") == 5);
    assert(ParseCountdownSeconds(L"12") == 12);
    assert(ParseCountdownSeconds(L" 8 ") == 8);

    assert(ParseCountdownSeconds(L"0") == 0);
    assert(ParseCountdownSeconds(L" 0 ") == 0);

    assert(ParseCountdownSeconds(nullptr) == -1);
    assert(ParseCountdownSeconds(L"") == -1);
    assert(ParseCountdownSeconds(L"-1") == -1);
    assert(ParseCountdownSeconds(L"abc") == -1);
    assert(ParseCountdownSeconds(L"5abc") == -1);

    return 0;
}
