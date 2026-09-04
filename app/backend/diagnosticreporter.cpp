#include "diagnosticreporter.h"

#include "logscrubber.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileInfoList>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QSysInfo>
#include <QStandardPaths>
#include <QUrl>

#include <limits>

#include <zlib.h>

#ifndef VBT_DIAG_ENDPOINT
#define VBT_DIAG_ENDPOINT "https://crash-endpoint.workers.dev/v1/report"
#endif

namespace {

constexpr qint64 kLogTailByteCap = 256 * 1024;
constexpr qint64 kCompressedBundleByteCap = 2 * 1024 * 1024;
constexpr int kNoteCodeUnits = 280;
constexpr int kConnectTimeoutMs = 10000;
constexpr int kIdleTimeoutMs = 15000;
constexpr int kOverallTimeoutMs = 30000;
constexpr qint64 kReplyReadBufferSize = 64 * 1024;
constexpr qint64 kReplyChunkSize = 16 * 1024;
constexpr qint64 kReplyBodyCap = 64 * 1024;

QByteArray gzipCompressImpl(const QByteArray &input)
{
    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                     MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return QByteArray();
    }
    QByteArray output;
    output.resize(static_cast<int>(deflateBound(&stream,
                                               static_cast<uLong>(input.size()))));
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(input.constData()));
    stream.avail_in = static_cast<uInt>(input.size());
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());
    int ret = deflate(&stream, Z_FINISH);
    deflateEnd(&stream);
    if (ret != Z_STREAM_END) {
        return QByteArray();
    }
    output.resize(static_cast<int>(stream.total_out));
    return output;
}

bool isLogFileName(const QString &name)
{
    return name.startsWith(QStringLiteral("Artemis-"))
        && name.endsWith(QStringLiteral(".log"))
        && !name.contains(QStringLiteral("crash"));
}

bool isCrashFileName(const QString &name)
{
    return name.startsWith(QStringLiteral("Artemis-"))
        && name.endsWith(QStringLiteral(".txt"))
        && name.contains(QStringLiteral("crash"));
}

QString pickNewest(const QDir &dir, bool (*match)(const QString &))
{
    const QFileInfoList entries =
        dir.entryInfoList(QDir::Files | QDir::NoSymLinks, QDir::Time);
    for (const QFileInfo &info : entries) {
        if (match(info.fileName())) {
            return info.fileName();
        }
    }
    return QString();
}

QByteArray tailBytes(const QString &path, qint64 byteCap)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    const qint64 size = file.size();
    if (size <= byteCap) {
        return file.readAll();
    }
    if (!file.seek(size - byteCap)) {
        return QByteArray();
    }
    return file.read(byteCap);
}

QString truncateToCodeUnits(const QString &note, int max)
{
    if (note.size() <= max) {
        return note;
    }
    return note.left(max);
}

} // namespace

DiagnosticReporter::DiagnosticReporter(QObject *parent) :
    QObject(parent),
    m_Nam(nullptr),
    m_Reply(nullptr),
    m_State(Idle)
{
    m_OwnedNam.reset(new QNetworkAccessManager(this));
    m_Nam = m_OwnedNam.data();
    m_DownloadsDir = QStandardPaths::writableLocation(
        QStandardPaths::DownloadLocation);
    m_ConnectTimer.setSingleShot(true);
    m_IdleTimer.setSingleShot(true);
    m_OverallTimer.setSingleShot(true);
    connect(&m_ConnectTimer, &QTimer::timeout,
            this, &DiagnosticReporter::handleConnectTimeout);
    connect(&m_IdleTimer, &QTimer::timeout,
            this, &DiagnosticReporter::handleIdleTimeout);
    connect(&m_OverallTimer, &QTimer::timeout,
            this, &DiagnosticReporter::handleOverallTimeout);
}

