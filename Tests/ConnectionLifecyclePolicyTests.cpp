#include <cassert>

#include "../Shared/ConnectionLifecyclePolicy.h"

int main()
{
    assert(ConnectionLifecyclePolicy::HeartbeatIntervalMs == 3000);
    assert(ConnectionLifecyclePolicy::ReconnectIntervalMs == 3000);

    assert(!ConnectionLifecyclePolicy::ShouldAutoDisconnectForHeartbeatMiss(1));
    assert(!ConnectionLifecyclePolicy::ShouldAutoDisconnectForHeartbeatMiss(3));
    assert(!ConnectionLifecyclePolicy::ShouldAutoDisconnectForHeartbeatMiss(100));

    assert(ConnectionLifecyclePolicy::ShouldAutoReconnect(false, true));
    assert(!ConnectionLifecyclePolicy::ShouldAutoReconnect(true, true));
    assert(!ConnectionLifecyclePolicy::ShouldAutoReconnect(false, false));

    return 0;
}
