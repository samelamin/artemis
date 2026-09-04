#include <QtTest>
#include <QRegularExpression>
#include <QString>

#include "backend/logscrubber.h"

class LogScrubberTest : public QObject
{
    Q_OBJECT

private slots:
    void redactsIpv4Address();
    void redactsIpv6Address();
    void redactsMacAddress();
    void dropsUrlQueryString();
    void redactsServerinfoXmlBody();
    void redactsRootXmlWrapper();
    void redactsApplistXmlBody();
    void redactsUuid();
    void redactsClientUniqueId();
    void replacesPcHostName();
    void redactsClipboardConnectedTo();
    void redactsHasNoMacAddress();
    void redactsIsAlreadyOnline();
    void redactsSentWolPacket();
    void redactsStartingOtpPairingFor();
    void redactsNetworkInterfaceName();
    void replacesDiscordUsername();
    void redactsOtpPin();
    void redactsOtpPassphrase();
    void redactsOtpGeneratedHash();
    void redactsOtpSalt();
    void redactsOtpHashForPinLine();
    void redactsHomePath();
    void redactsWindowsUserPath();
    void redactsMacUserPath();
    void redactsConfigPath();
    void redactsPemBlock();
    void redactsBearerToken();
    void redactsUrlHostDotLocal();
    void redactsUrlHostBareHostname();
    void redactsUrlHostIpLiteral();
    void replacesAppNameInKnownPhrasing();

    void preservesGpuString();
    void preservesDriverString();
    void preservesCodecHdrDecision();
    void preservesGeneralErrorText();
    void preservesStreamingResolutionWarning();
    void preservesQuittingAppFailureReason();

    void syntheticFixtureStripsAllPii();
};

namespace {

// A log line shape that the scrubber is allowed to add whitespace to.
// Tests assert on substrings, not byte equality, so trailing-newline
// normalization in the scrubber never breaks anything.
QString scrubOne(const QString& input)
{
    return LogScrubber::scrub(input);
}

} // namespace

void LogScrubberTest::redactsIpv4Address()
{
    const QString input = QStringLiteral(
        "Connecting to server at 192.168.1.42 over HTTPS");
    const QString out = scrubOne(input);
    QVERIFY2(!out.contains(QStringLiteral("192.168.1.42")),
             qPrintable(QStringLiteral("IPv4 address survived: ") + out));
    QVERIFY2(out.contains(QStringLiteral("[REDACTED]")),
             qPrintable(QStringLiteral("Expected [REDACTED] placeholder. Got: ") + out));
}

void LogScrubberTest::redactsIpv6Address()
{
    const QString input = QStringLiteral(
        "Resolved hostname to 2001:0db8:85a3:0000:0000:8a2e:0370:7334");
    const QString out = scrubOne(input);
    QVERIFY2(!out.contains(QStringLiteral("2001:0db8")),
             qPrintable(QStringLiteral("IPv6 address survived: ") + out));
    QVERIFY2(out.contains(QStringLiteral("[REDACTED]")),
             qPrintable(QStringLiteral("Expected [REDACTED] placeholder. Got: ") + out));

    // Compressed form too.
    const QString input2 = QStringLiteral("link-local fe80::1 responded");
    const QString out2 = scrubOne(input2);
    QVERIFY2(!out2.contains(QStringLiteral("fe80::1")),
             qPrintable(QStringLiteral("Compressed IPv6 survived: ") + out2));
}

void LogScrubberTest::redactsMacAddress()
{
    const QString input = QStringLiteral(
        "Found matching interface: wlan0 74:da:38:a1:b2:c3 up");
    const QString out = scrubOne(input);
    QVERIFY2(!out.contains(QStringLiteral("74:da:38")),
             qPrintable(QStringLiteral("MAC address survived: ") + out));
    QVERIFY2(out.contains(QStringLiteral("[REDACTED]")),
             qPrintable(QStringLiteral("Expected [REDACTED] placeholder. Got: ") + out));
}

