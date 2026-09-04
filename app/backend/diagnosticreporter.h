#pragma once

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QScopedPointer>
#include <QString>
#include <QTimer>
#include <QVariantMap>

class QNetworkReply;

class DiagnosticReporter : public QObject
{
    Q_OBJECT
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString reportId READ reportId NOTIFY reportIdChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorChanged)

    // Friend access is the smallest possible change that lets the
    // diagnostics test suite exercise the actual private production
    // compressor directly, instead of inferring its behavior from the
    // bytes that sendReport uploads. The public surface is unchanged.
    friend class DiagnosticReporterTest;

public:
    enum State {
        Idle,
        Sending,
        Sent,
        Failed
    };
    Q_ENUM(State)

    explicit DiagnosticReporter(QObject *parent = nullptr);
    DiagnosticReporter(QNetworkAccessManager *network,
                      const QString &logDir,
                      const QString &downloadsDir,
                      QObject *parent = nullptr);
    ~DiagnosticReporter() override;

    State state() const;
    QString reportId() const;
    QString errorMessage() const;

    Q_INVOKABLE QString buildPreview(const QString &note);
    Q_INVOKABLE QVariantMap buildPreviewParts(const QString &note);
    Q_INVOKABLE void sendReport(const QString &note);
    Q_INVOKABLE bool saveToDownloads(const QString &note);

signals:
    void stateChanged();
    void reportIdChanged();
    void errorChanged();

private slots:
    void handleReplyReadyRead();
    void handleReplyFinished();
    void handleConnectTimeout();
    void handleIdleTimeout();
    void handleOverallTimeout();

private:
    static qint64 logTailByteCap();
    static qint64 compressedBundleByteCap();
    static int maximumNoteCodeUnits();
    static QByteArray gzipCompress(const QByteArray &input);

    void setState(State state);
    void setReportId(const QString &id);
    void setError(const QString &message);
    void clearError();

    QByteArray buildBundleJson(const QString &note, qint64 logByteCap) const;

    QByteArray readLogTail(qint64 byteCap, bool *ok) const;
    QString readLatestCrashFile(bool *found) const;
    QString mostRecentLogSuffix() const;
    QString mostRecentCrashSuffix() const;
    QByteArray readTailBytes(const QString &fileName, qint64 byteCap) const;

    void startUpload();
    void failUpload(const QString &message);

    QNetworkAccessManager *m_Nam;
    QScopedPointer<QNetworkAccessManager> m_OwnedNam;
    QString m_LogDir;
    QString m_DownloadsDir;
    QNetworkReply *m_Reply;
    State m_State;
    QString m_ReportId;
    QString m_ErrorMessage;
    QByteArray m_Payload;
    QByteArray m_ReplyBody;
    QTimer m_ConnectTimer;
    QTimer m_IdleTimer;
    QTimer m_OverallTimer;
};