DiagnosticReporter::DiagnosticReporter(QNetworkAccessManager *network,
                                       const QString &logDir,
                                       const QString &downloadsDir,
                                       QObject *parent) :
    QObject(parent),
    m_Nam(network),
    m_Reply(nullptr),
    m_State(Idle)
{
    Q_ASSERT(m_Nam);
    m_LogDir = logDir;
    m_DownloadsDir = downloadsDir;
    m_ConnectTimer.setSingleShot(true);
    m_IdleTimer.setSingleShot(true);
    m_OverallTimer.setSingleShot(true);
    connect(&m_ConnectTimer, &QTimer::timeout,
            this, &DiagnosticReporter::handleConnectTimeout);
    connect(&m_IdleTimer, &QTimer::timeout,
            this, &DiagnosticReporter::handleIdleTimeout);
    connect(&m_OverallTimer, &QTimer::timeout,
            this, &DiagnosticReporter::handleOverallTimeout);
}

DiagnosticReporter::~DiagnosticReporter()
{
    m_ConnectTimer.stop();
    m_IdleTimer.stop();
    m_OverallTimer.stop();
    if (m_Reply) {
        QNetworkReply *reply = m_Reply;
        m_Reply = nullptr;
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
}

DiagnosticReporter::State DiagnosticReporter::state() const { return m_State; }
QString DiagnosticReporter::reportId() const { return m_ReportId; }
QString DiagnosticReporter::errorMessage() const { return m_ErrorMessage; }

qint64 DiagnosticReporter::logTailByteCap() { return kLogTailByteCap; }
qint64 DiagnosticReporter::compressedBundleByteCap()
{
    return kCompressedBundleByteCap;
}
int DiagnosticReporter::maximumNoteCodeUnits() { return kNoteCodeUnits; }
QByteArray DiagnosticReporter::gzipCompress(const QByteArray &input)
{
    return gzipCompressImpl(input);
}

void DiagnosticReporter::setState(State state)
{
    if (m_State == state) {
        return;
    }
    m_State = state;
    emit stateChanged();
}

void DiagnosticReporter::setReportId(const QString &id)
{
    if (m_ReportId == id) {
        return;
    }
    m_ReportId = id;
    emit reportIdChanged();
}

void DiagnosticReporter::setError(const QString &message)
{
    m_ErrorMessage = message;
    emit errorChanged();
}

void DiagnosticReporter::clearError()
{
    if (m_ErrorMessage.isEmpty()) {
        return;
    }
    m_ErrorMessage.clear();
    emit errorChanged();
}

QString DiagnosticReporter::mostRecentLogSuffix() const
{
    if (m_LogDir.isEmpty()) {
        return QString();
    }
    QDir dir(m_LogDir);
    if (!dir.exists()) {
        return QString();
    }
    return pickNewest(dir, &isLogFileName);
}

QString DiagnosticReporter::mostRecentCrashSuffix() const
{
    if (m_LogDir.isEmpty()) {
        return QString();
    }
    QDir dir(m_LogDir);
    if (!dir.exists()) {
        return QString();
    }
    return pickNewest(dir, &isCrashFileName);
}

QByteArray DiagnosticReporter::readTailBytes(const QString &fileName,
                                             qint64 byteCap) const
{
    if (m_LogDir.isEmpty()) {
        return QByteArray();
    }
    const QString path = QDir(m_LogDir).filePath(fileName);
    return tailBytes(path, byteCap);
}

QByteArray DiagnosticReporter::readLogTail(qint64 byteCap, bool *ok) const
{
    const QString name = mostRecentLogSuffix();
    if (name.isEmpty()) {
        if (ok) *ok = false;
        return QByteArray();
    }
    const QByteArray raw = readTailBytes(name, byteCap);
    if (raw.isEmpty()) {
        if (ok) *ok = false;
        return QByteArray();
    }
    QString text = QString::fromUtf8(raw);
    text = LogScrubber::scrub(text);
    if (ok) *ok = true;
    return text.toUtf8();
}

QString DiagnosticReporter::readLatestCrashFile(bool *found) const
{
    const QString name = mostRecentCrashSuffix();
    if (name.isEmpty()) {
        if (found) *found = false;
        return QString();
    }
    const QByteArray raw = readTailBytes(name, std::numeric_limits<qint64>::max());
    if (raw.isEmpty()) {
        if (found) *found = false;
        return QString();
    }
    QString text = QString::fromUtf8(raw);
    text = LogScrubber::scrub(text);
    if (found) *found = true;
    return text;
}

QByteArray DiagnosticReporter::buildBundleJson(const QString &note,
                                              qint64 logByteCap) const
{
    const QString trimmedNote = truncateToCodeUnits(note, kNoteCodeUnits);

    bool logOk = false;
    const QByteArray logBytes = readLogTail(logByteCap, &logOk);
    const QString logText = logOk ? QString::fromUtf8(logBytes) : QString();

    bool crashFound = false;
    const QString crashText = readLatestCrashFile(&crashFound);

    QJsonObject appObject;
    appObject.insert(QStringLiteral("version"),
                     QString::fromLatin1(VERSION_STR));
    appObject.insert(QStringLiteral("commit"),
                     QString::fromLatin1(VIBERTEMIS_BUILD_COMMIT));

    QJsonObject sysObject;
    sysObject.insert(QStringLiteral("os"),
                     QSysInfo::prettyProductName());
    sysObject.insert(QStringLiteral("kernelType"),
                     QSysInfo::kernelType());
    sysObject.insert(QStringLiteral("kernelVersion"),
                     QSysInfo::kernelVersion());
    sysObject.insert(QStringLiteral("arch"),
                     QSysInfo::currentCpuArchitecture());
    sysObject.insert(QStringLiteral("abi"),
                     QSysInfo::buildAbi());

    QJsonObject root;
    root.insert(QStringLiteral("v"), 1);
    root.insert(QStringLiteral("app"), appObject);
    root.insert(QStringLiteral("sys"), sysObject);
    root.insert(QStringLiteral("log"), logText);
    if (crashFound) {
        root.insert(QStringLiteral("crash"), crashText);
    } else {
        root.insert(QStringLiteral("crash"), QJsonValue());
    }
    root.insert(QStringLiteral("note"), trimmedNote);

    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

QString DiagnosticReporter::buildPreview(const QString &note)
{
    return QString::fromUtf8(buildBundleJson(note, kLogTailByteCap));
}

bool DiagnosticReporter::saveToDownloads(const QString &note)
{
    if (m_DownloadsDir.isEmpty()) {
        m_DownloadsDir = QStandardPaths::writableLocation(
            QStandardPaths::DownloadLocation);
    }
    if (m_DownloadsDir.isEmpty()) {
        return false;
    }
    QDir().mkpath(m_DownloadsDir);
    const QString fileName = QStringLiteral("vibertemis-diagnostic-%1.json")
        .arg(QDateTime::currentSecsSinceEpoch());
    const QString path = QDir(m_DownloadsDir).filePath(fileName);

    const QByteArray json = buildBundleJson(note, kLogTailByteCap);

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    if (file.write(json) != json.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

void DiagnosticReporter::sendReport(const QString &note)
{
    if (m_State == Sending) {
        return;
    }
    if (m_Reply) {
        return;
    }
    clearError();
    setReportId(QString());

    qint64 logByteCap = kLogTailByteCap;
    QByteArray json;
    QByteArray payload;
    while (true) {
        json = buildBundleJson(note, logByteCap);
        payload = gzipCompressImpl(json);
        if (payload.isEmpty()) {
            setState(Failed);
            setError(QStringLiteral("Failed to compress the diagnostic report."));
            return;
        }
        if (payload.size() <= kCompressedBundleByteCap) {
            break;
        }
        if (logByteCap == 0) {
            break;
        }
        logByteCap = logByteCap / 2;
    }

    if (payload.size() > kCompressedBundleByteCap) {
        setState(Failed);
        setError(QStringLiteral("The diagnostic report exceeds the 2 MB cap."));
        return;
    }

    m_Payload = payload;
    startUpload();
}

void DiagnosticReporter::startUpload()
{
    setState(Sending);
    m_ReplyBody.clear();

    QNetworkRequest request(QUrl(QString::fromLatin1(VBT_DIAG_ENDPOINT)));
    request.setRawHeader(QByteArrayLiteral("X-Vbt-Ver"),
                         QByteArrayLiteral(VERSION_STR));
    request.setRawHeader(QByteArrayLiteral("Content-Encoding"),
                         QByteArrayLiteral("gzip"));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QByteArrayLiteral("application/json"));
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute,
                         QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute,
                         QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);

    m_Reply = m_Nam->put(request, m_Payload);
    m_Reply->setReadBufferSize(kReplyReadBufferSize);
    connect(m_Reply, &QIODevice::readyRead,
            this, &DiagnosticReporter::handleReplyReadyRead);
    connect(m_Reply, &QNetworkReply::finished,
            this, &DiagnosticReporter::handleReplyFinished);
    m_ConnectTimer.start(kConnectTimeoutMs);
    m_IdleTimer.start(kIdleTimeoutMs);
    m_OverallTimer.start(kOverallTimeoutMs);
}

void DiagnosticReporter::handleReplyReadyRead()
{
    if (!m_Reply) {
        return;
    }
    m_ConnectTimer.stop();
    m_IdleTimer.start(kIdleTimeoutMs);
    while (m_Reply && m_Reply->bytesAvailable() > 0) {
        if (m_ReplyBody.size() >= kReplyBodyCap) {
            char sink[kReplyChunkSize];
            const qint64 read = m_Reply->read(sink, kReplyChunkSize);
            if (read <= 0) {
                return;
            }
            continue;
        }
        const qint64 requested = qMin<qint64>(
            kReplyChunkSize,
            kReplyBodyCap - m_ReplyBody.size() + 1);
        const QByteArray chunk = m_Reply->read(requested);
        if (chunk.isEmpty()) {
            return;
        }
        if (chunk.size() > kReplyBodyCap - m_ReplyBody.size()) {
            failUpload(QStringLiteral("The diagnostic endpoint response was too large."));
            return;
        }
        m_ReplyBody += chunk;
    }
}

void DiagnosticReporter::handleReplyFinished()
{
    if (!m_Reply) {
        return;
    }

    QNetworkReply *reply = m_Reply;
    m_ConnectTimer.stop();
    m_IdleTimer.stop();
    m_OverallTimer.stop();

    handleReplyReadyRead();

    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QByteArray body = m_ReplyBody;
    m_ReplyBody.clear();

    m_Reply = nullptr;
    reply->deleteLater();

    if (networkError != QNetworkReply::NoError) {
        failUpload(QStringLiteral("Network error %1.")
                   .arg(static_cast<int>(networkError)));
        return;
    }
    if (status < 200 || status > 299) {
        failUpload(QStringLiteral("The diagnostic endpoint returned HTTP %1.")
                   .arg(status));
        return;
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        failUpload(QStringLiteral("The diagnostic endpoint response was malformed."));
        return;
    }
    const QString id = doc.object().value(QStringLiteral("id")).toString();
    if (id.isEmpty()) {
        failUpload(QStringLiteral("The diagnostic endpoint did not return an id."));
        return;
    }
    setReportId(id);
    setState(Sent);
}

void DiagnosticReporter::handleConnectTimeout()
{
    if (m_Reply) {
        failUpload(QStringLiteral("Timed out connecting to the diagnostic endpoint."));
    }
}

void DiagnosticReporter::handleIdleTimeout()
{
    if (m_Reply) {
        failUpload(QStringLiteral("The diagnostic endpoint stopped responding."));
    }
}

void DiagnosticReporter::handleOverallTimeout()
{
    if (m_Reply) {
        failUpload(QStringLiteral("The diagnostic upload timed out."));
    }
}

void DiagnosticReporter::failUpload(const QString &message)
{
    if (m_Reply) {
        QNetworkReply *reply = m_Reply;
        m_Reply = nullptr;
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
    m_ConnectTimer.stop();
    m_IdleTimer.stop();
    m_OverallTimer.stop();
    m_ReplyBody.clear();
    setState(Failed);
    setError(message);
}