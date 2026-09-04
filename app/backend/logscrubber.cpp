#include "logscrubber.h"

#include <QRegularExpression>

namespace LogScrubber {

namespace {

constexpr const char* kRedacted = "[REDACTED]";

// URL query strings: drop everything from '?' up to whitespace, closing
// quote, or '<' (so XML attribute values that happen to contain '?' keep
// their content). Run early so the redacted payload cannot reintroduce
// something that looks like another class' input pattern.
const QRegularExpression& urlQueryRegex()
{
    static const QRegularExpression re(QStringLiteral(R"(\?[^\s"'<>]*)"));
    return re;
}

// http/https URL host component (hostname, IPv4/IPv6-literal, or .local
// mDNS name). Runs immediately after the query-string strip and before
// every other pass (including the IP regexes) so an IP-literal host is
// caught here rather than left for ipv4Regex() to redact separately —
// either way the IP disappears, but redacting it here keeps the URL's
// scheme/port/path shape visible instead of losing it to a bare
// [REDACTED]. \1 = scheme, \3 = optional ":port".
const QRegularExpression& urlHostRegex()
{
    static const QRegularExpression re(QStringLiteral(
        R"((https?://)(\[[0-9a-fA-F:]+\]|[^/\s:]+)((?::\d+)?))"));
    return re;
}

// PEM blocks: from "-----BEGIN ..." through "-----END ...". Multiline, with
// DotMatchesEverythingOption so the body (which may contain dashes and new
// lines) is consumed.
const QRegularExpression& pemBlockRegex()
{
    static const QRegularExpression re(
        QStringLiteral(R"(-----BEGIN [^-]+-----[\s\S]*?-----END [^-]+-----)"));
    return re;
}

// Standard hyphenated UUID, with optional Microsoft-style braces.
const QRegularExpression& uuidRegex()
{
    static const QRegularExpression re(
        QStringLiteral(R"(\{?[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\}?)"));
    return re;
}

// Client uniqueid: exactly 16 lowercase hex characters (matches the output
// of QString::number(uid, 16) in IdentityManager::getUniqueId). \b is a
// word boundary; uniqueness relies on UUIDs being the only other 16-hex-digit
// substring in real logs, and UUIDs always contain dashes so they don't
// collide here.
const QRegularExpression& uniqueIdRegex()
{
    static const QRegularExpression re(QStringLiteral(R"(\b[0-9a-f]{16}\b)"));
    return re;
}

// IPv4: dotted-quad, each octet 0-255.
const QRegularExpression& ipv4Regex()
{
    static const QRegularExpression re(QStringLiteral(
        R"(\b(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)(?:\.(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)){3}\b)"));
    return re;
}

// IPv6: covers full 8-group form, leading/trailing/embedded "::" compression,
// and the bare "::1" / "::" forms. (?<![:.\w]) / (?![:.\w]) keep it from
// matching inside longer hex sequences, MAC octets, version numbers like
// 1.2.3.4:80, or timestamps like 00:00:01.000.
const QRegularExpression& ipv6Regex()
{
    static const QRegularExpression re(QStringLiteral(
        R"IPV6((?<![:.\w])(?:(?:[0-9a-fA-F]{1,4}:){7}[0-9a-fA-F]{1,4}|(?:[0-9a-fA-F]{1,4}:){1,7}:|(?:[0-9a-fA-F]{1,4}:){1,6}(?::[0-9a-fA-F]{1,4}){1}|(?:[0-9a-fA-F]{1,4}:){1,5}(?::[0-9a-fA-F]{1,4}){1,2}|(?:[0-9a-fA-F]{1,4}:){1,4}(?::[0-9a-fA-F]{1,4}){1,3}|(?:[0-9a-fA-F]{1,4}:){1,3}(?::[0-9a-fA-F]{1,4}){1,4}|(?:[0-9a-fA-F]{1,4}:){1,2}(?::[0-9a-fA-F]{1,4}){1,5}|[0-9a-fA-F]{1,4}:(?::[0-9a-fA-F]{1,4}){1,6}|:(?::[0-9a-fA-F]{1,4}){1,7}|:(?::[0-9a-fA-F]{0,4})?:|::[0-9a-fA-F]{1,4}|::)(?![:.\w]))IPV6"));
    return re;
}

// MAC: 6 hex octets separated by ':' or '-', case-insensitive. Standard
// unicast / multicast / EUI-64 (with 16/20-bit OUI) shapes are all covered
// by the same shape; the test does not depend on format strictness.
const QRegularExpression& macRegex()
{
    static const QRegularExpression re(
        QStringLiteral(R"(\b(?:[0-9a-fA-F]{2}[:\-]){5}[0-9a-fA-F]{2}\b)"));
    return re;
}

// Authorization: Bearer <token> (case-insensitive on the scheme).
const QRegularExpression& bearerRegex()
{
    static const QRegularExpression re(
        QStringLiteral(R"((?i:Authorization:\s*Bearer\s+)\S+)"));
    return re;
}

// <serverinfo>...</serverinfo> body — element tags preserved, content
// replaced with [REDACTED]. Non-greedy so multiple elements do not collapse
// into one.
const QRegularExpression& serverInfoRegex()
{
    static const QRegularExpression re(
        QStringLiteral(R"(<serverinfo>([\s\S]*?)</serverinfo>)"));
    return re;
}

// <root>...</root> — Sunshine/Apollo's alternate wrapper for the same
// server-info payload serverInfoRegex() covers for GFE. Case-insensitive
// because server implementations vary in casing here.
const QRegularExpression& rootXmlRegex()
{
    static const QRegularExpression re(
        QStringLiteral(R"(<root\b[^>]*>([\s\S]*?)</root>)"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

// <applist>...</applist> body — same treatment.
const QRegularExpression& applistRegex()
{
    static const QRegularExpression re(
        QStringLiteral(R"(<applist>([\s\S]*?)</applist>)"));
    return re;
}

// <App>...</App> inner content (used to redact applist entries even when the
// surrounding <applist> wrapper is not present, e.g. a single <App> element
// printed on its own). Inner content becomes [REDACTED]; outer tag stays.
//
// The negative lookahead "(?<!<)" inside the inner-content capture means
// the regex only matches a <App>...</App> whose body contains no nested
// tag — i.e. bare plain text like <App>SomeTitle</App>. A <App> that wraps
// child tags (<AppTitle>, <ID>, etc.) is left alone here; the child tags
// are handled by their own rules (appTitleElementRegex), and leaving the
// <App> wrapper untouched is the right thing per the spec's "drop body,
// keep element names" contract.
const QRegularExpression& appElementRegex()
{
    static const QRegularExpression re(
        QStringLiteral(R"(<App>((?:(?!<)[\s\S])*?)</App>)"));
    return re;
}

// <AppTitle>...</AppTitle> inner content (also commonly printed standalone).
const QRegularExpression& appTitleElementRegex()
{
    static const QRegularExpression re(
        QStringLiteral(R"(<AppTitle>([\s\S]*?)</AppTitle>)"));
    return re;
}

// PC / host names appearing in known log phrasings (see computermanager.cpp).
// Each substitution is keyed off the surrounding phrase so a generic word in
// a GPU driver string is never matched by accident. The leading \s+ (or ^)
// anchors the match at the actual boundary rather than letting .*? over-match
// into a log-line prefix.
//
// "Found unexpected PC <name1> looking for <name2>" — both names are PC names
// (one is the unexpected machine, the other is what we were looking for), so
// both are replaced.
const QRegularExpression& pcFoundUnexpectedRegex()
{
    static const QRegularExpression re(
        QStringLiteral(R"(Found unexpected PC\s+\S+\s+looking for\s+\S+)"));
    return re;
}

const QRegularExpression& pcIsNowOnlineRegex()
{
    static const QRegularExpression re(
        QStringLiteral(R"((?:^|\s)\S+\s+is now online at)"));
    return re;
}

const QRegularExpression& pcIsNowOfflineRegex()
{
    static const QRegularExpression re(
        QStringLiteral(R"((?:^|\s)\S+\s+is now offline(?:\s|$))"));
    return re;
}

const QRegularExpression& pcDiscoveredMdnsRegex()
{
    static const QRegularExpression re(
        QStringLiteral(R"(Discovered mDNS host:\s+\S+)"));
    return re;
}

const QRegularExpression& pcResolvingRegex()
{
    static const QRegularExpression re(
        QStringLiteral(R"(\bResolving\s+\S+\s+timed out)"));
    return re;
}

const QRegularExpression& pcResolvedRegex()
{
    static const QRegularExpression re(
        QStringLiteral(R"(\bResolved\s+\S+\s+to\b)"));
    return re;
}

const QRegularExpression& pcNowAtRegex()
{
    static const QRegularExpression re(
        QStringLiteral(R"((?:^|\s)\S+\s+is now at\s)"));
    return re;
}

// ClipboardManager: "Connected to <name>" (QDebug default quoting applies
// to the name argument).
const QRegularExpression& clipboardConnectedToRegex()
{
    static const QRegularExpression re(
        QStringLiteral(R"(Connected to\s+\S+)"));
    return re;
}

// NvComputer::wake(): "<name> has no MAC address stored" / "<name> is
// already online" — name comes before the phrase here, mirroring
// pcIsNowOnlineRegex's shape.
const QRegularExpression& pcHasNoMacAddressRegex()
{
    static const QRegularExpression re(
        QStringLiteral(R"((?:^|\s)\S+\s+has no MAC address stored)"));
    return re;
}

const QRegularExpression& pcIsAlreadyOnlineRegex()
{
    static const QRegularExpression re(
        QStringLiteral(R"((?:^|\s)\S+\s+is already online)"));
    return re;
}

// NvComputer::wake(): "Sent WoL packet to <name> via ..." (qInfo().nospace()
// .noquote(), so no surrounding quotes on the name).
const QRegularExpression& sentWolPacketRegex()
{
    static const QRegularExpression re(
        QStringLiteral(R"(Sent WoL packet to\s+\S+\s+via)"));
    return re;
}

// PendingOTPPairingTask::run(): "Starting OTP pairing task for <name>".
const QRegularExpression& startingOtpPairingForRegex()
{
    static const QRegularExpression re(
        QStringLiteral(R"(Starting OTP pairing task for\s+\S+)"));
    return re;
}

// NvComputer: "Found matching interface: <name> <mac> <flags>". The MAC on
// this line is already redacted by macRegex() earlier in the pipeline; this
// rule only needs to catch the interface name token right after the phrase.
const QRegularExpression& foundMatchingInterfaceRegex()
{
    static const QRegularExpression re(
        QStringLiteral(R"(Found matching interface:\s+\S+)"));
    return re;
}

// Discord username: only appears in "Discord integration ready for user: <u>".
const QRegularExpression& discordUsernameRegex()
{
    static const QRegularExpression re(
        QStringLiteral(R"(Discord integration ready for user:\s+\S+)"));
    return re;
}

// OTP pairing secrets (see computermanager.cpp / otppairingmanager.cpp).
// Belt-and-braces: the source-level qDebug calls that used to print these
// values were removed, but the scrubber also denylists the phrase shapes
// in case the same shape appears anywhere else in captured text.
const QRegularExpression& otpPinFromUserRegex()
{
    static const QRegularExpression re(
        QStringLiteral(R"(PIN from user:\s*[^\n]*)"));
    return re;
}

const QRegularExpression& otpPassphraseFromUserRegex()
{
    static const QRegularExpression re(
        QStringLiteral(R"(Passphrase from user:\s*[^\n]*)"));
    return re;
}

const QRegularExpression& otpGeneratedHashRegex()
{
    static const QRegularExpression re(
        QStringLiteral(R"(Generated OTP hash:\s*[^\n]*)"));
    return re;
}

const QRegularExpression& otpUsingSaltRegex()
{
    static const QRegularExpression re(
        QStringLiteral(R"(Using salt:\s*[^\n]*)"));
    return re;
}

// Covers the combined "...for PIN: <pin> Salt: <salt>" line from
// OTPPairingManager in one match — everything from "PIN:" to end of line,
// which naturally includes the trailing "Salt: <value>" too.
const QRegularExpression& otpHashForPinRegex()
{
    static const QRegularExpression re(
        QStringLiteral(R"(Generated OTP hash for PIN:\s*[^\n]*)"));
    return re;
}

// Home directory paths under /home/<user>/, /root/, or ~/. Used as the
// catch-all after the config-path pass has already consumed config-style
// subpaths. The match captures the whole path so siblings on the same line
// (e.g. "/home/alice/x and /home/bob/y") do not get folded together.
const QRegularExpression& homePathRegex()
{
    static const QRegularExpression re(
        QStringLiteral(R"((?:/home/[^/\s]+|/root/|~)(?:/[^/\s]+)*)"));
    return re;
}

// Windows user paths: C:\Users\<name>\... or C:/Users/<name>/..., any
// drive letter, either slash direction.
const QRegularExpression& windowsUserPathRegex()
{
    static const QRegularExpression re(QStringLiteral(
        R"([A-Za-z]:[\\/]Users[\\/][^\\/\s]+(?:[\\/][^\\/\s]+)*)"));
    return re;
}

// macOS user paths: /Users/<name>/...
const QRegularExpression& macUserPathRegex()
{
    static const QRegularExpression re(
        QStringLiteral(R"(/Users/[^/\s]+(?:/[^/\s]+)*)"));
    return re;
}

// XDG config / state / cache paths. Matches "/home/<user>/.../.<x>",
// "/root/.../.<x>", or "~/<x>" where <x> is one of config, local/share,
// local/state, or cache. The whole path (including the leading
// /home/<user>/ or ~/ prefix) is collapsed so a sibling home-only path on
// the same line can still be matched by homePathRegex() afterwards.
const QRegularExpression& configPathRegex()
{
    static const QRegularExpression re(
        QStringLiteral(R"((?:/home/[^/\s]+|/root/|~)/(?:[^/\s]+/)*[^/\s]*\.(?:config|local/share|local/state|cache)(?:/[^/\s]+)*)"));
    return re;
}

// App / game title phrasings. Only narrowly-scoped real log-line shapes
// are matched here; the broader "verb + freeform title" patterns
// ("Streaming X", "Starting X", "Quitting X", "Resuming X") were
// originally considered but turned out to be false-positive landmines
// against real diagnostic text:
//
//   - "Streaming resolution is limited to 1080p on the Pi 4..." (drm.cpp)
//   - "Starting OTP pairing..." / "Starting Apollo OTP pairing..."
//     (otppairingmanager.cpp)
//   - "Quitting app failed, reason: ..." (quitstream.cpp)
//
// all of which the spec explicitly lists as must-survive error/diagnostic
// text. Real game/app titles only appear inside <applist>/<App>/<AppTitle>
// XML bodies (covered by serverInfoRegex/applistRegex/appElementRegex/
// appTitleElementRegex above), so there is no bare-phrasing heuristic to
// add here.
const QRegularExpression& appNameLaunchingRegex()
{
    static const QRegularExpression re(
        QStringLiteral(R"((Launching app with (?:ID|Name):\s+)(?:"[^"]*"|\S+)(?=\s|$|\s*[,\[]))"));
    return re;
}

const QRegularExpression& appNameQuotedRegex()
{
    static const QRegularExpression re(
        QStringLiteral(R"(\bApp not found for box art callback:\s+(?:"[^"]*"|\S+))"));
    return re;
}

void applyGlobal(QString& text, const QRegularExpression& re, const QString& replacement)
{
    // Iterate replace so overlapping matches (which Qt's single-pass replace
    // would skip) are still scrubbed. We bound the loop by a generous limit
    // rather than "while hasMatch" because a regex that matches the empty
    // string would otherwise loop forever.
    constexpr int kMaxIterations = 64;
    for (int i = 0; i < kMaxIterations; ++i) {
        const QString before = text;
        text.replace(re, replacement);
        if (text == before) {
            return;
        }
    }
}

} // namespace

QString scrub(const QString& input)
{
    QString text = input;

    // 1. Drop URL query strings first — the redacted payload would otherwise
    //    introduce IP-shaped and UUID-shaped substrings that the later
    //    passes would re-scrub (a no-op but wasted work and noise).
    applyGlobal(text, urlQueryRegex(), QString());

    // 1b. http/https URL host component. Runs early, right after the
    //     query-string strip, so an IP-literal host is caught in its URL
    //     context before the generic IP passes below ever see it.
    applyGlobal(text, urlHostRegex(), QStringLiteral("\\1<HOST>\\3"));

    // 2. PEM blocks — multi-line redact before any single-line rule so the
    //    body text (which contains dashes and capitals that look like other
    //    tokens) is gone.
    applyGlobal(text, pemBlockRegex(), QString::fromLatin1(kRedacted));

    // 3. UUIDs (standard hyphenated format). Braces optional.
    applyGlobal(text, uuidRegex(), QString::fromLatin1(kRedacted));

    // 4. Client uniqueid (16 lowercase hex). Done before IP/MAC so a
    //    substring of a uniqueid inside something larger is gone first.
    applyGlobal(text, uniqueIdRegex(), QString::fromLatin1(kRedacted));

    // 5. IPv4.
    applyGlobal(text, ipv4Regex(), QString::fromLatin1(kRedacted));

    // 6. IPv6.
    applyGlobal(text, ipv6Regex(), QString::fromLatin1(kRedacted));

    // 7. MAC addresses.
    applyGlobal(text, macRegex(), QString::fromLatin1(kRedacted));

    // 8. Authorization: Bearer <token>.
    applyGlobal(text, bearerRegex(), QString::fromLatin1(kRedacted));

    // 9. <serverinfo>...</serverinfo> and <applist>...</applist> bodies.
    //    Tag names are preserved so the structural shape of the message
    //    remains readable; only the inner content (which can include the
    //    server's hostname, MAC, GPU model, and the entire <App> list with
    //    per-app titles) is collapsed.
    applyGlobal(text, serverInfoRegex(),
                QStringLiteral("<serverinfo>[REDACTED]</serverinfo>"));
    applyGlobal(text, rootXmlRegex(),
                QStringLiteral("<root>[REDACTED]</root>"));
    applyGlobal(text, applistRegex(),
                QStringLiteral("<applist>[REDACTED]</applist>"));

    // 10. Standalone <App>...</App> and <AppTitle>...</AppTitle> elements
    //     (e.g. a single applist entry logged on its own). <AppTitle> runs
    //     first so that an <App> wrapper containing nested tags (e.g.
    //     <App><AppTitle>Hades II</AppTitle><ID>12345</ID></App>) has the
    //     title text already redacted before appElementRegex's bare-text-only
    //     check sees it — that check correctly leaves the structured <App>
    //     alone instead of blanking the whole wrapper.
    applyGlobal(text, appTitleElementRegex(),
                QStringLiteral("<AppTitle>[REDACTED]</AppTitle>"));
    applyGlobal(text, appElementRegex(),
                QStringLiteral("<App>[REDACTED]</App>"));

    // 11. PC / host names in known phrasings. Each rule replaces the whole
    //     match (name + surrounding phrase) with the literal placeholder
    //     shape so the log line stays readable.
    applyGlobal(text, pcFoundUnexpectedRegex(),
                QStringLiteral("Found unexpected PC <HOST> looking for <HOST>"));
    applyGlobal(text, pcIsNowOnlineRegex(),
                QStringLiteral(" <HOST> is now online at"));
    applyGlobal(text, pcIsNowOfflineRegex(),
                QStringLiteral(" <HOST> is now offline "));
    applyGlobal(text, pcDiscoveredMdnsRegex(),
                QStringLiteral("Discovered mDNS host: <HOST>"));
    applyGlobal(text, pcResolvingRegex(),
                QStringLiteral("Resolving <HOST> timed out"));
    applyGlobal(text, pcResolvedRegex(),
                QStringLiteral("Resolved <HOST> to"));
    applyGlobal(text, pcNowAtRegex(),
                QStringLiteral(" <HOST> is now at "));
    applyGlobal(text, clipboardConnectedToRegex(),
                QStringLiteral("Connected to <HOST>"));
    applyGlobal(text, pcHasNoMacAddressRegex(),
                QStringLiteral(" <HOST> has no MAC address stored"));
    applyGlobal(text, pcIsAlreadyOnlineRegex(),
                QStringLiteral(" <HOST> is already online"));
    applyGlobal(text, sentWolPacketRegex(),
                QStringLiteral("Sent WoL packet to <HOST> via"));
    applyGlobal(text, startingOtpPairingForRegex(),
                QStringLiteral("Starting OTP pairing task for <HOST>"));
    applyGlobal(text, foundMatchingInterfaceRegex(),
                QStringLiteral("Found matching interface: <HOST>"));

    // 12. Discord username in the one log line that carries it.
    applyGlobal(text, discordUsernameRegex(),
                QStringLiteral("Discord integration ready for user: <USER>"));

    // 12a. OTP pairing secrets (PIN, passphrase, hash, salt). Belt-and-braces
    //      with the source-level fix in computermanager.cpp /
    //      otppairingmanager.cpp — see those files.
    applyGlobal(text, otpPinFromUserRegex(),
                QStringLiteral("PIN from user: [REDACTED]"));
    applyGlobal(text, otpPassphraseFromUserRegex(),
                QStringLiteral("Passphrase from user: [REDACTED]"));
    applyGlobal(text, otpGeneratedHashRegex(),
                QStringLiteral("Generated OTP hash: [REDACTED]"));
    applyGlobal(text, otpUsingSaltRegex(),
                QStringLiteral("Using salt: [REDACTED]"));
    applyGlobal(text, otpHashForPinRegex(),
                QStringLiteral("Generated OTP hash for PIN: [REDACTED]"));

    // 13. Home and config paths. Config first so a path like
    //     "/home/alice/.config/state.json" collapses to "<CONFIG>" instead
    //     of "<HOME>/<CONFIG>" — leaving "<HOME>" for genuinely home-only
    //     paths (e.g. "~/Documents/notes.md").
    applyGlobal(text, configPathRegex(), QStringLiteral("<CONFIG>"));
    applyGlobal(text, homePathRegex(), QStringLiteral("<HOME>"));
    applyGlobal(text, windowsUserPathRegex(), QStringLiteral("<HOME>"));
    applyGlobal(text, macUserPathRegex(), QStringLiteral("<HOME>"));

    // 14. App / game titles. Only the two narrowly-scoped real log-line
    //     shapes (the "Launching app with ID:" prefix and the box-art
    //     callback warning) are matched; bare "verb + title" phrasings
    //     would over-match real diagnostic/error text. Real titles are
    //     otherwise captured by the <applist>/<App>/<AppTitle> XML-body
    //     rules above.
    applyGlobal(text, appNameLaunchingRegex(), QStringLiteral("\\1<APP>"));
    applyGlobal(text, appNameQuotedRegex(),
                QStringLiteral("App not found for box art callback: <APP>"));

    return text;
}

} // namespace LogScrubber
