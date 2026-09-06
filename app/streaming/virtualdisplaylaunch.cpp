#include "virtualdisplaylaunch.h"

#include "settings/refreshrateparser.h"

#include <algorithm>
#include <thread>

namespace VirtualDisplayLaunchPolicy {

namespace {

int connectionDelayMs(int attemptIndex)
{
    const qint64 scaled = static_cast<qint64>(kVirtualDisplayBaseRetryDelayMs) *
                          (static_cast<qint64>(attemptIndex) + 1);
    if (scaled > kVirtualDisplayBaseRetryDelayMs * 2) {
        return kVirtualDisplayBaseRetryDelayMs * 2;
    }
    return static_cast<int>(scaled);
}

}

void ConnectionAttemptState::reset()
{
    m_HasStageFailure.store(false);
    m_HasConnectionTermination.store(false);
    m_Stage.store(-1);
    m_StageErrorCode.store(0);
}

bool ConnectionAttemptState::hasStageFailure() const
{
    return m_HasStageFailure.load();
}

int ConnectionAttemptState::stage() const
{
    return m_Stage.load();
}

int ConnectionAttemptState::stageErrorCode() const
{
    return m_StageErrorCode.load();
}

bool ConnectionAttemptState::hasConnectionTermination() const
{
    return m_HasConnectionTermination.load();
}

void ConnectionAttemptState::setStageFailure(int stage, int errorCode)
{
    m_Stage.store(stage);
    m_StageErrorCode.store(errorCode);
    m_HasStageFailure.store(true);
}

void ConnectionAttemptState::setConnectionTermination()
{
    m_HasConnectionTermination.store(true);
}

bool resolveEffectiveSops(bool useVirtualDisplay,
                          bool isNvidiaServerSoftware,
                          bool hostSupportsResolution,
                          bool userGameOptimizations)
{
    if (isNvidiaServerSoftware && !hostSupportsResolution) {
        return false;
    }
    if (useVirtualDisplay && !isNvidiaServerSoftware) {
        return true;
    }
    return userGameOptimizations;
}

int maxConnectionAttempts(bool useVirtualDisplay, bool isNvidiaServerSoftware)
{
    return useVirtualDisplay && !isNvidiaServerSoftware
        ? kVirtualDisplayConnectionAttempts
        : 1;
}

bool runConnectionAttempts(int maxAttempts,
                           ConnectionAttemptState& state,
                           ConnectionCancellation& cancellation,
                           const ConnectionRetryOperations& operations,
                           const ConnectionRetryCallbacks& callbacks)
{
    const int boundedAttempts = std::max(1, maxAttempts);

    if (callbacks.preparing) {
        callbacks.preparing();
    }
    if (operations.launch && !operations.launch()) {
        if (callbacks.launchFailed) {
            callbacks.launchFailed();
        }
        return false;
    }
    if (cancellation.isCancelled()) {
        if (callbacks.cancelled) {
            callbacks.cancelled();
        }
        return false;
    }

    for (int attempt = 0; attempt < boundedAttempts; ++attempt) {
        state.reset();
        const int returnCode = operations.connect(attempt);
        if (returnCode == 0 && !state.hasConnectionTermination()) {
            return true;
        }
        if (cancellation.isCancelled()) {
            if (callbacks.cancelled) {
                callbacks.cancelled();
            }
            return false;
        }

        if (attempt + 1 >= boundedAttempts) {
            if (state.hasStageFailure() && callbacks.finalStageFailure) {
                callbacks.finalStageFailure(state.stage(), state.stageErrorCode());
            }
            else if (callbacks.finalGenericFailure) {
                callbacks.finalGenericFailure();
            }
            return false;
        }

        int delayMs = connectionDelayMs(attempt);
        while (delayMs > 0 && !cancellation.isCancelled()) {
            const int delaySliceMs = std::min(
                delayMs, kVirtualDisplayRetryDelaySliceMs);
            if (operations.wait) {
                operations.wait(delaySliceMs);
            }
            else {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(delaySliceMs));
            }
            delayMs -= delaySliceMs;
        }

        if (cancellation.isCancelled()) {
            if (callbacks.cancelled) {
                callbacks.cancelled();
            }
            return false;
        }
    }

    return false;
}

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
    double detectedRefreshHz)
{
    DeckDisplayResolution resolution = {
        false,
        savedWidth,
        savedHeight,
        savedProtocolFps
    };

    if (!settingEnabled || !useVirtualDisplay || !isSteamDeck || !isGamingMode ||
            detectedWidth <= 0 || detectedHeight <= 0) {
        return resolution;
    }

    int protocolFps = 0;
    if (!RefreshRateParser::toMilliHz(detectedRefreshHz, &protocolFps)) {
        return resolution;
    }

    resolution.applied = true;
    resolution.width = detectedWidth;
    resolution.height = detectedHeight;
    resolution.protocolFps = protocolFps;
    return resolution;
}

bool explicitHdrCanStart(bool hdrEnabled,
                        bool clientSupports10Bit,
                        bool hostSupports10Bit)
{
    return !hdrEnabled || (clientSupports10Bit && hostSupports10Bit);
}

bool isRecoverableTermination(int errorCode)
{
    return errorCode != 0;
}

bool canResumeExistingHostSession(int expectedAppId, int currentGameId)
{
    return expectedAppId > 0 && expectedAppId == currentGameId;
}

const char* rtspSessionUrlFromStorage(const QByteArray& storage)
{
    return storage.isEmpty() ? nullptr : storage.constData();
}

bool shouldSurfaceRecovery(int errorCode,
                           bool intentionalLocal,
                           bool connectionStarted)
{
    return errorCode != 0 && !intentionalLocal && connectionStarted;
}

bool shouldSurfaceError(int errorCode,
                        bool intentionalLocal,
                        bool connectionStarted)
{
    return errorCode != 0 && !intentionalLocal && !connectionStarted;
}

}
