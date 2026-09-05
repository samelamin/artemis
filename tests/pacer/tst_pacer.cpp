#include <QtTest>

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QMutex>
#include <QSemaphore>
#include <QThread>
#include <QWaitCondition>

#include <atomic>
#include <memory>
#include <thread>

#include "SDL_compat.h"
#include "streaming/streamutils.h"
#include "streaming/video/decoder.h"
#include "streaming/video/ffmpeg-renderers/pacer/pacer.h"
#include "streaming/video/ffmpeg-renderers/renderer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/mem.h>
}

namespace {

int g_NextFakeDisplayFps = 60;

}

int StreamUtils::getDisplayRefreshRate(SDL_Window*)
{
    return g_NextFakeDisplayFps;
}

class FakeVsyncSource : public IVsyncSource
{
public:
    bool initialize(SDL_Window*, int) override { return true; }
    bool isAsync() override { return true; }
    void waitForVsync() override {}

    Pacer* pacer() { return m_Pacer; }
    void setPacer(Pacer* p) { m_Pacer = p; }

private:
    Pacer* m_Pacer = nullptr;
};

class FakeRenderer : public IFFmpegRenderer
{
public:
    FakeRenderer() : IFFmpegRenderer(RendererType::Unknown) {}

    bool initialize(PDECODER_PARAMETERS) override { return true; }
    bool prepareDecoderContext(AVCodecContext*, AVDictionary**) override { return true; }
    void renderFrame(AVFrame*) override
    {
        QMutexLocker lk(&m_Mutex);
        m_Rendered++;
    }
    void notifyOverlayUpdated(Overlay::OverlayType) override {}

    bool isRenderThreadSupported() override { return m_RenderThreadSupported; }
    void setRenderThreadSupported(bool v) { m_RenderThreadSupported = v; }
    void cleanupRenderContext() override
    {
        QMutexLocker lk(&m_Mutex);
        m_CleanupCount++;
    }

    void waitToRender() override
    {
        QMutexLocker lk(&m_Mutex);
        m_WaitEnteredCount++;
        m_WaitEntered.release();
    }

    int renderedCount()
    {
        QMutexLocker lk(&m_Mutex);
        return m_Rendered;
    }
    int cleanupCount()
    {
        QMutexLocker lk(&m_Mutex);
        return m_CleanupCount;
    }
    QSemaphore& waitEnteredSemaphore() { return m_WaitEntered; }

private:
    QMutex m_Mutex;
    int m_Rendered = 0;
    int m_CleanupCount = 0;
    int m_WaitEnteredCount = 0;
    bool m_RenderThreadSupported = true;
    QSemaphore m_WaitEntered{0};
};

class PacerTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void earlySignalObservedBeforeWait();
    void coalescesRepeatedSignalsBeforeWait();
    void waitBoundsSpuriousWakesByDeadline();
    void waitReturnsFalseAfterStop();

    void handleVsyncEmptyQueueBailsOnStop();
    void handleVsyncStopGuardRunsEvenWithQueuedFrames();
    void handleVsyncSpuriousWakeWithoutStopTimesOut();

    void vsyncThreadFiresPacingWithoutCallback();
    void destructorFreesQueuedFramesAndJoins();
    void renderThreadShutdownWithEmptyQueuesIsBounded();

private:
    VIDEO_STATS m_Stats{};
    std::atomic<int> m_BufferFreeCount{0};

    AVFrame* makeTrackedFrame()
    {
        AVFrame* f = av_frame_alloc();
        uint8_t* data = static_cast<uint8_t*>(av_malloc(16));
        AVBufferRef* buf = av_buffer_create(data, 16,
            [](void* opaque, uint8_t* d) {
                auto* counter = static_cast<std::atomic<int>*>(opaque);
                counter->fetch_add(1, std::memory_order_relaxed);
                av_free(d);
            },
            &m_BufferFreeCount, 0);
        f->buf[0] = buf;
        return f;
    }
};

void PacerTest::initTestCase()
{
}

void PacerTest::cleanupTestCase()
{
}

void PacerTest::earlySignalObservedBeforeWait()
{
    FakeRenderer renderer;
    auto p = std::make_unique<Pacer>(&renderer, &m_Stats);

    p->signalVsync();
    QVERIFY2(p->waitForAsyncVsync(0),
             "early signalVsync() must be observed even with zero-timeout wait");
}

