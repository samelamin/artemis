#include "virtualdisplaylaunch.h"

namespace VirtualDisplayLaunchPolicy {

bool resolveEffectiveSops(bool useVirtualDisplay, bool userGameOptimizations)
{
    // Apollo/Vibepollo requires sops=1 for the host to actually resize to the
    // selected virtual display resolution. We override without touching the
    // user's saved gameOptimizations preference.
    return useVirtualDisplay ? true : userGameOptimizations;
}

int maxConnectionAttempts(bool useVirtualDisplay)
{
    return useVirtualDisplay ? kVirtualDisplayConnectionAttempts : 1;
}

int connectionRetryDelayMs(int attemptIndex)
{
    if (attemptIndex < 0) {
        return 0;
    }
    const qint64 scaled = static_cast<qint64>(kVirtualDisplayBaseRetryDelayMs) *
                          (static_cast<qint64>(attemptIndex) + 1);
    if (scaled > kMaxVirtualDisplayRetryDelayMs) {
        return kMaxVirtualDisplayRetryDelayMs;
    }
    return static_cast<int>(scaled);
}

}