void LogScrubberTest::dropsUrlQueryString()
{
    const QString input = QStringLiteral(
        "Executing request: https://192.168.1.50:47984/serverinfo?uniqueid=ABCDEF0123456789&uuid=01234567-89ab-cdef-0123-456789abcdef&rikey=deadbeef&rikeyid=12-34");
    const QString out = scrubOne(input);
    // Query string (and everything that lives in it) must be gone entirely.
    QVERIFY2(!out.contains(QStringLiteral("?")),
             qPrintable(QStringLiteral("'?' survived URL query strip: ") + out));
    QVERIFY2(!out.contains(QStringLiteral("uniqueid=")),
             qPrintable(QStringLiteral("uniqueid query param survived: ") + out));
    QVERIFY2(!out.contains(QStringLiteral("&rikey=")),
             qPrintable(QStringLiteral("rikey query param survived: ") + out));
    QVERIFY2(!out.contains(QStringLiteral("192.168.1.50")),
             qPrintable(QStringLiteral("Host IP inside URL survived: ") + out));
    // Scheme + host-shape + path remain.
    QVERIFY2(out.contains(QStringLiteral("https://")),
             qPrintable(QStringLiteral("URL scheme was dropped: ") + out));
    QVERIFY2(out.contains(QStringLiteral("/serverinfo")),
             qPrintable(QStringLiteral("URL path was dropped: ") + out));
}

void LogScrubberTest::redactsServerinfoXmlBody()
{
    const QString input = QStringLiteral(
        "getServerInfo HTTPS response: <serverinfo>"
        "<hostname>SALVATION-19</hostname>"
        "<uniqueid>deadbeefcafebabe</uniqueid>"
        "<mac>74:da:38:12:34:56</mac>"
        "<gfeVersion>3.20.3.20</gfeVersion>"
        "</serverinfo>");
    const QString out = scrubOne(input);
    QVERIFY2(out.contains(QStringLiteral("<serverinfo>[REDACTED]</serverinfo>")),
             qPrintable(QStringLiteral("serverinfo body collapse missing. Got: ") + out));
    QVERIFY2(!out.contains(QStringLiteral("SALVATION-19")),
             qPrintable(QStringLiteral("hostname inside serverinfo survived: ") + out));
    QVERIFY2(!out.contains(QStringLiteral("deadbeefcafebabe")),
             qPrintable(QStringLiteral("uniqueid inside serverinfo survived: ") + out));
    QVERIFY2(!out.contains(QStringLiteral("74:da:38")),
             qPrintable(QStringLiteral("MAC inside serverinfo survived: ") + out));
}

void LogScrubberTest::redactsRootXmlWrapper()
{
    const QString input = QStringLiteral(
        "getServerInfo response: <root status_code=\"200\"><hostname>BedroomPC</hostname></root>");
    const QString out = scrubOne(input);
    QVERIFY2(!out.contains(QStringLiteral("BedroomPC")),
             qPrintable(QStringLiteral("root XML body survived: ") + out));
    QVERIFY(out.contains(QStringLiteral("<root>[REDACTED]</root>")));

    const QString inputUpper = QStringLiteral(
        "getServerInfo response: <ROOT><hostname>BedroomPC</hostname></ROOT>");
    const QString outUpper = scrubOne(inputUpper);
    QVERIFY2(!outUpper.contains(QStringLiteral("BedroomPC")),
             qPrintable(QStringLiteral("uppercase ROOT XML body survived: ") + outUpper));
}

void LogScrubberTest::redactsApplistXmlBody()
{
    const QString input = QStringLiteral(
        "App list payload: <applist>"
        "<App><AppTitle>DOOM Eternal</AppTitle><ID>12345</ID></App>"
        "<App><AppTitle>Hades II</AppTitle><ID>67890</ID></App>"
        "</applist>");
    const QString out = scrubOne(input);
    QVERIFY2(out.contains(QStringLiteral("<applist>[REDACTED]</applist>")),
             qPrintable(QStringLiteral("applist body collapse missing. Got: ") + out));
    QVERIFY2(!out.contains(QStringLiteral("DOOM Eternal")),
             qPrintable(QStringLiteral("Game title inside applist survived: ") + out));
    QVERIFY2(!out.contains(QStringLiteral("Hades II")),
             qPrintable(QStringLiteral("Game title inside applist survived: ") + out));
}

