#include <QtTest>

#include <cstring>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QProcess>
#include <QQueue>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include "backend/diagnosticreporter.h"
#include "backend/logscrubber.h"

namespace {

struct Script {
    QByteArray body;
    int status = 200;
    QNetworkReply::NetworkError error = QNetworkReply::NoError;
    QByteArray errorText;
    QList<QPair<QByteArray, QByteArray>> headers;
};

class ScriptedReply : public QNetworkReply
{
public:
    ScriptedReply(const QNetworkRequest &request,
                  QNetworkAccessManager::Operation operation,
                  const Script &script,
                  QObject *parent) :
        QNetworkReply(parent),
        m_Body(script.body),
        m_Offset(0),
        m_Available(script.body.size())
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(operation);
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, script.status);
        for (const QPair<QByteArray, QByteArray> &header : script.headers) {
            setRawHeader(header.first, header.second);
        }
        setError(script.error,
                 script.errorText.isEmpty()
                    ? QStringLiteral("scripted network error")
                    : QString::fromUtf8(script.errorText));
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        QTimer::singleShot(0, this, [this]() {
            if (isFinished()) {
                return;
            }
            setFinished(true);
            emit finished();
        });
    }

    void abort() override
    {
        if (!isFinished()) {
            setError(QNetworkReply::OperationCanceledError,
                     QStringLiteral("cancelled"));
            setFinished(true);
            emit finished();
        }
    }

    qint64 bytesAvailable() const override
    {
        return (m_Available - m_Offset) + QIODevice::bytesAvailable();
    }

protected:
    qint64 readData(char *data, qint64 maxSize) override
    {
        if (m_Offset >= m_Available) {
            return m_Available >= m_Body.size() ? -1 : 0;
        }
        const qint64 count = qMin(maxSize, m_Available - m_Offset);
        memcpy(data, m_Body.constData() + m_Offset,
               static_cast<size_t>(count));
        m_Offset += count;
        return count;
    }

private:
    QByteArray m_Body;
    qint64 m_Offset;
    qint64 m_Available;
};

class FakeNam : public QNetworkAccessManager
{
public:
    explicit FakeNam(QObject *parent = nullptr) :
        QNetworkAccessManager(parent)
    {
    }

    void enqueue(const Script &script)
    {
        m_Scripts.enqueue(script);
    }

    QList<QNetworkRequest> requests;
    QList<QByteArray> sentBodies;
    QQueue<Script> m_Scripts;

protected:
    QNetworkReply *createRequest(Operation operation,
                                 const QNetworkRequest &request,
                                 QIODevice *outgoingData = nullptr) override
    {
        requests.append(request);

        Script script;
        if (!m_Scripts.isEmpty()) {
            script = m_Scripts.dequeue();
        } else {
            script.status = 500;
            script.error = QNetworkReply::UnknownNetworkError;
            script.errorText = QByteArrayLiteral("unexpected request");
        }

        if (outgoingData) {
            sentBodies.append(outgoingData->readAll());
        } else {
            sentBodies.append(QByteArray());
        }

        return new ScriptedReply(request, operation, script, this);
    }
};

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(bytes) == bytes.size();
}

bool writeLog(const QString &dir, const QByteArray &body)
{
    const QString path = QDir(dir).filePath(QStringLiteral(
        "Artemis-1700000000.log"));
    return writeFile(path, body);
}

bool writeCrash(const QString &dir, const QByteArray &body)
{
    const QString path = QDir(dir).filePath(QStringLiteral(
        "Artemis-crash-1700000000000.txt"));
    return writeFile(path, body);
}

const QByteArray kSmallLog =
    "00:00:01.000 - Info - Compiled with SDL 2.28.5\n"
    "00:00:01.020 - Info - GPU: NVIDIA GeForce RTX 4090 (driver 555.42.06)\n"
    "00:00:01.040 - Info - Selected codec: HEVC Main10, HDR enabled, bitrate 50 Mbps\n"
    "00:00:01.050 - Info - SDL Video driver: wayland\n"
    "00:00:02.000 - Info - Loaded unique ID from settings: deadbeefcafebabe\n"
    "00:00:02.100 - Info - Found unexpected PC SALVATION-19 looking for DESKTOP-PC-42\n"
    "00:00:02.500 - Info - DESKTOP-PC-42 is now online at 192.168.1.50:47984\n"
    "00:00:03.000 - Info - Qt Warning: QSocketNotifier: Failed to register socket notifier\n";

