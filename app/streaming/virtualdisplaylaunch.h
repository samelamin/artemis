#pragma once

#include <atomic>
#include <functional>

namespace VirtualDisplayLaunchPolicy {

static const int kVirtualDisplayConnectionAttempts = 3;
static const int kVirtualDisplayBaseRetryDelayMs = 2000;
static const int kVirtualDisplayRetryDelaySliceMs = 50;

class ConnectionAttemptState
{
public:
    ConnectionAttemptState() = default;

    void reset();

    bool hasStageFailure() const;
    int stage() const;
    int stageErrorCode() const;
    bool hasConnectionTermination() const;

    void setStageFailure(int stage, int errorCode);
    void setConnectionTermination();

private:
    std::atomic<bool> m_HasStageFailure{false};
    std::atomic<bool> m_HasConnectionTermination{false};
    std::atomic<int> m_Stage{-1};
    std::atomic<int> m_StageErrorCode{0};
};

// Resettable cancellation flag for a connection attempt.
//
// A naked std::atomic<bool> is easy to misuse: an accidental store(false)
// anywhere after an attempt begins will erase a concurrent user cancel and
// leave a partially-cancelled attempt running. This wrapper makes the
// lifecycle explicit at the call site:
//
//   - reset()   marks the *start* of an authorized attempt. Callers MUST
//               invoke this before any callbacks can observe cancellation.
//   - cancel()  requests cancellation of the attempt that is currently
//               in flight (or about to begin). Safe to call concurrently
//               from any thread.
//   - isCancelled() returns true once a cancel has been requested and not
//               since reset.
//
// The flag is monotonic between reset() and the end of the attempt: it
// only ever transitions false -> true. After the attempt finishes the
// caller is expected to reset() before kicking off a new attempt.
class ConnectionCancellation
{
public:
    ConnectionCancellation() = default;

    void reset()
    {
        m_Cancelled.store(false, std::memory_order_release);
    }

    void cancel()
    {
        m_Cancelled.store(true, std::memory_order_release);
    }

    bool isCancelled() const
    {
        return m_Cancelled.load(std::memory_order_acquire);
    }

private:
    std::atomic<bool> m_Cancelled{false};
};

struct ConnectionRetryOperations
{
    std::function<bool()> launch;
    std::function<int(int attemptIndex)> connect;
    std::function<void(int delaySliceMs)> wait;
};

struct ConnectionRetryCallbacks
{
    std::function<void()> preparing;
    std::function<void()> launchFailed;
    std::function<void(int stage, int errorCode)> finalStageFailure;
    std::function<void()> finalGenericFailure;
    std::function<void()> cancelled;
};

struct DeckDisplayResolution
{
    bool applied;
    int width;
    int height;
    int protocolFps;
};

bool resolveEffectiveSops(bool useVirtualDisplay,
                          bool isNvidiaServerSoftware,
                          bool hostSupportsResolution,
                          bool userGameOptimizations);
int maxConnectionAttempts(bool useVirtualDisplay, bool isNvidiaServerSoftware);
bool runConnectionAttempts(int maxAttempts,
                           ConnectionAttemptState& state,
                           ConnectionCancellation& cancellation,
                           const ConnectionRetryOperations& operations,
                           const ConnectionRetryCallbacks& callbacks);
DeckDisplayResolution resolveSteamDeckNativeDisplay(
    bool settingEnabled,
    bool useVirtualDisplay,
    bool isSteamDeck,
    bool isGamingMode,
    int savedWidth,
    int savedHeight,
    int savedProtocolFps,
    int detectedWidth,
    int detectedHeight,
    double detectedRefreshHz);
bool explicitHdrCanStart(bool hdrEnabled,
                        bool clientSupports10Bit,
                        bool hostSupports10Bit);
bool isRecoverableTermination(int errorCode);
bool canResumeExistingHostSession(int expectedAppId, int currentGameId);

bool shouldSurfaceRecovery(int errorCode,
                           bool intentionalLocal,
                           bool connectionStarted);
bool shouldSurfaceError(int errorCode,
                        bool intentionalLocal,
                        bool connectionStarted);

// Pure retry policy shared between the connection and audio renderers.
// Returns the attempt number that succeeded (1-based) or 0 if every attempt
// failed. waitFn is invoked between attempts (never after the last).
template<typename TryFn, typename WaitFn>
int retryWithFixedDelay(int maxAttempts, TryFn tryFn, WaitFn waitFn)
{
    for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
        if (tryFn(attempt)) {
            return attempt;
        }
        if (attempt < maxAttempts) {
            waitFn();
        }
    }
    return 0;
}

}
