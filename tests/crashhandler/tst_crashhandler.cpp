#include <QtTest>
#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLatin1String>
#include <QProcess>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "backend/crashhandler.h"
#include "backend/crashringbuffer.h"
#include "path.h"

namespace {

constexpr const char* kCrashChildArg = "--crash-child";
constexpr const char* kMarkerLine = "UNIQUE_MARKER_LINE_12345";

QString expectedLogDir(const QString& stateHome)
{
    return stateHome + QStringLiteral("/Artemis Desktop Project/Artemis");
}

// Re-exec the test binary as a crash-producing child. Forks directly so
// we can use waitpid/WIFSIGNALED/WTERMSIG to assert the child actually died
// of SIGSEGV, not just any crash. Qt's QProcess does not surface the raw
// signal number in every version, and the raw-fork path is the cleanest
// match for what the acceptance contract is asserting.
pid_t spawnCrashChild(const QString& stateHome)
{
    const pid_t pid = ::fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        // Child: re-exec the same binary with --crash-child <stateHome>.
        // Set XDG_STATE_HOME in the environment so QStandardPaths picks up
        // our temp directory as the StateLocation root.
        qputenv("XDG_STATE_HOME", stateHome.toUtf8());

        const QByteArray exe = QCoreApplication::applicationFilePath().toLocal8Bit();
        const QByteArray stateArg = stateHome.toLocal8Bit();

        ::execl(exe.constData(), exe.constData(),
                kCrashChildArg, stateArg.constData(),
                static_cast<char*>(nullptr));
        // execl only returns on failure.
        ::_exit(127);
    }
    return pid;
}

int runCrashChildMain(const QString& stateHome)
{
    // Must run before any QStandardPaths consumer touches the cache.
    qputenv("XDG_STATE_HOME", stateHome.toUtf8());

    QCoreApplication::setOrganizationName("Artemis Desktop Project");
    QCoreApplication::setOrganizationDomain("artemisdesktop.com");
    QCoreApplication::setApplicationName("Artemis");

    Path::initialize(false);

    // install() mkpaths the log directory and opens the crash file. Both
    // must succeed before the test asserts they exist on disk.
    CrashHandler::install();

    // Stand-in for "a log line emitted microseconds before the fault". This
    // goes through CrashRingBuffer::append exactly the way the production
    // logToLoggerStream() path does — synchronously, before any queue drain
    // — which is the whole property the acceptance test exercises.
    CrashRingBuffer::append(QString::fromLatin1(kMarkerLine));

    // Deliberate SIGSEGV via a real memory fault. We crash before any
    // QObject destructor runs, so the handler's own write()s are the only
    // way the marker survives.
    volatile int* p = nullptr;
    *p = 1;

    // Unreachable — the OS will have killed us before getting here.
    return 0;
}

} // namespace

class CrashHandlerTest : public QObject
{
    Q_OBJECT

private slots:
    void crashFileCapturesBuildIdFramesMapsAndRingBuffer();
    void installCreatesMissingLogDirectory();
    void ringBufferSnapshotHasNoNulPaddingBeforeWrap();
    void ringBufferSnapshotDropsOldestBytesAfterWrap();
};

void CrashHandlerTest::crashFileCapturesBuildIdFramesMapsAndRingBuffer()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString stateHome = tempDir.path();
    const QString logDir = expectedLogDir(stateHome);
    QVERIFY(!QDir(logDir).exists());

    const pid_t pid = spawnCrashChild(stateHome);
    QVERIFY2(pid >= 0, "fork() failed");

    int status = 0;
    const pid_t waited = ::waitpid(pid, &status, 0);
    QVERIFY2(waited == pid, "waitpid failed");

    QVERIFY2(WIFSIGNALED(status), "child exited normally instead of being signaled");
    QCOMPARE(WTERMSIG(status), SIGSEGV);

    QDir d(logDir);
    const QStringList crashFiles = d.entryList(
        QStringList(QStringLiteral("Artemis-crash-*.txt")),
        QDir::Files | QDir::NoDotAndDotDot);
    QCOMPARE(crashFiles.size(), 1);

    QFile crashFile(d.filePath(crashFiles.first()));
    QVERIFY(crashFile.open(QIODevice::ReadOnly));
    const QString content = QString::fromUtf8(crashFile.readAll());
    crashFile.close();

    // Build-id line present with a non-"unknown" hex value.
    const QRegularExpression buildIdRe(QStringLiteral("\nbuild-id: ([0-9a-f]+|unknown)\n"));
    const QRegularExpressionMatch buildIdMatch = buildIdRe.match(content);
    QVERIFY2(buildIdMatch.hasMatch(), "no build-id: line in crash file");
    const QString buildIdValue = buildIdMatch.captured(1);
    QVERIFY2(buildIdValue != QLatin1String("unknown"),
             qPrintable(QStringLiteral("build-id extraction failed (got 'unknown'). ")
                        + QStringLiteral("Crash file:\n") + content));
    QVERIFY(buildIdValue.size() >= 16); // 64-bit minimum, typically 40 for sha1

    // Frames section with at least one 0x-prefixed hex address.
    const int framesIdx = content.indexOf(QStringLiteral("\nframes:\n"));
    QVERIFY(framesIdx >= 0);
    const int logTailIdx = content.indexOf(QStringLiteral("\nlog tail:\n"));
    QVERIFY(logTailIdx > framesIdx);
    const QString framesSection = content.mid(framesIdx, logTailIdx - framesIdx);
    const QRegularExpression frameRe(QStringLiteral("0x[0-9a-fA-F]+"));
    QVERIFY2(frameRe.match(framesSection).hasMatch(),
             qPrintable(QStringLiteral("frames section contains no 0x... address. ")
                        + QStringLiteral("Frames section:\n") + framesSection));

    // /proc/self/maps markers.
    QVERIFY2(content.contains(QStringLiteral("r-xp"))
             || content.contains(QStringLiteral("r--p")),
             "no /proc/self/maps markers (r-xp / r--p) in crash file");

    // Marker from the ring buffer must appear after the log-tail marker.
    const int markerIdx = content.indexOf(QString::fromLatin1(kMarkerLine));
    QVERIFY2(markerIdx >= 0, "marker line not present in crash file at all");
    QVERIFY2(markerIdx >= logTailIdx,
             qPrintable(QStringLiteral("marker line was present but BEFORE log-tail marker. ")
                        + QStringLiteral("This would mean the marker was only in ")
                        + QStringLiteral("the file log, not the ring buffer (the async ")
                        + QStringLiteral("logger thread never drained — exactly the case ")
                        + QStringLiteral("the ring buffer is supposed to survive).")));
}

