#pragma once

namespace ConnectionLifecyclePolicy
{
    constexpr int HeartbeatIntervalMs = 3000;
    constexpr int ConnectTimeoutMs = 2000;
    constexpr int ReconnectInitialIntervalMs = 5000;
    constexpr int ReconnectMaxIntervalMs = 60000;

    inline int ReconnectDelayMs(int attempt)
    {
        int delayMs = ReconnectInitialIntervalMs;
        for (int index = 0; index < attempt && delayMs < ReconnectMaxIntervalMs; ++index)
        {
            delayMs = delayMs > ReconnectMaxIntervalMs / 2
                ? ReconnectMaxIntervalMs
                : delayMs * 2;
        }
        return delayMs;
    }

    inline bool ShouldAutoDisconnectForHeartbeatMiss(int missedPongCount)
    {
        (void)missedPongCount;
        return false;
    }

    inline bool ShouldAutoReconnect(bool userRequestedDisconnect, bool hasServerEndpoint)
    {
        return !userRequestedDisconnect && hasServerEndpoint;
    }
}
