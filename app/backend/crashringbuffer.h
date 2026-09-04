#pragma once

#include <QString>

#include <atomic>
#include <cstddef>

namespace CrashRingBuffer {

constexpr std::size_t kCapacity = 64 * 1024;

void append(const QString& message);

bool isCrashing();

void setCrashing(bool crashing);

std::size_t snapshot(char* outBuffer, std::size_t outBufferSize);

}