void LogScrubberTest::redactsUuid()
{
    const QString input = QStringLiteral(
        "Launching app with ID: 12345 and UUID: 01234567-89ab-cdef-0123-456789abcdef");
    const QString out = scrubOne(input);
    QVERIFY2(!out.contains(QStringLiteral("01234567-89ab-cdef-0123-456789abcdef")),
             qPrintable(QStringLiteral("UUID survived: ") + out));
    QVERIFY2(out.contains(QStringLiteral("[REDACTED]")),
             qPrintable(QStringLiteral("Expected [REDACTED] placeholder. Got: ") + out));
    // Also brace-wrapped form.
    const QString input2 = QStringLiteral(
        "Stored id {aabbccdd-eeff-0011-2233-445566778899} for later");
    const QString out2 = scrubOne(input2);
    QVERIFY2(!out2.contains(QStringLiteral("aabbccdd-eeff-0011-2233-445566778899")),
             qPrintable(QStringLiteral("Braced UUID survived: ") + out2));
}

void LogScrubberTest::redactsClientUniqueId()
{
    // As logged by IdentityManager::getUniqueId via QString::number(uid, 16).
    const QString input = QStringLiteral(
        "Loaded unique ID from settings: deadbeefcafebabe");
    const QString out = scrubOne(input);
    QVERIFY2(!out.contains(QStringLiteral("deadbeefcafebabe")),
             qPrintable(QStringLiteral("uniqueid survived: ") + out));
    QVERIFY2(out.contains(QStringLiteral("[REDACTED]")),
             qPrintable(QStringLiteral("Expected [REDACTED] placeholder. Got: ") + out));

    const QString input2 = QStringLiteral(
        "Generated new unique ID: 0123456789abcdef");
    const QString out2 = scrubOne(input2);
    QVERIFY2(!out2.contains(QStringLiteral("0123456789abcdef")),
             qPrintable(QStringLiteral("uniqueid survived: ") + out2));
}

void LogScrubberTest::replacesPcHostName()
{
    const QString input = QStringLiteral(
        "Found unexpected PC SALVATION-19 looking for DESKTOP-PC-42");
    const QString out = scrubOne(input);
    QVERIFY2(!out.contains(QStringLiteral("SALVATION-19")),
             qPrintable(QStringLiteral("PC hostname survived: ") + out));
    QVERIFY2(out.contains(QStringLiteral("<HOST>")),
             qPrintable(QStringLiteral("Expected <HOST> placeholder. Got: ") + out));

    // "is now online at" shape from computermanager.cpp:101.
    const QString input2 = QStringLiteral(
        "DESKTOP-PC-42 is now online at 192.168.1.50:47984");
    const QString out2 = scrubOne(input2);
    QVERIFY2(!out2.contains(QStringLiteral("DESKTOP-PC-42")),
             qPrintable(QStringLiteral("PC name in online log survived: ") + out2));
    QVERIFY2(out2.contains(QStringLiteral("<HOST> is now online at")),
             qPrintable(QStringLiteral("Expected '<HOST> is now online at'. Got: ") + out2));
}

void LogScrubberTest::redactsClipboardConnectedTo()
{
    const QString input = QStringLiteral(
        "ClipboardManager: Connected to \"Living-Room-PC\"");
    const QString out = scrubOne(input);
    QVERIFY2(!out.contains(QStringLiteral("Living-Room-PC")),
             qPrintable(QStringLiteral("Host name survived: ") + out));
    QVERIFY(out.contains(QStringLiteral("<HOST>")));
}

void LogScrubberTest::redactsHasNoMacAddress()
{
    const QString input = QStringLiteral("BedroomPC has no MAC address stored");
    const QString out = scrubOne(input);
    QVERIFY2(!out.contains(QStringLiteral("BedroomPC")),
             qPrintable(QStringLiteral("Host name survived: ") + out));
    QVERIFY(out.contains(QStringLiteral("<HOST>")));
}

void LogScrubberTest::redactsIsAlreadyOnline()
{
    const QString input = QStringLiteral("BedroomPC is already online");
    const QString out = scrubOne(input);
    QVERIFY2(!out.contains(QStringLiteral("BedroomPC")),
             qPrintable(QStringLiteral("Host name survived: ") + out));
    QVERIFY(out.contains(QStringLiteral("<HOST>")));
}

