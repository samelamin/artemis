#include <QtTest>

#include <QByteArray>
#include <QString>

#include "streaming/video/ffmpeg-renderers/rfipolicy.h"

class RfiPolicyTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void galliumDetection_data();
    void galliumDetection();

    void optInRequiresExactOne_data();
    void optInRequiresExactOne();

    void workaroundEnabled_data();
    void workaroundEnabled();

    void legacyIgnoreEnvNeverEnablesWorkaround_data();
    void legacyIgnoreEnvNeverEnablesWorkaround();

private:
    QByteArray m_OriginalHasRfi;
    QByteArray m_OriginalLegacyIgnore;
    bool m_HadHasRfi;
    bool m_HadLegacyIgnore;

    void applyEnv(const QByteArray& hasValue, bool hasHas,
                  const QByteArray& legacyValue, bool hasLegacy);
};

void RfiPolicyTest::initTestCase()
{
    m_HadHasRfi = qEnvironmentVariableIsSet("HAS_RFI_LATENCY_BUG");
    m_HadLegacyIgnore = qEnvironmentVariableIsSet("IGNORE_RFI_LATENCY_BUG");

    if (m_HadHasRfi) {
        m_OriginalHasRfi = qgetenv("HAS_RFI_LATENCY_BUG");
    }
    if (m_HadLegacyIgnore) {
        m_OriginalLegacyIgnore = qgetenv("IGNORE_RFI_LATENCY_BUG");
    }

    qunsetenv("HAS_RFI_LATENCY_BUG");
    qunsetenv("IGNORE_RFI_LATENCY_BUG");
}

void RfiPolicyTest::cleanupTestCase()
{
    if (m_HadHasRfi) {
        qputenv("HAS_RFI_LATENCY_BUG", m_OriginalHasRfi);
    }
    else {
        qunsetenv("HAS_RFI_LATENCY_BUG");
    }

    if (m_HadLegacyIgnore) {
        qputenv("IGNORE_RFI_LATENCY_BUG", m_OriginalLegacyIgnore);
    }
    else {
        qunsetenv("IGNORE_RFI_LATENCY_BUG");
    }
}

void RfiPolicyTest::applyEnv(const QByteArray& hasValue, bool hasHas,
                             const QByteArray& legacyValue, bool hasLegacy)
{
    if (hasHas) {
        qputenv("HAS_RFI_LATENCY_BUG", hasValue);
    }
    else {
        qunsetenv("HAS_RFI_LATENCY_BUG");
    }

    if (hasLegacy) {
        qputenv("IGNORE_RFI_LATENCY_BUG", legacyValue);
    }
    else {
        qunsetenv("IGNORE_RFI_LATENCY_BUG");
    }
}

void RfiPolicyTest::galliumDetection_data()
{
    QTest::addColumn<QString>("vendor");
    QTest::addColumn<bool>("expected");

    QTest::newRow("empty") << QString() << false;
    QTest::newRow("non-gallium") << QStringLiteral("Intel iHD") << false;
    QTest::newRow("lowercase gallium") << QStringLiteral("mesa gallium 0.0") << true;
    QTest::newRow("uppercase GALLIUM") << QStringLiteral("GALLIUM") << true;
    QTest::newRow("mixed case Gallium") << QStringLiteral("Mesa Gallium VAAPI") << true;
    QTest::newRow("embedded gALLiuM") << QStringLiteral("mesa gALLiuM") << true;
}

void RfiPolicyTest::galliumDetection()
{
    QFETCH(QString, vendor);
    QFETCH(bool, expected);

    QCOMPARE(RfiPolicy::isGalliumDriver(vendor), expected);
}

