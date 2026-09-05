#pragma once

#include <QByteArray>
#include <QLatin1String>
#include <QString>
#include <QtGlobal>

namespace RfiPolicy {

inline bool isGalliumDriver(const QString& vendorString)
{
    return vendorString.contains(QLatin1String("Gallium"), Qt::CaseInsensitive);
}

inline bool workaroundOptedIn()
{
    return qgetenv("HAS_RFI_LATENCY_BUG") == QByteArrayLiteral("1");
}

inline bool workaroundEnabled(const QString& vendorString)
{
    return isGalliumDriver(vendorString) && workaroundOptedIn();
}

} // namespace RfiPolicy
