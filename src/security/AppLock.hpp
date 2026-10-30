#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace ifs {

/// App lock: one password for the screen's protected web apps (Discord by default; the screen picks the others).
///
/// The password is never stored: a PBKDF2-HMAC-SHA256 hash (600 000 rounds, 16 random bytes of salt) is, together
/// with the list of locked apps, sealed with Windows' DPAPI for the current user and kept in the user's app data
/// (applock.bin, core/UserData.hpp). Someone who copies the file gets a blob only this Windows user can open, and
/// even then every guess costs 600 000 hashing rounds. Wrong passwords here wait longer after every 5 (30 s,
/// doubling, at most 15 min).
///
/// A locked app starts only with an unlock token (WEBAPP_OPEN unlock): unlock() gives one for an app, valid for
/// TOKEN_LIFETIME and for that app only. The app is told apart by its address (appIdForUrl), never by what the
/// screen says it is.
///
/// Screen -> service: APPLOCK_STATUS {}, APPLOCK_SETUP {password, locked}, APPLOCK_UNLOCK {password, appId},
///                    APPLOCK_CHANGE {password, newPassword}, APPLOCK_SET_LOCKED {password, locked},
///                    APPLOCK_RESET {} (also signs the web apps out: their browser profile is deleted)
/// Service -> screen: APPLOCK_RESULT {requestId, ok, error, retryAfterMs, token, hasPassword, locked}
class AppLock {
public:
    AppLock();

    nlohmann::json status();
    nlohmann::json setup(const std::string& password, const std::vector<std::string>& locked);
    nlohmann::json unlock(const std::string& password, const std::string& appId);
    nlohmann::json change(const std::string& password, const std::string& newPassword);
    nlohmann::json setLocked(const std::string& password, const std::vector<std::string>& locked);
    nlohmann::json reset(); // the caller closes the web app and deletes its browser profile

    // A locked app's start: true when it is not locked, or the token unlocked this app and is still valid
    bool mayOpen(const std::string& url, const std::string& token);
    bool isLockedUrl(const std::string& url);

    // "discord" for discord.com / discord.gg, "spotify", "youtube"; "" for any other address
    static std::string appIdForUrl(const std::string& url);

private:
    struct Record {
        bool present = false;
        int rounds = 0;
        std::vector<unsigned char> salt, hash;
        std::vector<std::string> locked;
    };
    bool load();
    bool save();
    bool verify(const std::string& password);
    nlohmann::json throttled();
    void failed();
    static std::string checkStrength(const std::string& password);

    std::mutex mutex_;
    Record rec_;
    int failures_ = 0;
    std::chrono::steady_clock::time_point blockedUntil_{};
    struct Token { std::string appId; std::chrono::steady_clock::time_point expires; };
    std::map<std::string, Token> tokens_;
};

} // namespace ifs