void PacerTest::coalescesRepeatedSignalsBeforeWait()
{
    FakeRenderer renderer;
    auto p = std::make_unique<Pacer>(&renderer, &m_Stats);

    for (int i = 0; i < 50; ++i) {
        p->signalVsync();
    }
    QVERIFY(p->waitForAsyncVsync(0));
    // Single-bit pending must be consumed: a second zero-timeout wait returns
    // false (no new signal, no fallback timeout).
    QVERIFY(!p->waitForAsyncVsync(0));
}

void PacerTest::waitBoundsSpuriousWakesByDeadline()
{
    FakeRenderer renderer;
    auto p = std::make_unique<Pacer>(&renderer, &m_Stats);

    std::atomic<bool> stop{false};
    std::thread waker([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            p->m_VsyncSignalled.wakeOne();
            QThread::msleep(2);
        }
    });

    QElapsedTimer timer; timer.start();
    bool pending = p->waitForAsyncVsync(150);
    qint64 elapsed = timer.elapsed();

    // Stop the waker immediately, then join (no fixed 400 ms wait).
    stop.store(true, std::memory_order_relaxed);
    waker.join();

    QVERIFY(!pending);
    QVERIFY2(elapsed >= 130,
             qPrintable(QStringLiteral("helper exited too early: %1 ms").arg(elapsed)));
    QVERIFY2(elapsed < 1000,
             qPrintable(QStringLiteral("helper ran past deadline: %1 ms").arg(elapsed)));
}

void PacerTest::waitReturnsFalseAfterStop()
{
    FakeRenderer renderer;
    auto p = std::make_unique<Pacer>(&renderer, &m_Stats);

    std::thread stopper([&]() {
        QThread::msleep(20);
        p->m_FrameQueueLock.lock();
        p->m_Stopping = true;
        p->m_VsyncSignalled.wakeAll();
        p->m_FrameQueueLock.unlock();
    });
    bool pending = p->waitForAsyncVsync(500);
    stopper.join();
    QVERIFY(!pending);
}

void PacerTest::handleVsyncEmptyQueueBailsOnStop()
{
    FakeRenderer renderer;
    auto p = std::make_unique<Pacer>(&renderer, &m_Stats);
    p->m_MaxVideoFps = 60;
    p->m_DisplayFps = 60;

    // The stopper runs after handleVsync enters its wait, so the
    // predicate-wakeAll path is exercised (not just the timeout path).
    std::thread stopper([&]() {
        QThread::msleep(20);
        p->m_FrameQueueLock.lock();
        p->m_Stopping = true;
        p->m_PacingQueueNotEmpty.wakeAll();
        p->m_FrameQueueLock.unlock();
    });

    QElapsedTimer timer; timer.start();
    p->handleVsync(1000);
    qint64 elapsed = timer.elapsed();

    stopper.join();
    QVERIFY(p->m_Stopping.load());

    p->m_FrameQueueLock.lock();
    bool empty = p->m_PacingQueue.isEmpty();
    int renderSize = p->m_RenderQueue.size();
    p->m_FrameQueueLock.unlock();

    QVERIFY(empty);
    QCOMPARE(renderSize, 0);
    // Must have returned promptly after the wakeAll, well before the 1000 ms
    // slack budget the test would have on a pure-timeout exit.
    QVERIFY2(elapsed < 500,
             qPrintable(QStringLiteral("handleVsync took too long to bail on stop: %1 ms")
                        .arg(elapsed)));
}

void PacerTest::handleVsyncStopGuardRunsEvenWithQueuedFrames()
{
    FakeRenderer renderer;
    auto p = std::make_unique<Pacer>(&renderer, &m_Stats);
    p->m_MaxVideoFps = 60;
    p->m_DisplayFps = 60;

    AVFrame* f = makeTrackedFrame();
    p->m_PacingQueue.enqueue(f);

    p->m_FrameQueueLock.lock();
    p->m_Stopping = true;
    p->m_FrameQueueLock.unlock();

    p->handleVsync(16);

    p->m_FrameQueueLock.lock();
    int renderSize = p->m_RenderQueue.size();
    int pacingSize = p->m_PacingQueue.size();
    p->m_FrameQueueLock.unlock();

    QCOMPARE(renderSize, 0);
    QCOMPARE(pacingSize, 1); // destructor owns the frame release
}

