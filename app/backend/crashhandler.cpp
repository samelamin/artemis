#include "crashhandler.h"

#include "crashringbuffer.h"
#include "path.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QString>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <unistd.h>

#include <elf.h>
#include <link.h>

#include <execinfo.h>

namespace CrashHandler {

namespace {

// File descriptor of the crash file. Opened at install time and held open
// for the lifetime of the process so the handler never has to call open().
int s_CrashFd = -1;

// Path of the crash file chosen at install time. Exposed via crashFilePath()
// so main() can unlink the current run's empty crash file at clean shutdown.
QString s_CrashPath;

// Alternate signal stack. A handler running on the overflowed stack itself
// would immediately re-fault, so SIGSEGV from a stack overflow must run
// here.
constexpr std::size_t kAltStackSize = 64 * 1024;
alignas(16) char s_AltStackMemory[kAltStackSize];

// Max stack frames captured per crash. Fixed so backtrace() does not
// allocate. The frame buffer itself is a local inside handleSignal() (not
// a global) — see the "Known limitations" note in
// docs/plans/2026-09-04-diagnostic-reports.md for why it isn't a
// thread-local either.
constexpr int kMaxFrames = 64;

// Compile-time constants for the build identity. Surface real values; the
// version and commit are defined for every build in app/app.pro.
constexpr const char kVersion[] = VERSION_STR;
constexpr const char kCommit[]  = VIBERTEMIS_BUILD_COMMIT;

// Build-id (NT_GNU_BUILD_ID note) hex-encoded, extracted from this
// executable's program headers at install time. Bounded to 40 hex chars
// (20 bytes / 160-bit sha1, the standard --build-id=sha1 output). Empty
// when extraction failed; the handler then emits "unknown". Computed only
// at install time so the in-handler path never parses ELF.
constexpr std::size_t kBuildIdHexMax = 40;
char s_BuildIdHex[kBuildIdHexMax + 1] = {0};

// Resolve /proc/self/exe to a canonical path at install time. Used as the
// input to extractBuildIdFromExe(), which reads program headers and notes
// from disk the same way readelf does.
char s_ExeRealpath[4096] = {0};

// Forward-declared so writeHex() (defined above) can call it; the full
// definition lives below, immediately before writeString().
static std::size_t writeAll(int fd, const char* buf, std::size_t len);

// Hand-rolled signal-safe integer-to-hex formatter. sprintf/snprintf with
// %p/%x are not guaranteed async-signal-safe; we write nibbles into a stack
// buffer ourselves.
void writeHex(int fd, unsigned long long value)
{
    char buf[2 + 16 + 1]; // "0x" + up to 16 hex digits + NUL
    buf[0] = '0';
    buf[1] = 'x';
    int pos = 2;

    if (value == 0) {
        buf[pos++] = '0';
    }
    else {
        char digits[16];
        int ndigits = 0;
        while (value != 0 && ndigits < 16) {
            const unsigned int nibble = value & 0xF;
            digits[ndigits++] = static_cast<char>(nibble < 10
                                                  ? '0' + nibble
                                                  : 'a' + (nibble - 10));
            value >>= 4;
        }
        for (int i = ndigits - 1; i >= 0; i--) {
            buf[pos++] = digits[i];
        }
    }

    buf[pos] = '\0';
    writeAll(fd, buf, static_cast<std::size_t>(pos));
}

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

void writeString(int fd, const char* s)
{
    if (s == nullptr) {
        return;
    }
    // std::strlen() is not in the strict POSIX async-signal-safe function
    // list. Walk the bytes ourselves instead of relying on it inside a
    // signal handler. Bounded so a corrupt/non-terminated pointer cannot
    // spin forever; every real caller in this file (build id, version,
    // commit, and the fixed phrase literals) is far under this bound.
    constexpr std::size_t kMaxLen = 4096;
    std::size_t len = 0;
    while (len < kMaxLen && s[len] != '\0') {
        len++;
    }
    writeAll(fd, s, len);
}

void writeProcMaps(int fd)
{
    // Copy /proc/self/maps verbatim through the crash file in fixed-size
    // chunks. open/read/write are async-signal-safe; building a std::string
    // or QString of arbitrary size in the handler is not.
    int mapsFd = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (mapsFd < 0) {
        writeString(fd, "/proc/self/maps open failed\n");
        return;
    }

    char chunk[4096];
    // Bound the iteration count so a pathological signal storm cannot
    // spin the loop forever inside a signal handler. A multi-hundred-KB
    // /proc/self/maps in 4096-byte chunks needs at most ~200 iterations
    // in the worst realistic case; 100000 is comfortably above that.
    constexpr int kMaxIterations = 100000;
    for (int iterations = 0; iterations < kMaxIterations; ++iterations) {
        const ssize_t n = ::read(mapsFd, chunk, sizeof(chunk));
        if (n > 0) {
            const std::size_t written = writeAll(fd, chunk,
                                                 static_cast<std::size_t>(n));
            if (written != static_cast<std::size_t>(n)) {
                break;
            }
        }
        else if (n == 0) {
            break;
        }
        else {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
    }

    ::close(mapsFd);
}

void writeRingBuffer(int fd)
{
    CrashRingBuffer::snapshotToFd(fd);
}

// Resolve the canonical path of this executable. Returns the length written
// to outBuf (0 on failure). Used at install time only — readlink and
// realpath are not async-signal-safe.
std::size_t resolveExePath(char* outBuf, std::size_t outBufSize)
{
    if (outBuf == nullptr || outBufSize == 0) {
        return 0;
    }
    const ssize_t n = ::readlink("/proc/self/exe", outBuf, outBufSize - 1);
    if (n <= 0) {
        return 0;
    }
    outBuf[n] = '\0';
    return static_cast<std::size_t>(n);
}

// Read from an open fd at a given offset. Returns bytes read, -1 on error.
ssize_t readAt(int fd, void* buf, std::size_t n, off_t off)
{
    return ::pread(fd, buf, n, off);
}

// Parse a single PT_NOTE blob (starting at `data`, of size `size`) and
// hex-encode a matching NT_GNU_BUILD_ID into s_BuildIdHex. Returns true if
// the build-id was extracted.
bool parseNoteSection(const unsigned char* data, std::size_t size)
{
    const std::size_t align = 4; // GNU note entries are always 4-byte padded,
                                  // regardless of ELF class (32 vs 64-bit)
    const unsigned char* end = data + size;

    const unsigned char* p = data;
    while (p + sizeof(ElfW(Nhdr)) <= end) {
        ElfW(Nhdr) nhdr;
        std::memcpy(&nhdr, p, sizeof(nhdr));

        const unsigned char* name = p + sizeof(nhdr);
        const unsigned char* desc = name + ((nhdr.n_namesz + align - 1) & ~(align - 1));

        if (desc > end || nhdr.n_descsz > static_cast<std::size_t>(end - desc)) {
            return false;
        }

        if (nhdr.n_type == NT_GNU_BUILD_ID &&
            nhdr.n_namesz == 4 &&
            std::memcmp(name, "GNU\0", 4) == 0 &&
            nhdr.n_descsz > 0 &&
            nhdr.n_descsz <= kBuildIdHexMax / 2) {

            for (std::size_t b = 0; b < nhdr.n_descsz; b++) {
                const unsigned int byte = desc[b];
                s_BuildIdHex[b * 2 + 0] = static_cast<char>(
                    (byte >> 4) < 10 ? ('0' + (byte >> 4))
                                     : ('a' + (byte >> 4) - 10));
                s_BuildIdHex[b * 2 + 1] = static_cast<char>(
                    (byte & 0xF) < 10 ? ('0' + (byte & 0xF))
                                      : ('a' + (byte & 0xF) - 10));
            }
            s_BuildIdHex[nhdr.n_descsz * 2] = '\0';
            return true;
        }

        const std::size_t alignedDesc = (nhdr.n_descsz + align - 1) & ~(align - 1);
        p = desc + alignedDesc;
    }
    return false;
}

// Read the ELF program headers from disk and look for a PT_NOTE whose
// contents encode NT_GNU_BUILD_ID. Reads straight from the file the way
// readelf does, so it does not depend on the loader's view of where
// segments live in memory.
bool extractBuildIdFromExe(const char* exePath)
{
    if (exePath == nullptr || exePath[0] == '\0') {
        return false;
    }

    int fd = ::open(exePath, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    ElfW(Ehdr) ehdr;
    if (readAt(fd, &ehdr, sizeof(ehdr), 0) != static_cast<ssize_t>(sizeof(ehdr))) {
        ::close(fd);
        return false;
    }

    // Sanity-check magic so we don't walk garbage if exePath points at
    // something that isn't actually an ELF file.
    if (std::memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0) {
        ::close(fd);
        return false;
    }

    if (ehdr.e_phnum == 0 || ehdr.e_phnum > 64) {
        ::close(fd);
        return false;
    }

    ElfW(Phdr) phdrs[64];
    const std::size_t phdrSize = sizeof(ElfW(Phdr)) * ehdr.e_phnum;
    if (readAt(fd, phdrs, phdrSize, ehdr.e_phoff) != static_cast<ssize_t>(phdrSize)) {
        ::close(fd);
        return false;
    }

    bool found = false;
    for (int i = 0; i < ehdr.e_phnum; i++) {
        const ElfW(Phdr)& phdr = phdrs[i];
        if (phdr.p_type != PT_NOTE) {
            continue;
        }

        if (phdr.p_filesz == 0 || phdr.p_filesz > 1024 * 1024) {
            continue;
        }

        // Read the note section into a fixed-size on-stack buffer when it
        // fits, otherwise into the heap. The maximum note size we'll ever
        // see in a real binary is small; the cap above is just a sanity
        // bound to keep this from going off the rails on a malformed file.
        unsigned char stackBuf[4096];
        std::unique_ptr<unsigned char[]> heapBuf;
        unsigned char* buf = stackBuf;
        if (phdr.p_filesz > sizeof(stackBuf)) {
            heapBuf.reset(new unsigned char[phdr.p_filesz]);
            buf = heapBuf.get();
        }

        if (readAt(fd, buf, phdr.p_filesz, phdr.p_offset)
            != static_cast<ssize_t>(phdr.p_filesz)) {
            continue;
        }

        if (parseNoteSection(buf, phdr.p_filesz)) {
            found = true;
            break;
        }
    }

    ::close(fd);
    return found;
}

void extractBuildId()
{
    if (resolveExePath(s_ExeRealpath, sizeof(s_ExeRealpath)) == 0) {
        return;
    }
    if (!extractBuildIdFromExe(s_ExeRealpath)) {
        // s_BuildIdHex is zero-initialized; on failure it stays empty and
        // the handler reports "unknown".
        s_BuildIdHex[0] = '\0';
    }
}

void handleSignal(int sig, siginfo_t* info, void* /*ucontext*/)
{
    if (s_CrashFd < 0) {
        // Install never succeeded; nothing we can usefully do.
        std::signal(sig, SIG_DFL);
        std::raise(sig);
        return;
    }

    // Mark the process as crashing *first*, so any other thread that wakes
    // up in the middle of writing to the ring buffer will see the flag and
    // return without touching shared state.
    CrashRingBuffer::setCrashing(true);

    writeString(s_CrashFd, "== crash ==\n");
    writeString(s_CrashFd, "signal: ");
    writeHex(s_CrashFd, static_cast<unsigned long long>(sig));
    writeString(s_CrashFd, "\nsi_addr: ");
    const unsigned long long addr = info != nullptr
                                    ? reinterpret_cast<unsigned long long>(info->si_addr)
                                    : 0ULL;
    writeHex(s_CrashFd, addr);
    writeString(s_CrashFd, "\nversion: ");
    writeString(s_CrashFd, kVersion);
    writeString(s_CrashFd, "\ncommit: ");
    writeString(s_CrashFd, kCommit);
    writeString(s_CrashFd, "\nbuild-id: ");
    writeString(s_CrashFd, s_BuildIdHex[0] != '\0' ? s_BuildIdHex : "unknown");
    writeString(s_CrashFd, "\n");

    writeString(s_CrashFd, "frames:\n");
    void* frameBuffer[kMaxFrames];
    // backtrace() was pre-warmed at install time, so this call does not
    // allocate. It is still not strictly async-signal-safe on glibc (it can
    // take the dynamic loader lock), which could deadlock the handler if
    // the crashing thread already held that lock. arm a 5-second watchdog
    // immediately before the call and disarm it immediately after — but
    // with a SIGALRM handler installed (not the default disposition) so a
    // deadlocked backtrace() that *does* survive the 5s timeout still
    // ends in a controlled process exit rather than the wrong exit signal
    // (SIGALRM's default is "terminate without core dump"), which would
    // defeat the system crash reporter.
    struct sigaction alarmPrev {};
    struct sigaction alarmSa {};
    alarmSa.sa_handler = [](int) { ::_exit(128 + SIGALRM); };
    sigemptyset(&alarmSa.sa_mask);
    alarmSa.sa_flags = 0;
    ::sigaction(SIGALRM, &alarmSa, &alarmPrev);
    ::alarm(5);
    const int nFrames = ::backtrace(frameBuffer, kMaxFrames);
    ::alarm(0);
    ::sigaction(SIGALRM, &alarmPrev, nullptr);
    for (int i = 0; i < nFrames; i++) {
        writeHex(s_CrashFd,
                 reinterpret_cast<unsigned long long>(frameBuffer[i]));
        writeString(s_CrashFd, "\n");
    }

    writeString(s_CrashFd, "maps:\n");
    writeProcMaps(s_CrashFd);

    writeString(s_CrashFd, "log tail:\n");
    writeRingBuffer(s_CrashFd);

    // Hand the rest of the crash to the OS so the process terminates the
    // way it normally would (core dump, etc.).
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

} // namespace

QString crashFilePath()
{
    return s_CrashPath;
}

void install()
{
    // Open the crash file once at install time so the handler never has to.
    // mkpath the directory at install time for the same reason.
    const QString crashDir = Path::getLogDir();
    if (crashDir.isEmpty()) {
        // Cannot usefully proceed; leave s_CrashFd = -1 so the handler bails.
        return;
    }

    QDir().mkpath(crashDir);

    const QString crashPath = crashDir + QStringLiteral("/Artemis-crash-")
                              + QString::number(QDateTime::currentMSecsSinceEpoch())
                              + QStringLiteral(".txt");
    s_CrashPath = crashPath;

    const QByteArray pathUtf8 = crashPath.toUtf8();
    s_CrashFd = ::open(pathUtf8.constData(),
                       O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                       0600);
    if (s_CrashFd < 0) {
        return;
    }

    // Install the alternate signal stack so a stack-overflow SIGSEGV can
    // still run the handler.
    //
    // Known limitation: sigaltstack() is inherently per-thread (a
    // POSIX/glibc property, not something this call can work around). This
    // only installs the alt-stack for the thread that calls install() (the
    // main thread). A stack overflow on any other thread — including Qt's
    // internal thread pools, which are not easily reachable to call
    // sigaltstack() on individually — will not have an alt-stack available,
    // and the handler may fail to run for that specific case. Non-overflow
    // signals (SIGABRT, SIGBUS, SIGFPE, SIGILL, and SIGSEGV from any cause
    // other than stack overflow) are handled correctly on every thread
    // regardless of which thread installed the alt-stack.
    stack_t alt{};
    alt.ss_sp = s_AltStackMemory;
    alt.ss_size = kAltStackSize;
    alt.ss_flags = 0;
    if (::sigaltstack(&alt, nullptr) != 0) {
        // Best effort. The handler still works for non-overflow signals.
    }

    // Pre-warm the unwinder so the in-handler call does not hit the
    // dynamic loader lock or first-call allocation. Uses its own
    // throwaway local — the in-handler call in handleSignal() has its own
    // separate local frame buffer (not a shared global, so concurrent or
    // nested crashes on different threads cannot corrupt each other's
    // frame data).
    void* warmupFrameBuffer[kMaxFrames];
    ::backtrace(warmupFrameBuffer, kMaxFrames);

    // Extract this executable's NT_GNU_BUILD_ID now so the handler only
    // has to write() a precomputed hex string.
    extractBuildId();

    struct sigaction sa {};
    sa.sa_sigaction = &handleSignal;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGSEGV);
    sigaddset(&sa.sa_mask, SIGABRT);
    sigaddset(&sa.sa_mask, SIGBUS);
    sigaddset(&sa.sa_mask, SIGFPE);
    sigaddset(&sa.sa_mask, SIGILL);
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;

    ::sigaction(SIGSEGV, &sa, nullptr);
    ::sigaction(SIGABRT, &sa, nullptr);
    ::sigaction(SIGBUS,  &sa, nullptr);
    ::sigaction(SIGFPE,  &sa, nullptr);
    ::sigaction(SIGILL,  &sa, nullptr);
}

} // namespace CrashHandler