void LogScrubberTest::redactsSentWolPacket()
{
    const QString input = QStringLiteral(
        "Sent WoL packet to BedroomPC via 192.168.1.5:9");
    const QString out = scrubOne(input);
    QVERIFY2(!out.contains(QStringLiteral("BedroomPC")),
             qPrintable(QStringLiteral("Host name survived: ") + out));
    QVERIFY(out.contains(QStringLiteral("<HOST>")));
}

void LogScrubberTest::redactsStartingOtpPairingFor()
{
    const QString input = QStringLiteral(
        "PendingOTPPairingTask: Starting OTP pairing task for BedroomPC");
    const QString out = scrubOne(input);
    QVERIFY2(!out.contains(QStringLiteral("BedroomPC")),
             qPrintable(QStringLiteral("Host name survived: ") + out));
    QVERIFY(out.contains(QStringLiteral("<HOST>")));
}

void LogScrubberTest::redactsNetworkInterfaceName()
{
    const QString input = QStringLiteral(
        "Found matching interface: eth0 aa:bb:cc:dd:ee:ff QFlags(...)");
    const QString out = scrubOne(input);
    QVERIFY2(!out.contains(QStringLiteral("eth0")),
             qPrintable(QStringLiteral("Interface name survived: ") + out));
    QVERIFY(out.contains(QStringLiteral("<HOST>")));
}

void LogScrubberTest::replacesDiscordUsername()
{
    const QString input = QStringLiteral(
        "Discord integration ready for user: discorduser#1234");
    const QString out = scrubOne(input);
    QVERIFY2(!out.contains(QStringLiteral("discorduser#1234")),
             qPrintable(QStringLiteral("Discord username survived: ") + out));
    QVERIFY(out.contains(QStringLiteral("<USER>")));
}

void LogScrubberTest::redactsOtpPin()
{
    const QString input = QStringLiteral(
        "PendingOTPPairingTask: PIN from user: 1234");
    const QString out = scrubOne(input);
    QVERIFY2(!out.contains(QStringLiteral("1234")),
             qPrintable(QStringLiteral("PIN survived: ") + out));
    QVERIFY(out.contains(QStringLiteral("[REDACTED]")));
}

void LogScrubberTest::redactsOtpPassphrase()
{
    const QString input = QStringLiteral(
        "PendingOTPPairingTask: Passphrase from user: correcthorsebattery");
    const QString out = scrubOne(input);
    QVERIFY2(!out.contains(QStringLiteral("correcthorsebattery")),
             qPrintable(QStringLiteral("Passphrase survived: ") + out));
    QVERIFY(out.contains(QStringLiteral("[REDACTED]")));
}

void LogScrubberTest::redactsOtpGeneratedHash()
{
    const QString input = QStringLiteral(
        "PendingOTPPairingTask: Generated OTP hash: DEADBEEFCAFEF00D");
    const QString out = scrubOne(input);
    QVERIFY2(!out.contains(QStringLiteral("DEADBEEFCAFEF00D")),
             qPrintable(QStringLiteral("OTP hash survived: ") + out));
    QVERIFY(out.contains(QStringLiteral("[REDACTED]")));
}

void LogScrubberTest::redactsOtpSalt()
{
    const QString input = QStringLiteral(
        "PendingOTPPairingTask: Using salt: abc123def456");
    const QString out = scrubOne(input);
    QVERIFY2(!out.contains(QStringLiteral("abc123def456")),
             qPrintable(QStringLiteral("Salt survived: ") + out));
    QVERIFY(out.contains(QStringLiteral("[REDACTED]")));
}

void LogScrubberTest::redactsOtpHashForPinLine()
{
    const QString input = QStringLiteral(
        "OTPPairingManager: Generated OTP hash for PIN: 9876 Salt: fedcba");
    const QString out = scrubOne(input);
    QVERIFY2(!out.contains(QStringLiteral("9876")),
             qPrintable(QStringLiteral("PIN survived: ") + out));
    QVERIFY2(!out.contains(QStringLiteral("fedcba")),
             qPrintable(QStringLiteral("Salt survived: ") + out));
    QVERIFY(out.contains(QStringLiteral("[REDACTED]")));
}