const QByteArray kSmallCrash =
    "Signal: SIGSEGV (11)\n"
    "Fault address: 0x0000000000000010\n"
    "Build: 0.6.7 aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
    "Frames:\n"
    "  #0 0x00007f8a + 0x0042 /usr/bin/artemis\n";

// Resolves the gzip-family decoder. Returns the absolute path of "gzip"
// if installed, or "gunzip" as a fallback. Returns an empty string when
// neither binary is on PATH.
QString resolveGzipDecoder()
{
    const QString gzipPath =
        QStandardPaths::findExecutable(QStringLiteral("gzip"));
    if (!gzipPath.isEmpty()) {
        return gzipPath;
    }
    return QStandardPaths::findExecutable(QStringLiteral("gunzip"));
}

// Invokes `decoder -n -d -c <gzPath>` (or `decoder -c <gzPath>` for
// gunzip-style tools) and returns the stdout. Sets *exitCode and
// *errorMessage when the process could not be started, finished, or
// exited normally.
QByteArray runGzipDecoder(const QString &decoder,
                          const QString &gzPath,
                          int *exitCode,
                          QString *errorMessage)
{
    QStringList arguments;
    const bool looksLikeGunzip =
        decoder.endsWith(QStringLiteral("/gunzip"))
        || decoder.endsWith(QStringLiteral("gunzip"));
    if (looksLikeGunzip) {
        arguments << QStringLiteral("-c") << gzPath;
    } else {
        arguments << QStringLiteral("-n") << QStringLiteral("-d")
                  << QStringLiteral("-c") << gzPath;
    }

    QProcess proc;
    proc.start(decoder, arguments);
    if (!proc.waitForStarted(5000)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Could not start %1: %2")
                                .arg(decoder, proc.errorString());
        }
        return QByteArray();
    }
    if (!proc.waitForFinished(30000)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("%1 timed out: %2")
                                .arg(decoder, proc.errorString());
        }
        return QByteArray();
    }
    if (exitCode) {
        *exitCode = proc.exitCode();
    }
    if (errorMessage) {
        *errorMessage = QString::fromUtf8(proc.readAllStandardError());
    }
    return proc.readAllStandardOutput();
}

} // namespace

class DiagnosticReporterTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void bundleUnderCap();
    void truncationPreservesStructuredHead();
    void uploadFailureFallsBackToFile();
    void previewByteIdenticalToUploadedPayload();
    void noteIsTruncatedToCodeUnits();
    void previewReflectsNoteTruncation();
    void crashSelectionSkipsEmptyNewestFile();
    void previewPartsExposeStructuredHeadAndRawText();

    // Direct coverage of the production gzip compressor. Each case calls
    // DiagnosticReporter::gzipCompress(input) via friend access and
    // validates the resulting stream with the *system* gzip -d -c
    // decoder, so we never copy the production implementation into the
    // test. The independent decoder is required by the cross-platform
    // recovery contract.
    void gzipCompressProducesValidHeaderAndTrailer();
    void gzipCompressRoundTripsCanonicalAscii();
    void gzipCompressCarriesCrc32OfInput();
    void gzipCompressEmptyInputProducesValidStream();
    void gzipCompressRoundTripsBinaryWithNulsAndAllBytes();
    void gzipCompressRoundTripsLargeIncompressibleInput();
    void gzipCompressIsDeterministic();

private:
    QTemporaryDir m_LogDir;
    QTemporaryDir m_DownloadsDir;
};

void DiagnosticReporterTest::initTestCase()
{
    QVERIFY(m_LogDir.isValid());
    QVERIFY(m_DownloadsDir.isValid());
}

