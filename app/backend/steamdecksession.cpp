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
        productName.contains(QStringLiteral("Steam Deck"), Qt::CaseInsensitive);
    const bool recognizedBoard =
        boardName.compare(QStringLiteral("Jupiter"), Qt::CaseInsensitive) == 0 ||
        boardName.compare(QStringLiteral("Aerith"), Qt::CaseInsensitive) == 0;
    return boardVendor.contains(QStringLiteral("Valve"), Qt::CaseInsensitive) &&
           (recognizedProduct || recognizedBoard);
}

bool SteamDeckSession::isSteamDeck()
{
    return isSteamDeckIdentity(
        readDmiValue(QStringLiteral("product_name")),
        readDmiValue(QStringLiteral("board_vendor")),
        readDmiValue(QStringLiteral("board_name")));
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

SteamDeckSession::Mode EnvironmentSessionModeProvider::mode() const
{
    return SteamDeckSession::current();
}
