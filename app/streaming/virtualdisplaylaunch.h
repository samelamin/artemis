#pragma once

#include <QtGlobal>

namespace VirtualDisplayLaunchPolicy {

// Bounded retry budget for cold virtual-display creation on the host. Picked
// large enough to cover a typical Sunshine/Vibepollo virtual display warmup
// without making the user wait too long if the host genuinely fails.
static const int kVirtualDisplayConnectionAttempts = 3;

// Initial backoff (ms) before the first retry. Each subsequent attempt waits a
// bit longer, capped at kMaxVirtualDisplayRetryDelayMs.
static const int kVirtualDisplayBaseRetryDelayMs = 2000;
static const int kMaxVirtualDisplayRetryDelayMs = 8000;

// Resolves the effective SOPS value that should be sent on the /launch or
// /resume request. Apollo/Vibepollo requires sops=1 to actually change the
// host's display resolution when virtual display is enabled, even if the user
// has the "gameOptimizations" preference disabled. The user's saved preference
// is never mutated; only the launch payload is forced.
bool resolveEffectiveSops(bool useVirtualDisplay, bool userGameOptimizations);

// Returns the maximum number of LiStartConnection attempts allowed for the
// session. Cold virtual-display creation on the host can exceed the readiness
// window of the first attempt, so virtual display sessions receive bounded
// retries. Non-virtual sessions keep the prior single-attempt behavior.
int maxConnectionAttempts(bool useVirtualDisplay);

// Returns the delay in milliseconds before attempting a retry. attemptIndex is
// zero-based and refers to the attempt that just failed. Linear backoff bounded
// by kMaxVirtualDisplayRetryDelayMs to avoid long stalls while still giving the
// host time to finish creating the virtual display.
int connectionRetryDelayMs(int attemptIndex);

}
