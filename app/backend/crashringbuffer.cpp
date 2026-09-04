#include "crashringbuffer.h"

#include <QByteArray>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace CrashRingBuffer {

namespace {

// Async-signal-safe. Retries on EINTR and partial writes; bails out after a
// bounded number of attempts so a persistently failing fd cannot spin
// forever inside a signal handler. Returns the number of bytes actually
// written.
static std::size_t writeAll(int fd, const char* buf, std::size_t len)
{
    constexpr int kMaxAttempts = 1024;
    std::size_t written = 0;
    int attempts = 0;
    while (written < len && attempts < kMaxAttempts) {
        ssize_t n = ::write(fd, buf + written, len - written);
        if (n > 0) {
            written += static_cast<std::size_t>(n);
        }
        else if (n < 0 && errno != EINTR) {
            break;
        }
        ++attempts;
    }
    return written;
}

// Total ring buffer size is 64 KB. A single contiguous copy is bounded to a
// few hundred bytes (a typical log line is well under that), so even though
// this is a tiny spinlock, contention is not a concern.
constexpr std::size_t kSlotCapacity = kCapacity;

alignas(64) char s_Buffer[kSlotCapacity] = {};
std::atomic<std::size_t> s_WriteIndex{0};

// True once the buffer has been written past its own length at least once.
// Before that point, only [0, s_WriteIndex) holds real data — the rest of
// s_Buffer is still its zero-initialized padding, and snapshot()/
// snapshotToFd() must not read it as if it were log content.
std::atomic<bool> s_Wrapped{false};

std::atomic_flag s_Spinlock = ATOMIC_FLAG_INIT;

std::atomic<bool> s_Crashing{false};

}

void append(const QString& message)
{
    if (s_Crashing.load(std::memory_order_acquire)) {
        // The crash handler is dumping the buffer. Any further write here
        // could corrupt the snapshot mid-read. Drop the message instead.
        return;
    }

    QByteArray utf8 = message.toUtf8();
    if (utf8.isEmpty()) {
        return;
    }

    // Bound the copy so a single message cannot monopolize the buffer or
    // wrap it past its own tail. Truncating here loses a line, which is
    // preferable to corrupting the buffer layout.
    const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(utf8.size()),
                                                kSlotCapacity);

    while (s_Spinlock.test_and_set(std::memory_order_acquire)) {
        // Spin briefly. The critical section is a memcpy of a few hundred
        // bytes; contention is essentially impossible in practice.
    }

    // Re-check s_Crashing once the spinlock is held. The earlier
    // s_Crashing.load() above is a fast-path bailout, but it tests
    // s_Crashing BEFORE we acquire the spinlock — a writer that is
    // already blocked here will pass the fast-path test, then sit on the
    // spinlock while the crash handler sets s_Crashing = true and starts
    // reading. Without this second check, we would then proceed to write
    // into the buffer concurrently with the handler's snapshot, corrupting
    // whatever it was about to capture.
    if (s_Crashing.load(std::memory_order_acquire)) {
        s_Spinlock.clear(std::memory_order_release);
        return;
    }

    const std::size_t head = s_WriteIndex.load(std::memory_order_relaxed);

    // Mirror snapshot()'s span-split read pattern. Without this split, a
    // memcpy near the end of the buffer would write past the end of
    // s_Buffer. Two spans are always sufficient because head < kSlotCapacity
    // and n <= kSlotCapacity, so head + n <= 2 * kSlotCapacity.
    const std::size_t firstSpan = std::min(n, kSlotCapacity - head);
    std::memcpy(s_Buffer + head, utf8.constData(), firstSpan);
    if (firstSpan < n) {
        std::memcpy(s_Buffer, utf8.constData() + firstSpan, n - firstSpan);
    }
    if (head + n >= kSlotCapacity) {
        s_Wrapped.store(true, std::memory_order_release);
    }
    s_WriteIndex.store((head + n) % kSlotCapacity, std::memory_order_release);

    s_Spinlock.clear(std::memory_order_release);
}

bool isCrashing()
{
    return s_Crashing.load(std::memory_order_acquire);
}

void setCrashing(bool crashing)
{
    s_Crashing.store(crashing, std::memory_order_release);
}

std::size_t snapshot(char* outBuffer, std::size_t outBufferSize)
{
    if (outBuffer == nullptr || outBufferSize == 0) {
        return 0;
    }

    // The crash handler must never take the spinlock or call anything that
    // can block. A best-effort, possibly-torn snapshot is fine and expected.
    //
    // Load s_Wrapped BEFORE s_WriteIndex: a writer that wraps the buffer
    // between these two loads would otherwise pair a stale pre-wrap head
    // with wrapped=true, and we'd treat the zero-padding region as
    // log content.
    const bool wrapped = s_Wrapped.load(std::memory_order_acquire);
    const std::size_t head = s_WriteIndex.load(std::memory_order_acquire);

    if (!wrapped) {
        // The buffer has never been written past its own length. Only
        // [0, head) holds real data; the rest of s_Buffer is still its
        // zero-initialized padding and must not be copied out.
        const std::size_t copied = std::min<std::size_t>(head, outBufferSize);
        std::memcpy(outBuffer, s_Buffer, copied);
        return copied;
    }

    // The buffer is logically circular starting from s_WriteIndex (the next
    // write position) and running for kSlotCapacity bytes, wrapping around.
    // Copy in at most two contiguous runs so the wrap is handled without
    // building a std::string or QByteArray of arbitrary size in the handler.
    const std::size_t firstSpan = std::min<std::size_t>(kSlotCapacity - head,
                                                       outBufferSize);
    std::memcpy(outBuffer, s_Buffer + head, firstSpan);
    std::size_t copied = firstSpan;

    if (copied < outBufferSize) {
        const std::size_t secondTake = std::min<std::size_t>(head,
                                                             outBufferSize - copied);
        std::memcpy(outBuffer + copied, s_Buffer, secondTake);
        copied += secondTake;
    }

    return copied;
}

std::size_t snapshotToFd(int fd)
{
    // The crash handler must never take the spinlock or call anything that
    // can block. A best-effort, possibly-torn snapshot is fine and expected.
    //
    // Stream the buffer straight to the fd in at most two ::write() calls
    // instead of building a caller buffer. This avoids the 64 KB on-stack
    // buffer that would otherwise exhaust the entire alternate signal stack
    // (kAltStackSize is also 64 KB), which is the exact case sigaltstack
    // exists to survive.
    //
    // Load s_Wrapped BEFORE s_WriteIndex: see snapshot() above for why.
    // Route each span through writeAll() so a short write or EINTR cannot
    // silently truncate the captured tail.
    const bool wrapped = s_Wrapped.load(std::memory_order_acquire);
    const std::size_t head = s_WriteIndex.load(std::memory_order_acquire);

    if (!wrapped) {
        // Only [0, head) holds real data — see snapshot() above for why.
        return writeAll(fd, s_Buffer, head);
    }

    const std::size_t firstSpan = kSlotCapacity - head;
    const std::size_t w1 = writeAll(fd, s_Buffer + head, firstSpan);
    std::size_t written = w1;

    if (head > 0) {
        const std::size_t w2 = writeAll(fd, s_Buffer, head);
        written += w2;
    }

    return written;
}

}