void LogScrubberTest::redactsHomePath()
{
    const QString input = QStringLiteral(
        "Log file: /home/alice/.local/share/Artemis/log.txt "
        "Settings: /root/.config/artemis/settings.ini "
        "Backup: ~/Documents/notes.md");
    const QString out = scrubOne(input);
    QVERIFY2(!out.contains(QStringLiteral("/home/alice")),
             qPrintable(QStringLiteral("/home/alice survived: ") + out));
    QVERIFY2(!out.contains(QStringLiteral("/root/")),
             qPrintable(QStringLiteral("/root/ survived: ") + out));
    QVERIFY2(!out.contains(QStringLiteral("~/Documents")),
             qPrintable(QStringLiteral("~/ survived: ") + out));
    QVERIFY2(out.contains(QStringLiteral("<HOME>")),
             qPrintable(QStringLiteral("Expected <HOME> placeholder. Got: ") + out));
}

void LogScrubberTest::redactsWindowsUserPath()
{
    const QString input1 = QStringLiteral(
        R"(Loading config from C:\Users\alice\AppData\Roaming\Artemis\config.ini)");
    const QString out1 = scrubOne(input1);
    QVERIFY2(!out1.contains(QStringLiteral("alice")),
             qPrintable(QStringLiteral("Windows username survived: ") + out1));
    QVERIFY(out1.contains(QStringLiteral("<HOME>")));

    const QString input2 = QStringLiteral(
        "Loading config from D:/Users/bob/AppData/Roaming/Artemis/config.ini");
    const QString out2 = scrubOne(input2);
    QVERIFY2(!out2.contains(QStringLiteral("bob")),
             qPrintable(QStringLiteral("Windows username survived: ") + out2));
    QVERIFY(out2.contains(QStringLiteral("<HOME>")));
}

void LogScrubberTest::redactsMacUserPath()
{
    const QString input = QStringLiteral(
        "Loading config from /Users/carol/Library/Application Support/Artemis/config.ini");
    const QString out = scrubOne(input);
    QVERIFY2(!out.contains(QStringLiteral("carol")),
             qPrintable(QStringLiteral("macOS username survived: ") + out));
    QVERIFY(out.contains(QStringLiteral("<HOME>")));
}

void LogScrubberTest::redactsConfigPath()
{
    // Mix a config-style path (collapsed to <CONFIG>) with a home-only path
    // (collapsed to <HOME>) so the test asserts both placeholders appear.
    const QString input = QStringLiteral(
        "Reading /home/alice/.config/artemis/state.json "
        "and /home/alice/Documents/notes.md");
    const QString out = scrubOne(input);
    QVERIFY2(out.contains(QStringLiteral("<HOME>")),
             qPrintable(QStringLiteral("Expected <HOME> for the home-only path. Got: ") + out));
    QVERIFY2(out.contains(QStringLiteral("<CONFIG>")),
             qPrintable(QStringLiteral("Expected <CONFIG> for the .config path. Got: ") + out));
    QVERIFY2(!out.contains(QStringLiteral("artemis/state.json")),
             qPrintable(QStringLiteral(".config subpath survived: ") + out));
    QVERIFY2(!out.contains(QStringLiteral("/home/alice")),
             qPrintable(QStringLiteral("home prefix survived: ") + out));
}

void LogScrubberTest::redactsPemBlock()
{
    const QString input = QStringLiteral(
        "Server certificate:\n"
        "-----BEGIN CERTIFICATE-----\n"
        "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAdeadbeefcafef00dba11\n"
        "-----END CERTIFICATE-----\n"
        "Above is the pinned cert.");
    const QString out = scrubOne(input);
    QVERIFY2(!out.contains(QStringLiteral("BEGIN CERTIFICATE")),
             qPrintable(QStringLiteral("PEM header survived: ") + out));
    QVERIFY2(!out.contains(QStringLiteral("END CERTIFICATE")),
             qPrintable(QStringLiteral("PEM footer survived: ") + out));
    QVERIFY2(!out.contains(QStringLiteral("deadbeefcafef00dba11")),
             qPrintable(QStringLiteral("PEM body survived: ") + out));
    QVERIFY2(out.contains(QStringLiteral("[REDACTED]")),
             qPrintable(QStringLiteral("Expected [REDACTED] placeholder. Got: ") + out));
}