void PacerTest::handleVsyncSpuriousWakeWithoutStopTimesOut()
{
    FakeRenderer renderer;
    auto p = std::make_unique<Pacer>(&renderer, &m_Stats);
    p->m_MaxVideoFps = 60;
    p->m_DisplayFps = 60;

    std::thread waker([&]() {
        QDeadlineTimer until(80);
        while (!until.hasExpired()) {
            p->m_PacingQueueNotEmpty.wakeOne();
            QThread::msleep(2);
        }
    });

    QElapsedTimer timer; timer.start();
    p->handleVsync(16); // 13 ms deadline with TIMER_SLACK_MS=3
    qint64 elapsed = timer.elapsed();
    waker.join();

    p->m_FrameQueueLock.lock();
    int renderSize = p->m_RenderQueue.size();
    p->m_FrameQueueLock.unlock();
    QCOMPARE(renderSize, 0);
    QVERIFY2(elapsed >= 10 && elapsed < 200,
             qPrintable(QStringLiteral("spurious-wake handleVsync took %1 ms").arg(elapsed)));
}

void PacerTest::vsyncThreadFiresPacingWithoutCallback()
{
    FakeRenderer renderer;
    auto p = std::make_unique<Pacer>(&renderer, &m_Stats);
    p->m_MaxVideoFps = 60;
    p->m_DisplayFps = 60;
    p->m_RendererAttributes = 0;
    auto* src = new FakeVsyncSource();
    src->setPacer(p.get());
    p->m_VsyncSource = src;

    AVFrame* f = makeTrackedFrame();
    p->submitFrame(f);

    p->m_VsyncThread = SDL_CreateThread(Pacer::vsyncThread, "PacerVsyncTest", p.get());

    QElapsedTimer timer; timer.start();
    bool delivered = false;
    while (timer.elapsed() < 1500) {
        p->m_FrameQueueLock.lock();
        delivered = !p->m_RenderQueue.isEmpty();
        p->m_FrameQueueLock.unlock();
        if (delivered) break;
        QThread::msleep(20);
    }
    QVERIFY2(delivered,
             "vsyncThread did not deliver a queued frame without any signalVsync()");

    // Render thread must consume the frame before the destructor frees it.
    p->m_RenderThread = SDL_CreateThread(Pacer::renderThread, "PacerRenderTest", p.get());
    while (renderer.renderedCount() < 1 && timer.elapsed() < 1500) {
        QThread::msleep(20);
    }
    QCOMPARE(renderer.renderedCount(), 1);
}

void PacerTest::destructorFreesQueuedFramesAndJoins()
{
    FakeRenderer renderer;
    auto p = std::make_unique<Pacer>(&renderer, &m_Stats);

    // Frames in BOTH queues with no consumer running must be released by the
    // destructor (av_frame_free on AVBuffer-backed frames).
    AVFrame* a = makeTrackedFrame();
    AVFrame* b = makeTrackedFrame();
    AVFrame* c = makeTrackedFrame();
    AVFrame* d = makeTrackedFrame();
    p->m_PacingQueue.enqueue(a);
    p->m_PacingQueue.enqueue(b);
    p->m_RenderQueue.enqueue(c);
    p->m_RenderQueue.enqueue(d);

    int freeBefore = m_BufferFreeCount.load();
    p.reset(); // drives the destructor
    int freeAfter = m_BufferFreeCount.load();

    QCOMPARE(freeAfter - freeBefore, 4);
    QCOMPARE(renderer.cleanupCount(), 1);
}

void PacerTest::renderThreadShutdownWithEmptyQueuesIsBounded()
{
    FakeRenderer renderer;
    auto p = std::make_unique<Pacer>(&renderer, &m_Stats);
    p->m_MaxVideoFps = 60;
    p->m_DisplayFps = 60;
    p->m_RendererAttributes = 0;

    p->m_RenderThread = SDL_CreateThread(Pacer::renderThread, "PacerRenderShutdown", p.get());

    // Wait until the render thread has entered its waitToRender() at least
    // once so we know it is parked inside the render loop, then trigger the
    // destructor wake.
    QVERIFY(renderer.waitEnteredSemaphore().tryAcquire(1, 1500));

    QElapsedTimer timer; timer.start();
    p.reset();
    qint64 elapsed = timer.elapsed();

    // Bounded by the process timeout; in practice returns promptly after the
    // destructor wakeAll.
    QVERIFY2(elapsed < 5000,
             qPrintable(QStringLiteral("destructor join took %1 ms").arg(elapsed)));
}

QTEST_GUILESS_MAIN(PacerTest)

#include "tst_pacer.moc"
