#pragma once

inline bool ShouldClickImmediatelyForCountdown(int seconds)
{
    return seconds <= 0;
}
