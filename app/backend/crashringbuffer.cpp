#include "crashringbuffer.h"

#include <QByteArray>

#include <atomic>
#include <cstring>

namespace CrashRingBuffer {

namespace {

// Total ring buffer size is 64 KB. A single contiguous copy is bounded to a
// few hundred bytes (a typical log line is well under that), so even though
// this is a tiny spinlock, contention is not a concern.
constexpr std::size_t kSlotCapacity = kCapacity;

alignas(64) char s_Buffer[kSlotCapacity] = {};
std::atomic<std::size_t> s_WriteIndex{0};

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
    // The buffer is logically circular starting from s_WriteIndex (the next
    // write position) and running for kSlotCapacity bytes, wrapping around.
    // Copy in at most two contiguous runs so the wrap is handled without
    // building a std::string or QByteArray of arbitrary size in the handler.
    const std::size_t head = s_WriteIndex.load(std::memory_order_acquire);

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

}
