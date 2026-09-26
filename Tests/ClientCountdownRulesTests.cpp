#include <cassert>
#include "../Shared/ClientCountdownRules.h"

int main()
{
    assert(ShouldClickImmediatelyForCountdown(0));
    assert(!ShouldClickImmediatelyForCountdown(1));
    assert(!ShouldClickImmediatelyForCountdown(5));

    return 0;
}
