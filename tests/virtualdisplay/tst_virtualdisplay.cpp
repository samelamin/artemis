#include <QtTest>
#include <QFile>
#include <QString>

#include "streaming/virtualdisplaylaunch.h"

using VirtualDisplayLaunchPolicy::resolveEffectiveSops;
using VirtualDisplayLaunchPolicy::maxConnectionAttempts;
using VirtualDisplayLaunchPolicy::connectionRetryDelayMs;
using VirtualDisplayLaunchPolicy::kVirtualDisplayConnectionAttempts;
using VirtualDisplayLaunchPolicy::kVirtualDisplayBaseRetryDelayMs;
using VirtualDisplayLaunchPolicy::kMaxVirtualDisplayRetryDelayMs;

class VirtualDisplayLaunchTest : public QObject
{
    Q_OBJECT

private slots:
    // Pure-policy helpers used by production Session.
    void resolveEffectiveSopsForcesSopsWhenVirtualDisplayEnabled();
    void resolveEffectiveSopsPreservesUserPreferenceForNormalSessions();
    void maxConnectionAttemptsIsOneForNormalAndBoundedForVirtualDisplay();
    void connectionRetryDelayMsGrowsLinearlyAndCapsAtMaximum();
    void connectionRetryDelayMsHandlesNegativeAttemptIndex();

    // Source-contract: NvHTTP::startApp must occur exactly once in
    // startConnectionAsync() and outside the LiStartConnection retry loop.
    // Reissuing /launch (or /resume) could duplicate or restart Steam on the
    // host, so this is the property the bounded retry is designed to protect.
    void startAppIsIssuedExactlyOnceAndOutsideTheRetryLoop();
};

void VirtualDisplayLaunchTest::resolveEffectiveSopsForcesSopsWhenVirtualDisplayEnabled()
{
    // Bug: virtual display requires sops=1 even when the user has
    // gameOptimizations disabled. The helper must override.
    QCOMPARE(resolveEffectiveSops(true, false), true);
    QCOMPARE(resolveEffectiveSops(true, true), true);
}

void VirtualDisplayLaunchTest::resolveEffectiveSopsPreservesUserPreferenceForNormalSessions()
{
    // Plain (non-virtual) session: SOPS reflects the user preference and must
    // never mutate it across many invocations.
    QCOMPARE(resolveEffectiveSops(false, true), true);
    QCOMPARE(resolveEffectiveSops(false, false), false);
    QCOMPARE(resolveEffectiveSops(false, false), false);
    QCOMPARE(resolveEffectiveSops(false, true), true);
}

void VirtualDisplayLaunchTest::maxConnectionAttemptsIsOneForNormalAndBoundedForVirtualDisplay()
{
    QCOMPARE(maxConnectionAttempts(false), 1);
    const int vd = maxConnectionAttempts(true);
    QVERIFY2(vd >= 2,
             "Virtual display needs at least two attempts so a cold host "
             "creation can be retried before giving up");
    QVERIFY2(vd <= kVirtualDisplayConnectionAttempts,
             "Attempts must be bounded by the documented retry count");
    // Deterministic: must not change between calls within a session.
    QCOMPARE(maxConnectionAttempts(true), vd);
    QCOMPARE(maxConnectionAttempts(false), 1);
}

void VirtualDisplayLaunchTest::connectionRetryDelayMsGrowsLinearlyAndCapsAtMaximum()
{
    QVERIFY(connectionRetryDelayMs(0) >= kVirtualDisplayBaseRetryDelayMs);
    QVERIFY(connectionRetryDelayMs(1) > connectionRetryDelayMs(0));
    QVERIFY(connectionRetryDelayMs(2) > connectionRetryDelayMs(1));
    // Cap kicks in well before absurd attempt indexes stall the UI.
    QCOMPARE(connectionRetryDelayMs(1000), kMaxVirtualDisplayRetryDelayMs);
    // Sweep across the bounded retry window to ensure we never exceed the cap.
    for (int i = 0; i < kVirtualDisplayConnectionAttempts + 4; ++i) {
        QVERIFY(connectionRetryDelayMs(i) <= kMaxVirtualDisplayRetryDelayMs);
    }
}

void VirtualDisplayLaunchTest::connectionRetryDelayMsHandlesNegativeAttemptIndex()
{
    QCOMPARE(connectionRetryDelayMs(-1), 0);
    QCOMPARE(connectionRetryDelayMs(-100), 0);
}

void VirtualDisplayLaunchTest::startAppIsIssuedExactlyOnceAndOutsideTheRetryLoop()
{
    // Resolve session.cpp relative to the test's source location so the
    // source-contract holds regardless of the build directory layout.
    const QString sessionPath = QStringLiteral(SESSION_CPP_SOURCE_PATH);
    QFile sessionFile(sessionPath);
    QVERIFY2(sessionFile.open(QIODevice::ReadOnly | QIODevice::Text),
             qPrintable(QString("Cannot open %1: %2")
                        .arg(sessionPath, sessionFile.errorString())));
    const QString source = QString::fromUtf8(sessionFile.readAll());
    sessionFile.close();

    // Bound the source under test to Session::startConnectionAsync() so we
    // do not assert about unrelated code. Slice runs to the next top-level
    // member function (flushWindowEvents) or, defensively, to EOF.
    const QString startMarker = QStringLiteral("bool Session::startConnectionAsync()");
    const int startIdx = source.indexOf(startMarker);
    QVERIFY2(startIdx >= 0, "Session::startConnectionAsync() must be defined");
    const int endIdx = source.indexOf("void Session::flushWindowEvents()",
                                      startIdx + startMarker.size());
    const QString slice = source.mid(startIdx,
                                      endIdx >= 0 ? endIdx - startIdx : -1);

    // The /launch (or /resume) request must occur exactly once in this function.
    QCOMPARE(slice.count("http.startApp("), 1);

    const int startAppIdx    = slice.indexOf("http.startApp(");
    const int maxAttemptsIdx = slice.indexOf("VirtualDisplayLaunchPolicy::maxConnectionAttempts(");
    const int retryLoopIdx   = slice.indexOf("for (int attempt =");
    const int liStartIdx     = slice.indexOf("LiStartConnection(");
    QVERIFY2(startAppIdx    >= 0, "http.startApp( must appear in startConnectionAsync()");
    QVERIFY2(maxAttemptsIdx >= 0, "maxConnectionAttempts(...) must be referenced");
    QVERIFY2(retryLoopIdx   >= 0, "Bounded retry for-loop must be present");
    QVERIFY2(liStartIdx     >= 0, "LiStartConnection( must appear");

    // Ordering: /launch precedes the retry loop, which precedes
    // LiStartConnection; maxConnectionAttempts(...) is evaluated first.
    QVERIFY2(startAppIdx    < retryLoopIdx, "http.startApp( must precede the retry loop");
    QVERIFY2(retryLoopIdx   < liStartIdx,   "retry loop must precede LiStartConnection(");
    QVERIFY2(maxAttemptsIdx < retryLoopIdx, "maxConnectionAttempts(...) must precede the retry loop");

    // No /launch reissue occurs inside the loop slice (for-loop header up
    // to the first LiStartConnection( call) — exactly what the bounded
    // retry is designed to protect.
    const QString loopSlice = slice.mid(retryLoopIdx, liStartIdx - retryLoopIdx);
    QCOMPARE(loopSlice.count("http.startApp("), 0);
}

QTEST_GUILESS_MAIN(VirtualDisplayLaunchTest)

#include "tst_virtualdisplay.moc"