void DiagnosticReporterTest::bundleUnderCap()
{
    QVERIFY(writeLog(m_LogDir.path(), kSmallLog));
    QVERIFY(writeCrash(m_LogDir.path(), kSmallCrash));

    FakeNam network;
    network.enqueue(Script{QByteArrayLiteral("{\"id\":\"VBT-AAAAA\"}"),
                           200, QNetworkReply::NoError, QByteArray()});

    DiagnosticReporter reporter(&network,
                                m_LogDir.path(),
                                m_DownloadsDir.path());

    reporter.sendReport(QStringLiteral("note"));
    QTRY_COMPARE(reporter.state(), DiagnosticReporter::Sent);
    QCOMPARE(reporter.reportId(), QStringLiteral("VBT-AAAAA"));

    QCOMPARE(network.sentBodies.size(), 1);
    const QByteArray payload = network.sentBodies.constFirst();
    QVERIFY2(payload.size() < 2 * 1024 * 1024,
             qPrintable(QStringLiteral("Payload size over cap: %1")
                        .arg(payload.size())));
    QVERIFY2(payload.size() > 0,
             qPrintable(QStringLiteral("Empty payload sent.")));
}

void DiagnosticReporterTest::truncationPreservesStructuredHead()
{
    QByteArray hugeLog;
    hugeLog.reserve(4 * 1024 * 1024);
    quint32 seed = 0x12345678u;
    while (hugeLog.size() < 4 * 1024 * 1024) {
        seed = seed * 1664525u + 1013904223u;
        const QByteArray chunk = QByteArray::number(seed, 16)
            + QByteArrayLiteral(" - ")
            + QByteArray::number(static_cast<qulonglong>(seed) >> 7, 16)
            + QByteArrayLiteral(" - diagnostics line\n");
        hugeLog += chunk;
    }
    QVERIFY(writeLog(m_LogDir.path(), hugeLog));
    QVERIFY(writeCrash(m_LogDir.path(), kSmallCrash));

    FakeNam network;
    network.enqueue(Script{QByteArrayLiteral("{\"id\":\"VBT-BBBBB\"}"),
                           200, QNetworkReply::NoError, QByteArray()});

    DiagnosticReporter reporter(&network,
                                m_LogDir.path(),
                                m_DownloadsDir.path());

    const QString note = QStringLiteral("truncation test");
    reporter.sendReport(note);
    QTRY_COMPARE(reporter.state(), DiagnosticReporter::Sent);
    QCOMPARE(network.sentBodies.size(), 1);

    const QByteArray payload = network.sentBodies.constFirst();
    QVERIFY2(payload.size() <= 2 * 1024 * 1024,
             qPrintable(QStringLiteral("Payload still over cap: %1")
                        .arg(payload.size())));

    const QString decoder = resolveGzipDecoder();
    if (decoder.isEmpty()) {
        QSKIP("Neither gzip nor gunzip is on PATH; "
              "skipping decoder-based end-to-end check.");
    }

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString gzPath = tempDir.filePath(QStringLiteral("truncation.gz"));
    QVERIFY(writeFile(gzPath, payload));

    int exitCode = -1;
    QString stderrMessage;
    const QByteArray decompressed = runGzipDecoder(decoder, gzPath, &exitCode,
                                                   &stderrMessage);
    QVERIFY2(stderrMessage.isEmpty(), qPrintable(stderrMessage));
    QCOMPARE(exitCode, 0);
    QVERIFY2(!decompressed.isEmpty(),
             qPrintable(QStringLiteral("Could not inflate payload.")));

    const QJsonDocument doc = QJsonDocument::fromJson(decompressed);
    QVERIFY2(doc.isObject(),
             qPrintable(QStringLiteral("Decompressed payload was not a JSON object: ")
                        + decompressed));

    const QJsonObject root = doc.object();
    QVERIFY(root.contains(QStringLiteral("v")));
    QVERIFY(root.contains(QStringLiteral("app")));
    QVERIFY(root.contains(QStringLiteral("sys")));
    QVERIFY(root.contains(QStringLiteral("log")));
    QVERIFY(root.contains(QStringLiteral("crash")));
    QVERIFY(root.contains(QStringLiteral("note")));

    const QJsonObject app = root.value(QStringLiteral("app")).toObject();
    QCOMPARE(app.value(QStringLiteral("version")).toString(),
             QStringLiteral("0.6.7"));
    QCOMPARE(app.value(QStringLiteral("commit")).toString(),
             QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));

    const QJsonObject sys = root.value(QStringLiteral("sys")).toObject();
    QVERIFY(sys.contains(QStringLiteral("os")));
    QVERIFY(sys.contains(QStringLiteral("kernelType")));
    QVERIFY(sys.contains(QStringLiteral("kernelVersion")));
    QVERIFY(sys.contains(QStringLiteral("arch")));
    QVERIFY(sys.contains(QStringLiteral("abi")));

    QCOMPARE(root.value(QStringLiteral("note")).toString(), note);

    const QString logField = root.value(QStringLiteral("log")).toString();
    QVERIFY2(logField.size() < hugeLog.size(),
             qPrintable(QStringLiteral("Log field not truncated: size=%1")
                        .arg(logField.size())));
    QVERIFY2(logField.size() > 0,
             qPrintable(QStringLiteral("Log field unexpectedly empty.")));

    const QString crashField = root.value(QStringLiteral("crash")).toString();
    QVERIFY2(crashField.contains(QStringLiteral("SIGSEGV")),
             qPrintable(QStringLiteral("Crash field unexpectedly lost: ")
                        + crashField));
}