void CrashHandlerTest::installCreatesMissingLogDirectory()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    // The log dir does not exist yet — install() must create it.
    qputenv("XDG_STATE_HOME", tempDir.path().toUtf8());
    QCoreApplication::setOrganizationName("Artemis Desktop Project");
    QCoreApplication::setOrganizationDomain("artemisdesktop.com");
    QCoreApplication::setApplicationName("Artemis");

    Path::initialize(false);

    const QString logDir = Path::getLogDir();
    QVERIFY(!QDir(logDir).exists());

    CrashHandler::install();

    QVERIFY2(QDir(logDir).exists(),
             qPrintable(QStringLiteral("install() did not create the log directory: ")
                        + logDir));
}

void CrashHandlerTest::ringBufferSnapshotHasNoNulPaddingBeforeWrap()
{
    const QString marker = QStringLiteral(
        "RINGBUF_PREWRAP_MARKER_0123456789_0123456789_0123456789_0123");
    QCOMPARE(marker.size(), 60); // sanity: exactly 60 chars, well under 64 KB
    CrashRingBuffer::append(marker);

    char out[256] = {};
    const std::size_t copied = CrashRingBuffer::snapshot(out, sizeof(out));

    const QByteArray got(out, static_cast<int>(copied));
    QCOMPARE(static_cast<int>(copied), marker.toUtf8().size());
    QVERIFY2(!got.contains('\0'),
             "snapshot() returned NUL bytes before the buffer ever wrapped");
    QCOMPARE(QString::fromUtf8(got), marker);
}

void CrashHandlerTest::ringBufferSnapshotDropsOldestBytesAfterWrap()
{
    // Force a wrap: a single append of exactly kCapacity bytes, on top of
    // the marker already written by the previous test, guarantees
    // head + n >= kCapacity.
    QByteArray fill(static_cast<int>(CrashRingBuffer::kCapacity), 'B');
    CrashRingBuffer::append(QString::fromLatin1(fill));

    std::vector<char> out(CrashRingBuffer::kCapacity);
    const std::size_t copied = CrashRingBuffer::snapshot(out.data(), out.size());

    QCOMPARE(copied, CrashRingBuffer::kCapacity);
    const QByteArray got(out.data(), static_cast<int>(copied));
    QVERIFY2(!got.contains('\0'),
             "snapshot() returned NUL bytes after the buffer wrapped");
    // The pre-wrap marker from the previous test must have been fully
    // overwritten — it no longer fits once the buffer is entirely 'B's.
    QVERIFY2(!got.contains("RINGBUF_PREWRAP_MARKER"),
             "oldest bytes were not dropped after the buffer wrapped");
}

int main(int argc, char* argv[])
{
    // Intercept --crash-child BEFORE QApplication parses argv, so the
    // crash-producing child runs the failure path without going through
    // QTest.
    for (int i = 1; i < argc; i++) {
        if (QLatin1String(argv[i]) == QLatin1String(kCrashChildArg)
            && i + 1 < argc) {
            return runCrashChildMain(QString::fromLocal8Bit(argv[i + 1]));
        }
    }

    QCoreApplication app(argc, argv);
    CrashHandlerTest tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_crashhandler.moc"