void LogScrubberTest::redactsBearerToken()
{
    const QString input = QStringLiteral(
        "Authorization: Bearer abc123XYZ_def-456==789 sent");
    const QString out = scrubOne(input);
    QVERIFY2(!out.contains(QStringLiteral("abc123XYZ_def-456==789")),
             qPrintable(QStringLiteral("Bearer token survived: ") + out));
    QVERIFY2(!out.contains(QStringLiteral("Bearer")),
             qPrintable(QStringLiteral("'Bearer' scheme label survived: ") + out));
    QVERIFY2(out.contains(QStringLiteral("[REDACTED]")),
             qPrintable(QStringLiteral("Expected [REDACTED] placeholder. Got: ") + out));
}

void LogScrubberTest::redactsUrlHostDotLocal()
{
    const QString input = QStringLiteral(
        "Connecting to http://desktop-my-pc.local:47989/serverinfo");
    const QString out = scrubOne(input);
    QVERIFY2(!out.contains(QStringLiteral("desktop-my-pc.local")),
             qPrintable(QStringLiteral(".local hostname survived: ") + out));
    QVERIFY(out.contains(QStringLiteral("http://<HOST>:47989/serverinfo")));
}

void LogScrubberTest::redactsUrlHostBareHostname()
{
    const QString input = QStringLiteral(
        "Connecting to https://bedroompc:47984/pair");
    const QString out = scrubOne(input);
    QVERIFY2(!out.contains(QStringLiteral("bedroompc")),
             qPrintable(QStringLiteral("Bare hostname survived: ") + out));
    QVERIFY(out.contains(QStringLiteral("https://<HOST>:47984/pair")));
}

void LogScrubberTest::redactsUrlHostIpLiteral()
{
    const QString input = QStringLiteral(
        "Connecting to http://192.168.1.42:47989/serverinfo");
    const QString out = scrubOne(input);
    QVERIFY2(!out.contains(QStringLiteral("192.168.1.42")),
             qPrintable(QStringLiteral("IP-literal host survived: ") + out));
    QVERIFY(out.contains(QStringLiteral("http://<HOST>:47989/serverinfo")));
}

void LogScrubberTest::replacesAppNameInKnownPhrasing()
{
    // Two narrowly-scoped real log-line shapes are the only app/title
    // redactors that survive (the broader "verb + freeform title" patterns
    // were removed because they over-match real diagnostic text — see the
    // drm.cpp / otppairingmanager.cpp / quitstream.cpp negative tests
    // below for the lines that prompted the removal).

    // "Launching app with ID: <id>" from NvHTTP::startApp (nvhttp.cpp).
    const QString input = QStringLiteral(
        "Launching app with ID: 12345 and UUID: 01234567-89ab-cdef-0123-456789abcdef");
    const QString out = scrubOne(input);
    QVERIFY2(!out.contains(QStringLiteral("12345")),
             qPrintable(QStringLiteral("App ID in Launching line survived: ") + out));
    QVERIFY2(out.contains(QStringLiteral("<APP>")),
             qPrintable(QStringLiteral("Expected <APP> placeholder. Got: ") + out));

    // "App not found for box art callback: <title>" from appmodel.cpp.
    const QString input2 = QStringLiteral(
        "App not found for box art callback: Cyberpunk 2077");
    const QString out2 = scrubOne(input2);
    QVERIFY2(!out2.contains(QStringLiteral("Cyberpunk 2077")),
             qPrintable(QStringLiteral("Game title in box-art warning survived: ") + out2));
    QVERIFY2(out2.contains(QStringLiteral("<APP>")),
             qPrintable(QStringLiteral("Expected <APP> placeholder. Got: ") + out2));
}

void LogScrubberTest::preservesGpuString()
{
    const QString input = QStringLiteral(
        "GPU: NVIDIA GeForce RTX 4090 (driver 555.42.06)");
    const QString out = scrubOne(input);
    QCOMPARE(out, input);
}

void LogScrubberTest::preservesDriverString()
{
    const QString input = QStringLiteral(
        "SDL Video driver: wayland (built against libwayland 1.21.0)");
    const QString out = scrubOne(input);
    QCOMPARE(out, input);
}