void DiagnosticReporterTest::uploadFailureFallsBackToFile()
{
    QVERIFY(writeLog(m_LogDir.path(), kSmallLog));
    QVERIFY(writeCrash(m_LogDir.path(), kSmallCrash));

    FakeNam network;
    Script failed;
    failed.status = 0;
    failed.error = QNetworkReply::ConnectionRefusedError;
    failed.errorText = QByteArrayLiteral("connection refused");
    network.enqueue(failed);

    DiagnosticReporter reporter(&network,
                                m_LogDir.path(),
                                m_DownloadsDir.path());

    reporter.sendReport(QStringLiteral("failure test"));
    QTRY_COMPARE(reporter.state(), DiagnosticReporter::Failed);
    QVERIFY(!reporter.errorMessage().isEmpty());

    QCOMPARE(network.sentBodies.size(), 1);
    QVERIFY(!network.sentBodies.constFirst().isEmpty());

    const QByteArray expectedPreview = reporter.buildPreview(
        QStringLiteral("failure test")).toUtf8();

    QVERIFY(reporter.saveToDownloads(QStringLiteral("failure test")));

    QDir dir(m_DownloadsDir.path());
    const QFileInfoList entries =
        dir.entryInfoList(QStringList() << QStringLiteral("vibertemis-diagnostic-*.json"),
                          QDir::Files | QDir::NoSymLinks);
    QCOMPARE(entries.size(), 1);

    QFile savedFile(entries.constFirst().filePath());
    QVERIFY(savedFile.open(QIODevice::ReadOnly));
    const QByteArray savedBytes = savedFile.readAll();
    QCOMPARE(savedBytes, expectedPreview);
}

void DiagnosticReporterTest::previewByteIdenticalToUploadedPayload()
{
    QVERIFY(writeLog(m_LogDir.path(), kSmallLog));
    QVERIFY(writeCrash(m_LogDir.path(), kSmallCrash));

    FakeNam network;
    network.enqueue(Script{QByteArrayLiteral("{\"id\":\"VBT-CCCCC\"}"),
                           200, QNetworkReply::NoError, QByteArray()});

    DiagnosticReporter reporter(&network,
                                m_LogDir.path(),
                                m_DownloadsDir.path());

    const QString note = QStringLiteral("preview identity check");
    const QString previewText = reporter.buildPreview(note);
    QVERIFY(!previewText.isEmpty());

    reporter.sendReport(note);
    QTRY_COMPARE(reporter.state(), DiagnosticReporter::Sent);
    QCOMPARE(network.sentBodies.size(), 1);

    const QByteArray uploaded = network.sentBodies.constFirst();
    QVERIFY2(uploaded.size() > 0,
             qPrintable(QStringLiteral("Empty body captured.")));

    QVERIFY2(uploaded.startsWith(QByteArrayLiteral("\x1f\x8b")),
             qPrintable(QStringLiteral(
                 "Uploaded payload does not start with the gzip magic bytes. "
                 "Bytes 0-1: ") + uploaded.left(2).toHex()));

    const QString decoder = resolveGzipDecoder();
    if (decoder.isEmpty()) {
        QSKIP("Neither gzip nor gunzip is on PATH; "
              "skipping decoder-based preview-identity check.");
    }

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString gzPath = tempDir.filePath(QStringLiteral("uploaded.gz"));
    QVERIFY(writeFile(gzPath, uploaded));

    int exitCode = -1;
    QString stderrMessage;
    const QByteArray decompressed = runGzipDecoder(decoder, gzPath, &exitCode,
                                                   &stderrMessage);
    QVERIFY2(stderrMessage.isEmpty(), qPrintable(stderrMessage));
    QCOMPARE(exitCode, 0);
    QCOMPARE(decompressed, previewText.toUtf8());
}

