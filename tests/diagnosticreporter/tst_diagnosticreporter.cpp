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
#include <QQueue>
#include <QTemporaryDir>
#include <QTimer>

#include <zlib.h>

#include "backend/diagnosticreporter.h"
#include "backend/logscrubber.h"

namespace {

QByteArray gzipInflate(const QByteArray &input)
{
    z_stream stream{};
    if (inflateInit2(&stream, MAX_WBITS + 16) != Z_OK) {
        return QByteArray();
    }
    QByteArray output;
    output.resize(static_cast<int>(input.size() * 6 + 64));
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.constData()));
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());
    int ret = Z_BUF_ERROR;
    while (stream.avail_in > 0) {
        ret = inflate(&stream, Z_NO_FLUSH);
        if (ret == Z_STREAM_END) {
            break;
        }
        if (ret != Z_OK && ret != Z_BUF_ERROR) {
            inflateEnd(&stream);
            return QByteArray();
        }
        if (stream.avail_out == 0) {
            const int old = output.size();
            output.resize(old * 2);
            stream.next_out = reinterpret_cast<Bytef*>(output.data() + old);
            stream.avail_out = static_cast<uInt>(output.size() - old);
        }
    }
    if (ret != Z_STREAM_END) {
        inflateEnd(&stream);
        return QByteArray();
    }
    output.resize(static_cast<int>(stream.total_out));
    inflateEnd(&stream);
    return output;
}

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

    const QByteArray decompressed = gzipInflate(payload);
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

    const QByteArray decompressed = gzipInflate(uploaded);
    QVERIFY2(!decompressed.isEmpty(),
             qPrintable(QStringLiteral("Inflation of uploaded payload failed.")));

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

QTEST_GUILESS_MAIN(DiagnosticReporterTest)

#include "tst_diagnosticreporter.moc"