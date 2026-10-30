#include "security/AppLock.hpp"
#include "core/Logger.hpp"
#include "core/UserData.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#endif

namespace ifs {

namespace {

constexpr int kRounds = 600000;          // PBKDF2-HMAC-SHA256 (OWASP's figure for SHA-256)
constexpr size_t kSaltBytes = 16;
constexpr size_t kHashBytes = 32;
constexpr size_t kMaxPassword = 128;
constexpr auto kTokenLifetime = std::chrono::minutes(2); // long enough for Try again after a failed start
constexpr int kFreeTries = 5;
constexpr auto kFirstBlock = std::chrono::seconds(30);
constexpr auto kMaxBlock = std::chrono::minutes(15);
const wchar_t* kEntropy = L"c6g3-infotainment-applock-v1"; // DPAPI's second secret: only this service unseals it

std::filesystem::path recordPath() { return userDataDir() / "applock.bin"; }

std::string toHex(const std::vector<unsigned char>& b) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(b.size() * 2);
    for (unsigned char c : b) { s += d[c >> 4]; s += d[c & 15]; }
    return s;
}

std::vector<unsigned char> fromHex(const std::string& s) {
    std::vector<unsigned char> b;
    if (s.size() % 2) return b;
    auto v = [](char c) { return c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0'; };
    for (size_t i = 0; i + 1 < s.size(); i += 2) b.push_back(static_cast<unsigned char>(v(s[i]) * 16 + v(s[i + 1])));
    return b;
}

std::vector<unsigned char> randomBytes(size_t n) {
    std::vector<unsigned char> b(n);
#if defined(_WIN32)
    BCryptGenRandom(nullptr, b.data(), static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#endif
    return b;
}

std::vector<unsigned char> pbkdf2(const std::string& password, const std::vector<unsigned char>& salt, int rounds) {
    std::vector<unsigned char> out(kHashBytes);
#if defined(_WIN32)
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0) return {};
    const NTSTATUS st = BCryptDeriveKeyPBKDF2(alg, reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),
                                              static_cast<ULONG>(password.size()), const_cast<PUCHAR>(salt.data()),
                                              static_cast<ULONG>(salt.size()), static_cast<ULONGLONG>(rounds),
                                              out.data(), static_cast<ULONG>(out.size()), 0);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (st != 0) return {};
#endif
    return out;
}

bool sameBytes(const std::vector<unsigned char>& a, const std::vector<unsigned char>& b) {
    if (a.size() != b.size() || a.empty()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); i++) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

#if defined(_WIN32)
bool dpapi(bool seal, const std::string& in, std::string& out) {
    DATA_BLOB src{static_cast<DWORD>(in.size()), reinterpret_cast<BYTE*>(const_cast<char*>(in.data()))};
    DATA_BLOB ent{static_cast<DWORD>(wcslen(kEntropy) * sizeof(wchar_t)), reinterpret_cast<BYTE*>(const_cast<wchar_t*>(kEntropy))};
    DATA_BLOB dst{};
    const BOOL ok = seal ? CryptProtectData(&src, L"c6g3 app lock", &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &dst)
                         : CryptUnprotectData(&src, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &dst);
    if (!ok) return false;
    out.assign(reinterpret_cast<char*>(dst.pbData), dst.cbData);
    SecureZeroMemory(dst.pbData, dst.cbData);
    LocalFree(dst.pbData);
    return true;
}
#endif

// Only short plain ids from the screen ("discord"), at most 32 of them
std::vector<std::string> cleanIds(const std::vector<std::string>& ids) {
    std::vector<std::string> out;
    for (const auto& id : ids) {
        if (id.empty() || id.size() > 32 || out.size() >= 32) continue;
        if (!std::all_of(id.begin(), id.end(), [](char c) { return std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '_' || c == '-'; })) continue;
        if (std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
    }
    return out;
}

nlohmann::json fail(const std::string& error) { return {{"ok", false}, {"error", error}}; }

} // namespace

AppLock::AppLock() { load(); }

std::string AppLock::appIdForUrl(const std::string& url) {
    const size_t scheme = url.find("://");
    std::string host = url.substr(scheme == std::string::npos ? 0 : scheme + 3);
    host = host.substr(0, host.find_first_of("/?#:"));
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto is = [&](const std::string& domain) {
        return host == domain || (host.size() > domain.size() && host.compare(host.size() - domain.size() - 1, std::string::npos, "." + domain) == 0);
    };
    if (is("discord.com") || is("discord.gg") || is("discordapp.com")) return "discord";
    if (is("spotify.com")) return "spotify";
    if (is("youtube.com") || is("youtu.be")) return "youtube";
    return "";
}

std::string AppLock::checkStrength(const std::string& password) {
    if (password.size() < 8) return "Use at least 8 characters.";
    if (password.size() > kMaxPassword) return "Use at most 128 characters.";
    const bool letter = std::any_of(password.begin(), password.end(), [](unsigned char c) { return std::isalpha(c); });
    const bool digit = std::any_of(password.begin(), password.end(), [](unsigned char c) { return std::isdigit(c); });
    if (!letter || !digit) return "Use letters and numbers.";
    return "";
}

bool AppLock::load() {
    rec_ = Record{};
#if defined(_WIN32)
    std::ifstream f(recordPath(), std::ios::binary);
    if (!f) return false;
    const std::string sealed((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string plain;
    if (!dpapi(false, sealed, plain)) {
        Logger::instance().warn("AppLock", "applock.bin could not be opened (another Windows user or a damaged file)");
        return false;
    }
    try {
        const auto j = nlohmann::json::parse(plain);
        rec_.rounds = j.value("rounds", 0);
        rec_.salt = fromHex(j.value("salt", ""));
        rec_.hash = fromHex(j.value("hash", ""));
        rec_.locked = cleanIds(j.value("locked", std::vector<std::string>{}));
        rec_.present = rec_.rounds > 0 && rec_.salt.size() == kSaltBytes && rec_.hash.size() == kHashBytes;
    } catch (const nlohmann::json::exception&) {
        rec_ = Record{};
    }
    SecureZeroMemory(plain.data(), plain.size());
#endif
    return rec_.present;
}

bool AppLock::save() {
#if defined(_WIN32)
    nlohmann::json j = {{"v", 1}, {"kdf", "pbkdf2-sha256"}, {"rounds", rec_.rounds}, {"salt", toHex(rec_.salt)},
                        {"hash", toHex(rec_.hash)}, {"locked", rec_.locked}};
    std::string plain = j.dump(), sealed;
    const bool ok = dpapi(true, plain, sealed);
    SecureZeroMemory(plain.data(), plain.size());
    if (!ok) return false;
    std::error_code ec;
    std::filesystem::create_directories(recordPath().parent_path(), ec);
    const std::filesystem::path tmp = recordPath().wstring() + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(sealed.data(), static_cast<std::streamsize>(sealed.size()));
        if (!f) return false;
    }
    std::filesystem::rename(tmp, recordPath(), ec);
    return !ec;
#else
    return false;
#endif
}

bool AppLock::verify(const std::string& password) {
    if (!rec_.present || password.empty() || password.size() > kMaxPassword) return false;
    return sameBytes(pbkdf2(password, rec_.salt, rec_.rounds), rec_.hash);
}

nlohmann::json AppLock::throttled() {
    const auto now = std::chrono::steady_clock::now();
    if (now >= blockedUntil_) return nullptr;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(blockedUntil_ - now).count();
    return {{"ok", false}, {"error", "Too many wrong passwords. Wait a moment."}, {"retryAfterMs", ms}};
}

void AppLock::failed() {
    if (++failures_ < kFreeTries) return;
    auto block = kFirstBlock * (1 << std::min(failures_ - kFreeTries, 5));
    if (block > kMaxBlock) block = kMaxBlock;
    blockedUntil_ = std::chrono::steady_clock::now() + block;
    Logger::instance().warn("AppLock", "{} wrong app lock passwords: next try in {} s", failures_,
                            std::chrono::duration_cast<std::chrono::seconds>(block).count());
}

nlohmann::json AppLock::status() {
    std::lock_guard lock(mutex_);
    return {{"ok", true}, {"hasPassword", rec_.present}, {"locked", rec_.locked}};
}

nlohmann::json AppLock::setup(const std::string& password, const std::vector<std::string>& locked) {
    std::lock_guard lock(mutex_);
    if (rec_.present) return fail("A password is already set.");
    if (const auto weak = checkStrength(password); !weak.empty()) return fail(weak);
    Record r;
    r.rounds = kRounds;
    r.salt = randomBytes(kSaltBytes);
    r.hash = pbkdf2(password, r.salt, r.rounds);
    r.locked = cleanIds(locked);
    r.present = r.hash.size() == kHashBytes;
    if (!r.present) return fail("The password could not be stored.");
    rec_ = r;
    if (!save()) return fail("The password could not be saved.");
    Logger::instance().info("AppLock", "App lock password set ({} app(s) locked)", rec_.locked.size());
    return {{"ok", true}, {"hasPassword", true}, {"locked", rec_.locked}};
}

nlohmann::json AppLock::unlock(const std::string& password, const std::string& appId) {
    std::lock_guard lock(mutex_);
    if (auto t = throttled(); !t.is_null()) return t;
    if (!verify(password)) {
        failed();
        return fail("Wrong password.");
    }
    failures_ = 0;
    const auto now = std::chrono::steady_clock::now();
    for (auto it = tokens_.begin(); it != tokens_.end();) it = it->second.expires <= now ? tokens_.erase(it) : std::next(it);
    const std::string token = toHex(randomBytes(32));
    tokens_[token] = {appId, now + kTokenLifetime};
    return {{"ok", true}, {"token", token}};
}

nlohmann::json AppLock::change(const std::string& password, const std::string& newPassword) {
    std::lock_guard lock(mutex_);
    if (auto t = throttled(); !t.is_null()) return t;
    if (!verify(password)) {
        failed();
        return fail("Wrong password.");
    }
    failures_ = 0;
    if (const auto weak = checkStrength(newPassword); !weak.empty()) return fail(weak);
    Record r = rec_;
    r.salt = randomBytes(kSaltBytes);
    r.rounds = kRounds;
    r.hash = pbkdf2(newPassword, r.salt, r.rounds);
    if (r.hash.size() != kHashBytes) return fail("The password could not be stored.");
    rec_ = r;
    if (!save()) return fail("The password could not be saved.");
    tokens_.clear();
    return {{"ok", true}, {"hasPassword", true}, {"locked", rec_.locked}};
}

nlohmann::json AppLock::setLocked(const std::string& password, const std::vector<std::string>& locked) {
    std::lock_guard lock(mutex_);
    if (auto t = throttled(); !t.is_null()) return t;
    if (!verify(password)) {
        failed();
        return fail("Wrong password.");
    }
    failures_ = 0;
    rec_.locked = cleanIds(locked);
    if (!save()) return fail("The change could not be saved.");
    return {{"ok", true}, {"hasPassword", true}, {"locked", rec_.locked}};
}

nlohmann::json AppLock::reset() {
    std::lock_guard lock(mutex_);
    std::error_code ec;
    std::filesystem::remove(recordPath(), ec);
    rec_ = Record{};
    tokens_.clear();
    failures_ = 0;
    blockedUntil_ = {};
    Logger::instance().info("AppLock", "App lock reset (the web apps are signed out)");
    return {{"ok", true}, {"hasPassword", false}, {"locked", nlohmann::json::array()}};
}

bool AppLock::isLockedUrl(const std::string& url) {
    std::lock_guard lock(mutex_);
    const std::string id = appIdForUrl(url);
    return rec_.present && !id.empty() && std::find(rec_.locked.begin(), rec_.locked.end(), id) != rec_.locked.end();
}

bool AppLock::mayOpen(const std::string& url, const std::string& token) {
    if (!isLockedUrl(url)) return true;
    std::lock_guard lock(mutex_);
    const auto it = tokens_.find(token);
    return it != tokens_.end() && it->second.appId == appIdForUrl(url) && it->second.expires > std::chrono::steady_clock::now();
}

} // namespace ifs