void DiagnosticReporterTest::noteIsTruncatedToCodeUnits()
{
    QVERIFY(writeLog(m_LogDir.path(), kSmallLog));

    FakeNam network;
    DiagnosticReporter reporter(&network,
                                m_LogDir.path(),
                                m_DownloadsDir.path());

    QString longNote;
    longNote.reserve(500);
    for (int i = 0; i < 500; ++i) {
        longNote.append(QLatin1Char('x'));
    }
    const QString preview = reporter.buildPreview(longNote);
    const QJsonObject root =
        QJsonDocument::fromJson(preview.toUtf8()).object();
    const QString serializedNote = root.value(QStringLiteral("note")).toString();
    QCOMPARE(serializedNote.size(), 280);
}

void DiagnosticReporterTest::previewReflectsNoteTruncation()
{
    QVERIFY(writeLog(m_LogDir.path(), kSmallLog));

    FakeNam network;
    DiagnosticReporter reporter(&network,
                                m_LogDir.path(),
                                m_DownloadsDir.path());

    QString note;
    note.reserve(300);
    for (int i = 0; i < 300; ++i) {
        note.append(QLatin1Char('y'));
    }
    const QString preview = reporter.buildPreview(note);
    QVERIFY(preview.contains(QStringLiteral("\"note\": \"yyy")));
    QVERIFY(preview.contains(QString(280, QLatin1Char('y'))));
}

void DiagnosticReporterTest::crashSelectionSkipsEmptyNewestFile()
{
    // Seed the log dir with the older, non-empty crash file first.
    QVERIFY(writeCrash(m_LogDir.path(), kSmallCrash));

    // Sleep long enough to cross the 1-second mtime-resolution boundary so
    // the newer empty file's mtime is genuinely later than the older one's,
    // regardless of any filesystem with 1-second mtime granularity.
    QTest::qSleep(1100);

    // Write a newer, lexically-later crash file that is empty. CrashHandler
    // unconditionally creates an empty file on every launch, and pickNewest
    // must skip it so the previous run's real crash is reported.
    const QString emptyCrashPath = QDir(m_LogDir.path()).filePath(QStringLiteral(
        "Artemis-crash-1800000000000.txt"));
    {
        QFile emptyCrash(emptyCrashPath);
        QVERIFY(emptyCrash.open(QIODevice::WriteOnly | QIODevice::Truncate));
        // Leave the file empty on purpose; do not write any bytes.
        emptyCrash.close();
        QVERIFY(QFileInfo(emptyCrashPath).size() == 0);
    }

    QVERIFY(writeLog(m_LogDir.path(), kSmallLog));

    FakeNam network;
    DiagnosticReporter reporter(&network,
                                m_LogDir.path(),
                                m_DownloadsDir.path());

    const QString preview = reporter.buildPreview(QString());
    const QJsonDocument doc = QJsonDocument::fromJson(preview.toUtf8());
    QVERIFY2(doc.isObject(),
             qPrintable(QStringLiteral("Preview was not a JSON object: ")
                        + preview));

    const QJsonObject root = doc.object();
    QVERIFY(root.contains(QStringLiteral("crash")));
    const QJsonValue crashValue = root.value(QStringLiteral("crash"));
    QVERIFY2(!crashValue.isNull(),
             qPrintable(QStringLiteral("Crash field unexpectedly null; "
                                       "the empty newer crash file was "
                                       "selected instead of the older "
                                       "non-empty one.")));
    QVERIFY2(crashValue.isString(),
             qPrintable(QStringLiteral("Crash field was not a string: ")
                        + crashValue.toString()));
    const QString crashText = crashValue.toString();
    QVERIFY2(!crashText.isEmpty(),
             qPrintable(QStringLiteral("Crash field was empty.")));
    QCOMPARE(crashText, QString::fromUtf8(kSmallCrash));
}

