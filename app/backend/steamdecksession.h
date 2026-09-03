#pragma once

#include <QProcessEnvironment>

class SteamDeckSession
{
public:
    enum Mode { Desktop, Gaming, Unknown };
    enum Model { NotSteamDeck, LCD, OLED };

    static Mode classify(const QProcessEnvironment &environment);
    static Mode current();
    static bool isSteamDeck();
    static bool isSteamDeckIdentity(const QString &productName,
                                    const QString &boardVendor,
                                    const QString &boardName);
    static Model modelFromIdentity(const QString &productName,
                                   const QString &boardVendor,
                                   const QString &boardName);
    static Model model();
    static bool shouldEnableGamescopeWsi(const QProcessEnvironment& environment,
                                        const QString& layerLibraryPath);
    static void enableGamescopeWsiIfAvailable();
};

class SessionModeProvider
{
public:
    virtual ~SessionModeProvider() = default;
    virtual SteamDeckSession::Mode mode() const = 0;
};

class EnvironmentSessionModeProvider final : public SessionModeProvider
{
public:
    SteamDeckSession::Mode mode() const override;
};