void LogScrubberTest::preservesCodecHdrDecision()
{
    const QString input = QStringLiteral(
        "Selected codec: HEVC Main10, HDR enabled, bitrate 50 Mbps");
    const QString out = scrubOne(input);
    QCOMPARE(out, input);
}

void LogScrubberTest::preservesGeneralErrorText()
{
    const QString input = QStringLiteral(
        "Qt Warning: QSocketNotifier: Failed to register socket notifier "
        "(fd=12, type=Read, enabled=true): Operation not permitted");
    const QString out = scrubOne(input);
    QCOMPARE(out, input);
}

// Locks in the bug fix that removed the over-matching "Streaming <title>"
// regex: this exact line lives in
// app/streaming/video/ffmpeg-renderers/drm.cpp:790 and must survive
// scrubbing because it's a real hardware-limitation diagnostic.
void LogScrubberTest::preservesStreamingResolutionWarning()
{
    const QString input = QStringLiteral(
        "Streaming resolution is limited to 1080p on the Pi 4 inside the desktop environment!");
    const QString out = scrubOne(input);
    QCOMPARE(out, input);
}

// Locks in the bug fix that removed the over-matching "Quitting <title>"
// regex: this exact phrasing lives in app/cli/quitstream.cpp:102 and the
// %1 placeholder carries the actual failure reason — exactly the kind of
// error text the spec says must be kept.
void LogScrubberTest::preservesQuittingAppFailureReason()
{
    const QString input = QStringLiteral(
        "Quitting app failed, reason: Connection timed out");
    const QString out = scrubOne(input);
    QCOMPARE(out, input);
}