void DiagnosticReporterTest::previewPartsExposeStructuredHeadAndRawText()
{
    QVERIFY(writeLog(m_LogDir.path(), kSmallLog));
    QVERIFY(writeCrash(m_LogDir.path(), kSmallCrash));

    FakeNam network;
    DiagnosticReporter reporter(&network,
                                m_LogDir.path(),
                                m_DownloadsDir.path());

    const QString note = QStringLiteral("structured preview test");
    const QVariantMap parts = reporter.buildPreviewParts(note);

    QVERIFY(parts.contains(QStringLiteral("version")));
    QVERIFY(parts.contains(QStringLiteral("commit")));
    QVERIFY(parts.contains(QStringLiteral("os")));
    QVERIFY(parts.contains(QStringLiteral("kernelType")));
    QVERIFY(parts.contains(QStringLiteral("kernelVersion")));
    QVERIFY(parts.contains(QStringLiteral("arch")));
    QVERIFY(parts.contains(QStringLiteral("abi")));
    QVERIFY(!parts.value(QStringLiteral("os")).toString().isEmpty());

    QCOMPARE(parts.value(QStringLiteral("note")).toString(), note);
    QCOMPARE(parts.value(QStringLiteral("hasCrash")).toBool(), true);

    const QString logText = parts.value(QStringLiteral("logText")).toString();
    QVERIFY(!logText.isEmpty());
    QVERIFY(!logText.contains(QLatin1Char('\\')) || !logText.contains(QStringLiteral("\n")));

    const QString crashText = parts.value(QStringLiteral("crashText")).toString();
    QCOMPARE(crashText, QString::fromUtf8(kSmallCrash));

    // The structured accessor must return the SAME underlying log/crash
    // content as buildPreview's JSON "log"/"crash" fields (modulo JSON
    // string escaping), proving it is not a second, divergent code path.
    const QString jsonPreview = reporter.buildPreview(note);
    const QJsonObject root =
        QJsonDocument::fromJson(jsonPreview.toUtf8()).object();
    QCOMPARE(root.value(QStringLiteral("crash")).toString(), crashText);
}

// Direct production-compressor coverage.
//
// Tests in this block invoke DiagnosticReporter::gzipCompress() through
// friend access. Each one decodes the resulting stream with the system
// gzip binary, the source of truth for the gzip format. There is no copy
// of the production CRC32 table in this test; if the production stream
// ever drifts from RFC 1952, gzip -d will reject it.
//
// QSKIP when neither gzip nor gunzip is on PATH so the suite still runs
// on minimal CI images.

void DiagnosticReporterTest::gzipCompressProducesValidHeaderAndTrailer()
{
    // Friend access into the private static production compressor.
    const QByteArray payload = DiagnosticReporter::gzipCompress(
        QByteArrayLiteral("123456789"));

    QVERIFY2(payload.size() >= 20,
             qPrintable(QStringLiteral(
                 "Compressed payload too short: %1 bytes").arg(payload.size())));

    QCOMPARE(static_cast<int>(static_cast<quint8>(payload[0])), 0x1f);
    QCOMPARE(static_cast<int>(static_cast<quint8>(payload[1])), 0x8b);
    QCOMPARE(static_cast<int>(static_cast<quint8>(payload[2])), 0x08);
    QCOMPARE(static_cast<int>(static_cast<quint8>(payload[3])), 0x00);
    QCOMPARE(static_cast<int>(static_cast<quint8>(payload[4])), 0x00);
    QCOMPARE(static_cast<int>(static_cast<quint8>(payload[5])), 0x00);
    QCOMPARE(static_cast<int>(static_cast<quint8>(payload[6])), 0x00);
    QCOMPARE(static_cast<int>(static_cast<quint8>(payload[7])), 0x00);
    QCOMPARE(static_cast<int>(static_cast<quint8>(payload[8])), 0x00);
    QCOMPARE(static_cast<int>(static_cast<quint8>(payload[9])), 0xff);
}

void DiagnosticReporterTest::gzipCompressRoundTripsCanonicalAscii()
{
    const QString decoder = resolveGzipDecoder();
    if (decoder.isEmpty()) {
        QSKIP("Neither gzip nor gunzip is on PATH; "
              "skipping round-trip test.");
    }

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString gzPath = tempDir.filePath(QStringLiteral("canonical.gz"));
    const QByteArray input = QByteArrayLiteral("123456789");

    const QByteArray payload = DiagnosticReporter::gzipCompress(input);
    QVERIFY2(!payload.isEmpty(),
             qPrintable(QStringLiteral("gzipCompress returned empty.")));
    QVERIFY(writeFile(gzPath, payload));

    int exitCode = -1;
    QString stderrMessage;
    const QByteArray decoded = runGzipDecoder(decoder, gzPath, &exitCode,
                                              &stderrMessage);
    QVERIFY2(stderrMessage.isEmpty(), qPrintable(stderrMessage));
    QCOMPARE(exitCode, 0);
    QCOMPARE(decoded, input);
}

