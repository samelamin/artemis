#include "steamdecksession.h"

#include <QFile>

namespace {

bool hasDesktopToken(QString desktop, const QString &expected)
{
    desktop.replace(QLatin1Char(';'), QLatin1Char(':'));
    const QStringList tokens = desktop.split(QLatin1Char(':'));
    for (const QString &token : tokens) {
        if (token.trimmed().compare(expected, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

QString readDmiValue(const QString &name)
{
    QFile file(QStringLiteral("/sys/devices/virtual/dmi/id/") + name);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QString();
    }
    return QString::fromLocal8Bit(file.readAll().trimmed());
}

} // namespace

bool SteamDeckSession::isSteamDeckIdentity(const QString &productName,
                                            const QString &boardVendor,
                                            const QString &boardName)
{
    const bool recognizedProduct =
        productName.compare(QStringLiteral("Jupiter"), Qt::CaseInsensitive) == 0 ||
        productName.compare(QStringLiteral("Aerith"), Qt::CaseInsensitive) == 0 ||
        productName.compare(QStringLiteral("Galileo"), Qt::CaseInsensitive) == 0 ||
        productName.contains(QStringLiteral("Steam Deck"), Qt::CaseInsensitive);
    const bool recognizedBoard =
        boardName.compare(QStringLiteral("Jupiter"), Qt::CaseInsensitive) == 0 ||
        boardName.compare(QStringLiteral("Aerith"), Qt::CaseInsensitive) == 0 ||
        boardName.compare(QStringLiteral("Galileo"), Qt::CaseInsensitive) == 0;
    return boardVendor.contains(QStringLiteral("Valve"), Qt::CaseInsensitive) &&
           (recognizedProduct || recognizedBoard);
}

SteamDeckSession::Model SteamDeckSession::modelFromIdentity(const QString &productName,
                                                            const QString &boardVendor,
                                                            const QString &boardName)
{
    if (!boardVendor.contains(QStringLiteral("Valve"), Qt::CaseInsensitive)) {
        return NotSteamDeck;
    }

    const QString knownOledModels[] = {
        QStringLiteral("Galileo"),
    };
    const QString knownLcdModels[] = {
        QStringLiteral("Jupiter"),
        QStringLiteral("Aerith"),
    };

    for (const QString &name : knownOledModels) {
        if (productName.compare(name, Qt::CaseInsensitive) == 0) {
            return OLED;
        }
    }
    for (const QString &name : knownLcdModels) {
        if (productName.compare(name, Qt::CaseInsensitive) == 0) {
            return LCD;
        }
    }

    for (const QString &name : knownOledModels) {
        if (boardName.compare(name, Qt::CaseInsensitive) == 0) {
            return OLED;
        }
    }
    for (const QString &name : knownLcdModels) {
        if (boardName.compare(name, Qt::CaseInsensitive) == 0) {
            return LCD;
        }
    }

    // Fallback: isSteamDeckIdentity() accepts any Valve unit whose product name
    // contains "Steam Deck" even with an unknown codename. Classify those as
    // LCD so isSteamDeck() stays true (matches pre-branch behaviour) while the
    // new OLED-only HDR default stays off for unrecognised hardware - we
    // must never enable HDR on a Deck variant we cannot positively identify.
    if (isSteamDeckIdentity(productName, boardVendor, boardName)) {
        return LCD;
    }

    return NotSteamDeck;
}

bool SteamDeckSession::isSteamDeck()
{
    return model() != NotSteamDeck;
}

SteamDeckSession::Model SteamDeckSession::model()
{
    const QString productName = readDmiValue(QStringLiteral("product_name"));
    const QString boardVendor = readDmiValue(QStringLiteral("board_vendor"));
    const QString boardName = readDmiValue(QStringLiteral("board_name"));
    if (productName.isEmpty() && boardVendor.isEmpty() && boardName.isEmpty()) {
        qWarning("SteamDeckSession: could not read DMI product/board fields; treating as NotSteamDeck");
    }
    return modelFromIdentity(productName, boardVendor, boardName);
}

SteamDeckSession::Mode SteamDeckSession::classify(const QProcessEnvironment &environment)
{
    const QString desktop = environment.value(QStringLiteral("XDG_CURRENT_DESKTOP"));
    if (!environment.value(QStringLiteral("GAMESCOPE_WAYLAND_DISPLAY")).trimmed().isEmpty()
        || hasDesktopToken(desktop, QStringLiteral("gamescope"))) {
        return Gaming;
    }
    const QString fullSession = environment.value(QStringLiteral("KDE_FULL_SESSION")).trimmed();
    const QString sessionType = environment.value(QStringLiteral("XDG_SESSION_TYPE")).trimmed();
    if (hasDesktopToken(desktop, QStringLiteral("kde"))
        && fullSession.compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0
        && (sessionType.compare(QStringLiteral("wayland"), Qt::CaseInsensitive) == 0
            || sessionType.compare(QStringLiteral("x11"), Qt::CaseInsensitive) == 0)) {
        return Desktop;
    }
    return Unknown;
}

SteamDeckSession::Mode SteamDeckSession::current()
{
    return classify(QProcessEnvironment::systemEnvironment());
}

bool SteamDeckSession::shouldEnableGamescopeWsi(const QProcessEnvironment& environment,
                                                const QString& layerLibraryPath)
{
    if (classify(environment) != Gaming) {
        return false;
    }
    if (!environment.value(QStringLiteral("ENABLE_GAMESCOPE_WSI")).trimmed().isEmpty()) {
        return false;
    }
    if (environment.value(QStringLiteral("DISABLE_GAMESCOPE_WSI")).trimmed() == QLatin1String("1")) {
        return false;
    }
    if (layerLibraryPath.isEmpty() || !QFile::exists(layerLibraryPath)) {
        return false;
    }
    return true;
}

void SteamDeckSession::enableGamescopeWsiIfAvailable()
{
    const QString candidatePaths[] = {
        QStringLiteral("/var/run/host/usr/lib/libVkLayer_FROG_gamescope_wsi_x86_64.so"),
        QStringLiteral("/run/host/usr/lib/libVkLayer_FROG_gamescope_wsi_x86_64.so"),
    };
    QString layerPath;
    for (const QString& candidate : candidatePaths) {
        if (QFile::exists(candidate)) {
            layerPath = candidate;
            break;
        }
    }
    if (!shouldEnableGamescopeWsi(QProcessEnvironment::systemEnvironment(), layerPath)) {
        return;
    }
    qputenv("ENABLE_GAMESCOPE_WSI", "1");
    qWarning("SteamDeckSession: enabled gamescope Vulkan WSI layer at %s", qUtf8Printable(layerPath));
}

SteamDeckSession::Mode EnvironmentSessionModeProvider::mode() const
{
    return SteamDeckSession::current();
}