void RfiPolicyTest::optInRequiresExactOne_data()
{
    QTest::addColumn<QByteArray>("envValue");
    QTest::addColumn<bool>("hasEnv");
    QTest::addColumn<bool>("expected");

    QTest::newRow("unset") << QByteArray() << false << false;
    QTest::newRow("empty") << QByteArray("") << true << false;
    QTest::newRow("zero") << QByteArray("0") << true << false;
    QTest::newRow("one") << QByteArray("1") << true << true;
    QTest::newRow("true") << QByteArray("true") << true << false;
    QTest::newRow("zero-one") << QByteArray("01") << true << false;
    QTest::newRow("leading space") << QByteArray(" 1") << true << false;
    QTest::newRow("trailing newline") << QByteArray("1\n") << true << false;
    QTest::newRow("uppercase one") << QByteArray("ONE") << true << false;
}

void RfiPolicyTest::optInRequiresExactOne()
{
    QFETCH(QByteArray, envValue);
    QFETCH(bool, hasEnv);
    QFETCH(bool, expected);

    applyEnv(envValue, hasEnv, QByteArray(), false);

    QCOMPARE(RfiPolicy::workaroundOptedIn(), expected);
}

void RfiPolicyTest::workaroundEnabled_data()
{
    QTest::addColumn<QString>("vendor");
    QTest::addColumn<QByteArray>("envValue");
    QTest::addColumn<bool>("hasEnv");
    QTest::addColumn<bool>("expected");

    QTest::newRow("gallium + unset") << QStringLiteral("Mesa Gallium") << QByteArray() << false << false;
    QTest::newRow("gallium + empty") << QStringLiteral("Mesa Gallium") << QByteArray("") << true << false;
    QTest::newRow("gallium + zero") << QStringLiteral("Mesa Gallium") << QByteArray("0") << true << false;
    QTest::newRow("gallium + one") << QStringLiteral("Mesa Gallium") << QByteArray("1") << true << true;
    QTest::newRow("gallium + true") << QStringLiteral("Mesa Gallium") << QByteArray("true") << true << false;
    QTest::newRow("gallium + zero-one") << QStringLiteral("Mesa Gallium") << QByteArray("01") << true << false;
    QTest::newRow("non-gallium + one") << QStringLiteral("Intel iHD") << QByteArray("1") << true << false;
    QTest::newRow("empty vendor + one") << QString() << QByteArray("1") << true << false;
    QTest::newRow("uppercase GALLIUM + one") << QStringLiteral("GALLIUM") << QByteArray("1") << true << true;
    QTest::newRow("mixed-case GaLLiuM + one") << QStringLiteral("mesa GaLLiuM driver") << QByteArray("1") << true << true;
}

void RfiPolicyTest::workaroundEnabled()
{
    QFETCH(QString, vendor);
    QFETCH(QByteArray, envValue);
    QFETCH(bool, hasEnv);
    QFETCH(bool, expected);

    applyEnv(envValue, hasEnv, QByteArray(), false);

    QCOMPARE(RfiPolicy::workaroundEnabled(vendor), expected);
}

void RfiPolicyTest::legacyIgnoreEnvNeverEnablesWorkaround_data()
{
    QTest::addColumn<QByteArray>("legacyValue");
    QTest::addColumn<bool>("hasLegacy");

    QTest::newRow("unset legacy") << QByteArray() << false;
    QTest::newRow("empty legacy") << QByteArray("") << true;
    QTest::newRow("legacy=0") << QByteArray("0") << true;
    QTest::newRow("legacy=1") << QByteArray("1") << true;
    QTest::newRow("legacy=true") << QByteArray("true") << true;
}

void RfiPolicyTest::legacyIgnoreEnvNeverEnablesWorkaround()
{
    QFETCH(QByteArray, legacyValue);
    QFETCH(bool, hasLegacy);

    applyEnv(QByteArray(), false, legacyValue, hasLegacy);

    QCOMPARE(RfiPolicy::workaroundEnabled(QStringLiteral("Mesa Gallium")), false);
    QCOMPARE(RfiPolicy::workaroundOptedIn(), false);
}

QTEST_GUILESS_MAIN(RfiPolicyTest)

#include "tst_rfipolicy.moc"