void DiagnosticReporterTest::gzipCompressCarriesCrc32OfInput()
{
    const QString decoder = resolveGzipDecoder();
    if (decoder.isEmpty()) {
        QSKIP("Neither gzip nor gunzip is on PATH; "
              "skipping CRC trailer test.");
    }

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString gzPath = tempDir.filePath(QStringLiteral("crc-vector.gz"));

    // Canonical ZIP/PNG/gzip CRC32 test vector: 0xCBF43926 for
    // "123456789". The trailer in our output must carry this value in
    // little-endian order when the input is exactly this string.
    const QByteArray input = QByteArrayLiteral("123456789");
    const QByteArray payload = DiagnosticReporter::gzipCompress(input);
    QVERIFY(payload.size() >= 20);
    QVERIFY(writeFile(gzPath, payload));

    const auto *p = reinterpret_cast<const quint8 *>(payload.constData());
    const auto readLe32 = [&p](int off) {
        return static_cast<quint32>(p[off])
            | (static_cast<quint32>(p[off + 1]) << 8)
            | (static_cast<quint32>(p[off + 2]) << 16)
            | (static_cast<quint32>(p[off + 3]) << 24);
    };
    const quint32 trailerCrc = readLe32(payload.size() - 8);
    const quint32 trailerIsize = readLe32(payload.size() - 4);

    QCOMPARE(trailerCrc, quint32(0xCBF43926u));
    QCOMPARE(trailerIsize, quint32(9u));

    int exitCode = -1;
    QString stderrMessage;
    const QByteArray decoded = runGzipDecoder(decoder, gzPath, &exitCode,
                                              &stderrMessage);
    QVERIFY2(stderrMessage.isEmpty(), qPrintable(stderrMessage));
    QCOMPARE(exitCode, 0);
    QCOMPARE(decoded, input);
}

void DiagnosticReporterTest::gzipCompressEmptyInputProducesValidStream()
{
    const QString decoder = resolveGzipDecoder();
    if (decoder.isEmpty()) {
        QSKIP("Neither gzip nor gunzip is on PATH; "
              "skipping empty-input test.");
    }

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString gzPath = tempDir.filePath(QStringLiteral("empty.gz"));

    const QByteArray payload = DiagnosticReporter::gzipCompress(QByteArray());
    QCOMPARE(payload.size(), 20);

    QVERIFY(writeFile(gzPath, payload));

    int exitCode = -1;
    QString stderrMessage;
    const QByteArray decoded = runGzipDecoder(decoder, gzPath, &exitCode,
                                              &stderrMessage);
    QVERIFY2(stderrMessage.isEmpty(), qPrintable(stderrMessage));
    QCOMPARE(exitCode, 0);
    QVERIFY2(decoded.isEmpty(),
             qPrintable(QStringLiteral("Empty input did not decode to "
                                       "empty output: %1 bytes")
                        .arg(decoded.size())));

    const auto *p = reinterpret_cast<const quint8 *>(payload.constData());
    for (int i = 12; i < 20; ++i) {
        QCOMPARE(static_cast<int>(p[i]), 0x00);
    }
}