void LogScrubberTest::syntheticFixtureStripsAllPii()
{
    const QString fixture = QStringLiteral(
        "00:00:01.000 - Info - Compiled with SDL 2.28.5\n"
        "00:00:01.020 - Info - GPU: NVIDIA GeForce RTX 4090 (driver 555.42.06)\n"
        "00:00:01.040 - Info - Selected codec: HEVC Main10, HDR enabled, bitrate 50 Mbps\n"
        "00:00:01.050 - Info - SDL Video driver: wayland\n"
        "00:00:02.000 - Info - Loaded unique ID from settings: deadbeefcafebabe\n"
        "00:00:02.100 - Info - Found unexpected PC SALVATION-19 looking for DESKTOP-PC-42\n"
        "00:00:02.500 - Info - DESKTOP-PC-42 is now online at 192.168.1.50:47984\n"
        "00:00:03.000 - Info - Discovered mDNS host: steamdeck.local\n"
        "00:00:03.200 - Info - getServerInfo HTTPS response: <serverinfo>"
        "<hostname>STEAMDECK</hostname>"
        "<uniqueid>aabbccddeeff0011</uniqueid>"
        "<mac>74:da:38:12:34:56</mac>"
        "<gfeVersion>3.20.3.20</gfeVersion>"
        "</serverinfo>\n"
        "00:00:03.300 - Info - Server reachable via 2001:db8::1 (link-local fe80::1)\n"
        "00:00:03.400 - Info - Executing request: https://192.168.1.50:47984/serverinfo?uniqueid=ABCDEF0123456789&rikey=deadbeef\n"
        "00:00:04.000 - Info - Discord integration ready for user: discorduser#1234\n"
        "00:00:04.500 - Info - Launching app with ID: 12345 and UUID: 01234567-89ab-cdef-0123-456789abcdef\n"
        "00:00:04.700 - Info - app list entry: <App><AppTitle>Hades II</AppTitle><ID>12345</ID></App>\n"
        "00:00:05.000 - Info - Server certificate:\n"
        "-----BEGIN CERTIFICATE-----\n"
        "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAdeadbeefcafef00dba11\n"
        "-----END CERTIFICATE-----\n"
        "00:00:05.500 - Info - Log file: /home/alice/.local/share/Artemis/log.txt\n"
        "00:00:06.000 - Info - Qt Warning: QSocketNotifier: Failed to register socket notifier\n"
    );

    const QString out = LogScrubber::scrub(fixture);

    // No IPv4-looking substring anywhere.
    const QRegularExpression ipv4(QStringLiteral(
        R"(\b(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)(?:\.(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)){3}\b)"));
    QVERIFY2(!ipv4.match(out).hasMatch(),
             qPrintable(QStringLiteral("IPv4-looking substring leaked: ") + out));

    // No IPv6-looking substring. We split this into the two shapes that
    // a real IPv6 address can take: the '::' shorthand (compressed form)
    // and the full 8-group form. A naive "2+ colon-separated hex groups"
    // regex would match timestamps like "00:00:01" and produce false
    // positives — the '::' and 8-group checks together cover real IPv6
    // without touching time stamps.
    QVERIFY2(!out.contains(QStringLiteral("::")),
             qPrintable(QStringLiteral("'::' IPv6 shorthand leaked: ") + out));
    const QRegularExpression ipv6Full(QStringLiteral(
        R"((?:[0-9a-fA-F]{1,4}:){7}[0-9a-fA-F]{1,4})"));
    QVERIFY2(!ipv6Full.match(out).hasMatch(),
             qPrintable(QStringLiteral("Full-form IPv6 leaked: ") + out));

    // No UUID-looking substring.
    const QRegularExpression uuid(QStringLiteral(
        R"([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})"));
    QVERIFY2(!uuid.match(out).hasMatch(),
             qPrintable(QStringLiteral("UUID-looking substring leaked: ") + out));

    // Specific PII values absent.
    QVERIFY2(!out.contains(QStringLiteral("SALVATION-19")),
             qPrintable(QStringLiteral("PC hostname leaked: ") + out));
    QVERIFY2(!out.contains(QStringLiteral("DESKTOP-PC-42")),
             qPrintable(QStringLiteral("PC hostname leaked: ") + out));
    QVERIFY2(!out.contains(QStringLiteral("steamdeck.local")),
             qPrintable(QStringLiteral("mDNS host leaked: ") + out));
    QVERIFY2(!out.contains(QStringLiteral("STEAMDECK")),
             qPrintable(QStringLiteral("server hostname leaked: ") + out));
    QVERIFY2(!out.contains(QStringLiteral("deadbeefcafebabe")),
             qPrintable(QStringLiteral("uniqueid leaked: ") + out));
    QVERIFY2(!out.contains(QStringLiteral("discorduser#1234")),
             qPrintable(QStringLiteral("Discord username leaked: ") + out));
    QVERIFY2(!out.contains(QStringLiteral("Hades II")),
             qPrintable(QStringLiteral("App title inside <AppTitle> leaked: ") + out));
    // App/title tag names must survive (structural shape of the message
    // is preserved per the spec — only the inner content is collapsed).
    QVERIFY2(out.contains(QStringLiteral("<App>")),
             qPrintable(QStringLiteral("'<App>' tag name was dropped: ") + out));
    QVERIFY2(out.contains(QStringLiteral("<AppTitle>")),
             qPrintable(QStringLiteral("'<AppTitle>' tag name was dropped: ") + out));
    QVERIFY2(!out.contains(QStringLiteral("/home/alice")),
             qPrintable(QStringLiteral("Home path leaked: ") + out));
    QVERIFY2(!out.contains(QStringLiteral("BEGIN CERTIFICATE")),
             qPrintable(QStringLiteral("PEM block leaked: ") + out));

    // GPU, codec/HDR, and error lines must survive verbatim.
    QVERIFY2(out.contains(QStringLiteral("GPU: NVIDIA GeForce RTX 4090")),
             qPrintable(QStringLiteral("GPU string was modified: ") + out));
    QVERIFY2(out.contains(QStringLiteral("Selected codec: HEVC Main10, HDR enabled")),
             qPrintable(QStringLiteral("Codec/HDR line was modified: ") + out));
    QVERIFY2(out.contains(QStringLiteral("SDL Video driver: wayland")),
             qPrintable(QStringLiteral("Driver string was modified: ") + out));
    QVERIFY2(out.contains(QStringLiteral("QSocketNotifier: Failed to register socket notifier")),
             qPrintable(QStringLiteral("Error text was modified: ") + out));
}

QTEST_GUILESS_MAIN(LogScrubberTest)

#include "tst_logscrubber.moc"