void DiagnosticReporterTest::gzipCompressRoundTripsBinaryWithNulsAndAllBytes()
{
    const QString decoder = resolveGzipDecoder();
    if (decoder.isEmpty()) {
        QSKIP("Neither gzip nor gunzip is on PATH; skipping binary-input "
              "round-trip test.");
    }

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString gzPath = tempDir.filePath(QStringLiteral("binary.gz"));

    // A deterministic input that exercises every byte value, plenty of
    // NULs, and a long run of repeated bytes that defeats naive LZ77
    // heuristics. 8192 bytes is plenty to cover all 256 byte values and
    // remains small enough that the test runs in under a second.
    QByteArray input;
    input.reserve(8192);
    for (int i = 0; i < 8192; ++i) {
        const int b = (i * 73 + 11) & 0xff;
        input.append(static_cast<char>(b));
    }
    // Inject a few NUL runs that gzip's deflate stores verbatim.
    for (int i = 0; i < 4; ++i) {
        const int offset = (i * 2048) + 17;
        for (int j = 0; j < 32; ++j) {
            input[offset + j] = '\0';
        }
    }

    const QByteArray payload = DiagnosticReporter::gzipCompress(input);
    QVERIFY2(!payload.isEmpty(),
             qPrintable(QStringLiteral("gzipCompress returned empty for "
                                       "%1-byte binary input.")
                        .arg(input.size())));
    QVERIFY(writeFile(gzPath, payload));

    int exitCode = -1;
    QString errorMessage;
    const QByteArray decoded = runGzipDecoder(decoder, gzPath, &exitCode,
                                              &errorMessage);
    QVERIFY2(errorMessage.isEmpty(), qPrintable(errorMessage));
    QCOMPARE(exitCode, 0);
    QCOMPARE(decoded, input);
}

void DiagnosticReporterTest::gzipCompressRoundTripsLargeIncompressibleInput()
{
    const QString decoder = resolveGzipDecoder();
    if (decoder.isEmpty()) {
        QSKIP("Neither gzip nor gunzip is on PATH; skipping large-input "
              "round-trip test.");
    }

    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString gzPath = tempDir.filePath(QStringLiteral("large.gz"));

    // 256 KiB of pseudo-random bytes that no LZ77 will compress; this
    // exercises the deflate bounded-output path of qCompress and the
    // trailer arithmetic at the upper end of the input size.
    QByteArray input;
    input.reserve(256 * 1024);
    quint32 seed = 0xc0ffee01u;
    for (int i = 0; i < 256 * 1024; ++i) {
        seed = seed * 1664525u + 1013904223u;
        input.append(static_cast<char>((seed >> 16) & 0xff));
    }

    const QByteArray payload = DiagnosticReporter::gzipCompress(input);
    QVERIFY2(!payload.isEmpty(),
             qPrintable(QStringLiteral("gzipCompress returned empty for "
                                       "%1-byte input.")
                        .arg(input.size())));
    QVERIFY(writeFile(gzPath, payload));

    int exitCode = -1;
    QString errorMessage;
    const QByteArray decoded = runGzipDecoder(decoder, gzPath, &exitCode,
                                              &errorMessage);
    QVERIFY2(errorMessage.isEmpty(), qPrintable(errorMessage));
    QCOMPARE(exitCode, 0);
    QCOMPARE(decoded.size(), input.size());
    QCOMPARE(decoded, input);

    // ISIZE must equal input.size() modulo 2^32. For inputs that fit in
    // 32 bits this is just input.size() in little-endian.
    const auto *p = reinterpret_cast<const quint8 *>(payload.constData());
    const quint32 trailerIsize =
        static_cast<quint32>(p[payload.size() - 4])
        | (static_cast<quint32>(p[payload.size() - 3]) << 8)
        | (static_cast<quint32>(p[payload.size() - 2]) << 16)
        | (static_cast<quint32>(p[payload.size() - 1]) << 24);
    QCOMPARE(static_cast<quint64>(trailerIsize),
             static_cast<quint64>(input.size()));
}

void DiagnosticReporterTest::gzipCompressIsDeterministic()
{
    // Identical input must produce byte-identical output. The fixed
    // MTIME=0 header and the deterministic CRC32 + ISIZE trailer are
    // the only stable parts of the stream; the deflate payload from
    // qCompress is also deterministic, so we can compare full streams.
    const QByteArray input = QByteArrayLiteral("deterministic gzip input");
    const QByteArray first = DiagnosticReporter::gzipCompress(input);
    const QByteArray second = DiagnosticReporter::gzipCompress(input);
    const QByteArray third = DiagnosticReporter::gzipCompress(input);
    QCOMPARE(first, second);
    QCOMPARE(first, third);

    QVERIFY(first.size() >= 10);
    for (int i = 4; i <= 7; ++i) {
        QCOMPARE(static_cast<int>(static_cast<quint8>(first[i])), 0x00);
    }
}

QTEST_GUILESS_MAIN(DiagnosticReporterTest)

#include "tst_diagnosticreporter.moc"