#include "resources/RadioDiscovery.hpp"
#include "core/Logger.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace ifs {

namespace {

constexpr size_t kMaxPageBytes     = 2 * 1024 * 1024;
constexpr size_t kMaxPlaylistBytes = 64 * 1024;
constexpr size_t kMaxLogoBytes     = 2 * 1024 * 1024;
constexpr size_t kMaxProbes        = 12;  // candidates opened as streams
constexpr size_t kProbesAtOnce     = 6;
constexpr size_t kMaxPlaylists     = 6;   // .pls / .m3u files found on a page that are opened
constexpr size_t kEntriesPerList   = 2;   // a playlist's mirrors play the same: the first ones are enough
constexpr int    kPageTimeoutMs    = 12000;
constexpr int    kLogoPageTimeoutMs = 6000; // a station's own website, read only for its logo
constexpr size_t kLogoPageBytes    = 256 * 1024; // the logo tags are in the page's head
constexpr int    kProbeTimeoutMs   = 9000;
constexpr const char* kUserAgent   = "Mozilla/5.0 (c6g3 infotainment radio)";

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string avError(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof(buf));
    return buf;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool startsWith(const std::string& s, const std::string& p) { return s.rfind(p, 0) == 0; }
bool endsWith(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0; }
bool contains(const std::string& s, const std::string& p) { return s.find(p) != std::string::npos; }

void replaceAll(std::string& s, const std::string& from, const std::string& to) {
    for (size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos; pos += to.size()) s.replace(pos, from.size(), to);
}

// Station names come from servers as UTF-8 or Latin-1: Latin-1 is converted (the JSON sent must be UTF-8)
std::string toValidUtf8(const std::string& s) {
    bool valid = true;
    for (size_t i = 0; i < s.size() && valid;) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (len == 0 || i + len > s.size()) valid = false;
        for (int k = 1; valid && k < len; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) >> 6) != 0x2) valid = false;
        }
        i += len ? len : 1;
    }
    if (valid) return s;
    std::string out;
    for (unsigned char c : s) {
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// &amp; &quot; &#39; &#x27; ... (names and addresses taken from HTML)
std::string decodeEntities(std::string s) {
    replaceAll(s, "&amp;", "&");
    replaceAll(s, "&quot;", "\"");
    replaceAll(s, "&apos;", "'");
    replaceAll(s, "&lt;", "<");
    replaceAll(s, "&gt;", ">");
    replaceAll(s, "&nbsp;", " ");
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&' && i + 2 < s.size() && s[i + 1] == '#') {
            const size_t semi = s.find(';', i);
            if (semi != std::string::npos && semi - i <= 8) {
                const bool hex = s[i + 2] == 'x' || s[i + 2] == 'X';
                const std::string digits = s.substr(i + (hex ? 3 : 2), semi - i - (hex ? 3 : 2));
                char* end = nullptr;
                const unsigned long cp = std::strtoul(digits.c_str(), &end, hex ? 16 : 10);
                if (!digits.empty() && end && *end == '\0' && cp > 0 && cp < 0x110000) {
                    if (cp < 0x80) out += static_cast<char>(cp);
                    else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
                    else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
                    else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
                    i = semi;
                    continue;
                }
            }
        }
        out += s[i];
    }
    return out;
}

// ── Addresses ──

struct UrlParts {
    std::string scheme, host, path; // path: without query / fragment, lower case
    int port = 0;                   // 0: the scheme's default
};

UrlParts parseUrl(const std::string& url) {
    UrlParts u;
    const size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return u;
    u.scheme = lower(url.substr(0, schemeEnd));
    const size_t hostStart = schemeEnd + 3;
    size_t pathStart = url.find_first_of("/?#", hostStart);
    std::string authority = url.substr(hostStart, pathStart == std::string::npos ? std::string::npos : pathStart - hostStart);
    const size_t at = authority.rfind('@');
    if (at != std::string::npos) authority = authority.substr(at + 1);
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos) {
        u.port = std::atoi(authority.substr(colon + 1).c_str());
        authority = authority.substr(0, colon);
    }
    u.host = lower(authority);
    if (pathStart != std::string::npos && url[pathStart] == '/') {
        const size_t pathEnd = url.find_first_of("?#", pathStart);
        u.path = lower(url.substr(pathStart, pathEnd == std::string::npos ? std::string::npos : pathEnd - pathStart));
    }
    return u;
}

std::string originOf(const std::string& url) {
    const size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return "";
    const size_t pathStart = url.find_first_of("/?#", schemeEnd + 3);
    return url.substr(0, pathStart);
}

// A link found on a page, made absolute ("" when it is no web address)
std::string resolveUrl(const std::string& base, std::string ref) {
    ref = trim(decodeEntities(ref));
    if (ref.empty() || ref.size() > 2048) return "";
    const std::string l = lower(ref);
    if (startsWith(l, "http://") || startsWith(l, "https://")) return ref;
    if (startsWith(l, "//")) {
        const size_t schemeEnd = base.find("://");
        return (schemeEnd == std::string::npos ? std::string("https") : base.substr(0, schemeEnd)) + ":" + ref;
    }
    if (contains(l.substr(0, std::min<size_t>(l.size(), 12)), ":") || startsWith(l, "#")) return ""; // mailto:, data:, javascript:, #...
    if (base.empty()) return "";
    if (ref[0] == '/') return originOf(base) + ref;
    std::string dir = base.substr(0, base.find_first_of("?#"));
    const size_t schemeEnd = dir.find("://");
    const size_t lastSlash = dir.rfind('/');
    if (lastSlash == std::string::npos || lastSlash < schemeEnd + 3) dir += '/';
    else dir = dir.substr(0, lastSlash + 1);
    return dir + ref;
}

std::string extensionOf(const std::string& path) {
    const size_t slash = path.rfind('/');
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
    return path.substr(dot);
}

const std::set<std::string> kAudioExt    = {".mp3", ".aac", ".aacp", ".ogg", ".oga", ".opus", ".flac", ".mpga", ".adts"};
const std::set<std::string> kPlaylistExt = {".pls", ".m3u"};
const std::set<std::string> kWebExt      = {".html", ".htm", ".php", ".asp", ".aspx", ".jsp", ".js", ".mjs", ".css", ".png",
                                            ".jpg", ".jpeg", ".gif", ".svg", ".webp", ".ico", ".json", ".xml", ".woff",
                                            ".woff2", ".ttf", ".txt", ".pdf", ".mp4", ".webm", ".mov", ".zip", ".map", ".avif"};

bool isPlaylistUrl(const std::string& url) {
    return kPlaylistExt.count(extensionOf(parseUrl(url).path)) > 0;
}

// How much a link looks like a radio stream (0: not one). strong: the page says it is audio (<audio>, <source>,
// a "stream" field)
int streamScore(const std::string& url, bool strong) {
    const UrlParts u = parseUrl(url);
    if ((u.scheme != "http" && u.scheme != "https") || u.host.empty()) return 0;
    const std::string ext = extensionOf(u.path);
    int score = 0;
    if (kAudioExt.count(ext)) score = 60;
    else if (ext == ".m3u8") score = 55;
    else if (kPlaylistExt.count(ext)) score = 50;
    else if (kWebExt.count(ext)) return 0;
    else if (u.port != 0 && u.port != 80 && u.port != 443) score = 35; // Icecast / Shoutcast servers
    else if (endsWith(u.path, "/;") || contains(u.path, "/stream") || contains(u.path, "/listen") || contains(u.path, "/live") ||
             contains(u.host, "stream") || contains(u.host, "icecast") || contains(u.host, "shoutcast") || startsWith(u.host, "ice")) score = 25;
    if (strong) score = std::max(score, 20) + 30;
    return score;
}

// ── Reading web pages ──

struct Deadline {
    const std::atomic<bool>* stop = nullptr;
    int64_t untilMs = 0;
};

int deadlineCallback(void* opaque) {
    const auto* d = static_cast<const Deadline*>(opaque);
    return ((d->stop && d->stop->load()) || nowMs() > d->untilMs) ? 1 : 0;
}

struct Fetched {
    bool ok = false;
    bool binary = false; // audio (or another file that is no text): not read
    std::string body;
    std::string mime;
    std::string finalUrl; // after redirects
    std::string error;
};

bool looksBinary(const std::string& s) {
    size_t odd = 0;
    for (unsigned char c : s) {
        if (c == 0 || (c < 0x09) || (c > 0x0D && c < 0x20)) ++odd;
    }
    return odd > s.size() / 100 + 2;
}

// readBinary: a picture (read whatever it is); otherwise a stream (audio) is recognised and not read
Fetched fetchUrl(const std::string& url, size_t maxBytes, const std::atomic<bool>& stop, bool readBinary = false,
                 int timeoutMs = kPageTimeoutMs) {
    Fetched f;
    Deadline d{&stop, nowMs() + timeoutMs};
    AVIOInterruptCB cb{deadlineCallback, &d};
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "user_agent", kUserAgent, 0);
    av_dict_set(&opts, "icy", "0", 0);
    av_dict_set(&opts, "rw_timeout", std::to_string(timeoutMs * 1000LL).c_str(), 0);
    av_dict_set(&opts, "headers", "Accept: text/html,application/xhtml+xml,application/json,*/*;q=0.8\r\n", 0);
    AVIOContext* pb = nullptr;
    const int ret = avio_open2(&pb, url.c_str(), AVIO_FLAG_READ, &cb, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        f.error = avError(ret);
        return f;
    }
    uint8_t* v = nullptr;
    if (av_opt_get(pb, "mime_type", AV_OPT_SEARCH_CHILDREN, &v) >= 0 && v) f.mime = lower(reinterpret_cast<char*>(v));
    av_freep(&v);
    if (av_opt_get(pb, "location", AV_OPT_SEARCH_CHILDREN, &v) >= 0 && v) f.finalUrl = reinterpret_cast<char*>(v);
    av_freep(&v);
    if (f.finalUrl.empty()) f.finalUrl = url;

    const bool textual = contains(f.mime, "mpegurl") || contains(f.mime, "scpls") || contains(f.mime, "pls") ||
                         startsWith(f.mime, "text/") || contains(f.mime, "json") || contains(f.mime, "xml") || contains(f.mime, "html");
    const bool audio = !textual && (startsWith(f.mime, "audio/") || startsWith(f.mime, "video/") || f.mime == "application/ogg");
    if (audio && !readBinary) {
        f.binary = true;
    } else {
        std::vector<unsigned char> buf(64 * 1024);
        while (f.body.size() < maxBytes) {
            const int n = avio_read(pb, buf.data(), static_cast<int>(std::min(buf.size(), maxBytes - f.body.size())));
            if (n <= 0) break;
            f.body.append(reinterpret_cast<const char*>(buf.data()), n);
            // A stream sent as "application/octet-stream" (or with no type): seen in its first bytes
            if (!readBinary && !textual && f.body.size() >= 4096 && looksBinary(f.body.substr(0, 4096))) {
                f.binary = true;
                f.body.clear();
                break;
            }
        }
        if (!readBinary && !f.binary && !textual && !f.body.empty() && looksBinary(f.body.substr(0, std::min<size_t>(f.body.size(), 4096)))) {
            f.binary = true;
            f.body.clear();
        }
    }
    avio_closep(&pb);
    f.ok = true;
    return f;
}

// .pls ("File1=...") or a plain .m3u list (an HLS playlist is no list: it plays itself)
bool isHlsPlaylist(const std::string& body) { return contains(body, "#EXT-X-"); }

std::vector<std::string> playlistEntries(const std::string& body, const std::string& base) {
    std::vector<std::string> out;
    const bool pls = contains(lower(body.substr(0, 512)), "[playlist]");
    size_t pos = 0;
    while (pos < body.size()) {
        size_t end = body.find_first_of("\r\n", pos);
        if (end == std::string::npos) end = body.size();
        std::string line = trim(body.substr(pos, end - pos));
        pos = end + 1;
        if (line.empty()) continue;
        if (pls) {
            if (!startsWith(lower(line), "file")) continue;
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            line = trim(line.substr(eq + 1));
        } else if (line[0] == '#') {
            continue;
        }
        const std::string url = resolveUrl(base, line);
        if (!url.empty()) out.push_back(url);
    }
    return out;
}

// ── What a page says about itself ──

struct PageInfo {
    std::string title;
    std::string logo; // og:image, apple-touch-icon, icon
};

// The value of attribute `name` in one tag ("" when missing)
std::string tagAttr(const std::string& tag, const std::string& name) {
    const std::string l = lower(tag);
    size_t pos = 0;
    while ((pos = l.find(name, pos)) != std::string::npos) {
        const bool boundary = pos == 0 || std::isspace(static_cast<unsigned char>(l[pos - 1])) || l[pos - 1] == '<';
        size_t eq = pos + name.size();
        while (eq < l.size() && std::isspace(static_cast<unsigned char>(l[eq]))) ++eq;
        if (boundary && eq < l.size() && l[eq] == '=') {
            size_t v = eq + 1;
            while (v < l.size() && std::isspace(static_cast<unsigned char>(l[v]))) ++v;
            if (v < l.size() && (l[v] == '"' || l[v] == '\'')) {
                const size_t close = l.find(l[v], v + 1);
                if (close != std::string::npos) return tag.substr(v + 1, close - v - 1);
            } else {
                size_t e = v;
                while (e < l.size() && !std::isspace(static_cast<unsigned char>(l[e])) && l[e] != '>') ++e;
                return tag.substr(v, e - v);
            }
        }
        pos += name.size();
    }
    return "";
}

PageInfo pageInfo(const std::string& html, const std::string& base) {
    PageInfo info;
    const std::string l = lower(html);
    std::string ogTitle, siteName, ogImage, touchIcon, icon;
    for (const std::string tagName : {"<meta", "<link"}) {
        size_t pos = 0;
        while ((pos = l.find(tagName, pos)) != std::string::npos) {
            const size_t end = l.find('>', pos);
            if (end == std::string::npos) break;
            const std::string tag = html.substr(pos, end - pos);
            pos = end;
            if (tagName == "<meta") {
                const std::string prop = lower(tagAttr(tag, "property") + tagAttr(tag, "name"));
                const std::string content = tagAttr(tag, "content");
                if (prop == "og:title" && ogTitle.empty()) ogTitle = content;
                else if (prop == "og:site_name" && siteName.empty()) siteName = content;
                else if ((prop == "og:image" || prop == "og:image:url" || prop == "twitter:image") && ogImage.empty()) ogImage = content;
            } else {
                const std::string rel = lower(tagAttr(tag, "rel"));
                const std::string href = tagAttr(tag, "href");
                if (contains(rel, "apple-touch-icon") && touchIcon.empty()) touchIcon = href;
                else if (contains(rel, "icon") && icon.empty()) icon = href;
            }
        }
    }
    std::string title;
    const size_t t = l.find("<title");
    if (t != std::string::npos) {
        const size_t s = l.find('>', t);
        const size_t e = l.find("</title", t);
        if (s != std::string::npos && e != std::string::npos && e > s) title = html.substr(s + 1, e - s - 1);
    }
    info.title = trim(decodeEntities(!ogTitle.empty() ? ogTitle : !title.empty() ? title : siteName));
    if (info.title.size() > 80) info.title = info.title.substr(0, 80);
    for (const std::string& cand : {ogImage, touchIcon, icon}) {
        info.logo = resolveUrl(base, cand);
        if (!info.logo.empty()) break;
    }
    if (info.logo.empty() && !originOf(base).empty()) info.logo = originOf(base) + "/favicon.ico";
    return info;
}

// ── Candidates ──

struct Candidate {
    Candidate() = default;
    Candidate(std::string u, int s) : url(std::move(u)), score(s) {}
    std::string url;
    int score = 0;
    // Known before opening it (radio-browser.info)
    std::string name, genre, language, country, logo;
};

void addCandidate(std::vector<Candidate>& list, std::set<std::string>& seen, Candidate c) {
    if (c.url.empty() || c.score <= 0 || seen.count(c.url)) return;
    seen.insert(c.url);
    list.push_back(std::move(c));
}

// Stream addresses on a page: absolute links that look like streams, attributes / JSON fields that hold them
void scanPage(const std::string& html, const std::string& base, std::vector<Candidate>& list, std::set<std::string>& seen) {
    std::string text = html;
    replaceAll(text, "\\/", "/");
    replaceAll(text, "\\u002F", "/");
    replaceAll(text, "\\u002f", "/");
    replaceAll(text, "\\u0026", "&");
    replaceAll(text, "&amp;", "&");
    const std::string l = lower(text);

    // 1. Absolute links anywhere (scripts included)
    for (const std::string scheme : {"http://", "https://"}) {
        size_t pos = 0;
        while ((pos = l.find(scheme, pos)) != std::string::npos) {
            size_t end = pos + scheme.size();
            while (end < text.size() && end - pos < 2048) {
                const char c = text[end];
                if (std::isspace(static_cast<unsigned char>(c)) || c == '"' || c == '\'' || c == '<' || c == '>' || c == '\\' ||
                    c == '(' || c == ')' || c == '`' || c == '{' || c == '}' || c == '[' || c == ']' || c == '|' || c == '^') break;
                ++end;
            }
            std::string url = text.substr(pos, end - pos);
            while (!url.empty() && (url.back() == '.' || url.back() == ',')) url.pop_back();
            addCandidate(list, seen, {url, streamScore(url, false)});
            pos = end;
        }
    }

    // 2. key="value" (attributes) and "key": "value" (JSON): relative links, and links the page marks as audio
    for (size_t i = 1; i + 1 < text.size(); ++i) {
        if (text[i] != '=' && text[i] != ':') continue;
        size_t v = i + 1;
        while (v < text.size() && (text[v] == ' ' || text[v] == '\t')) ++v;
        if (v >= text.size() || (text[v] != '"' && text[v] != '\'')) continue;
        const size_t close = text.find(text[v], v + 1);
        if (close == std::string::npos || close - v > 2048) continue;
        const std::string value = text.substr(v + 1, close - v - 1);
        if (value.empty() || value.find('\n') != std::string::npos) continue;

        // The key just before the '=' / ':'
        size_t k = i;
        while (k > 0 && (text[k - 1] == ' ' || text[k - 1] == '\t')) --k;
        std::string key;
        if (k >= 2 && (text[k - 1] == '"' || text[k - 1] == '\'')) {
            const size_t open = text.rfind(text[k - 1], k - 2);
            if (open != std::string::npos && k - 1 - open <= 40) key = l.substr(open + 1, k - 2 - open);
        } else {
            size_t s = k;
            while (s > 0 && k - s < 40 && (std::isalnum(static_cast<unsigned char>(text[s - 1])) || text[s - 1] == '-' || text[s - 1] == '_')) --s;
            key = l.substr(s, k - s);
        }
        if (key.empty()) continue;
        // The tag the attribute belongs to (<audio src>, <source src>)
        std::string tagName;
        if (text[i] == '=') {
            const size_t lt = l.rfind('<', i);
            const size_t gt = l.rfind('>', i);
            if (lt != std::string::npos && (gt == std::string::npos || gt < lt) && i - lt < 1024) {
                size_t e = lt + 1;
                while (e < l.size() && std::isalnum(static_cast<unsigned char>(l[e]))) ++e;
                tagName = l.substr(lt + 1, e - lt - 1);
            }
        }
        const bool strong = ((tagName == "audio" || tagName == "source") && key == "src") || contains(key, "stream") ||
                            key == "url_resolved" || key == "hls" || key == "mp3" || key == "aac" || key == "audio" || key == "listen_url";
        const bool linkKey = strong || key == "src" || key == "href" || key == "url" || key == "file" || startsWith(key, "data-");
        if (!linkKey) continue;
        const std::string url = resolveUrl(base, value);
        if (!url.empty()) addCandidate(list, seen, {url, streamScore(url, strong)});
        i = close;
    }
}

// radio-browser.info pages are built by scripts: its public API answers for a station (its id) or a search
bool radioBrowser(const std::string& url, std::vector<Candidate>& list, std::set<std::string>& seen, const std::atomic<bool>& stop,
                  std::string& title) {
    if (!contains(parseUrl(url).host, "radio-browser.info")) return false;
    std::string query;
    // 8-4-4-4-12 hex: a station's id
    for (size_t i = 0; i + 36 <= url.size() && query.empty(); ++i) {
        bool ok = true;
        for (size_t k = 0; k < 36 && ok; ++k) {
            const char c = url[i + k];
            ok = (k == 8 || k == 13 || k == 18 || k == 23) ? c == '-' : std::isxdigit(static_cast<unsigned char>(c)) != 0;
        }
        if (ok) query = "stations/byuuid/" + url.substr(i, 36);
    }
    if (query.empty()) {
        const size_t n = url.find("name=");
        if (n == std::string::npos) return false;
        const size_t e = url.find_first_of("&#", n);
        const std::string name = url.substr(n + 5, e == std::string::npos ? std::string::npos : e - n - 5);
        if (name.empty()) return false;
        query = "stations/search?name=" + name + "&limit=10&hidebroken=true&order=clickcount&reverse=true";
    }
    for (const char* server : {"https://de1.api.radio-browser.info/json/", "https://fi1.api.radio-browser.info/json/"}) {
        const Fetched f = fetchUrl(server + query, kMaxPageBytes, stop);
        if (!f.ok || f.body.empty()) continue;
        try {
            const auto j = nlohmann::json::parse(f.body);
            if (!j.is_array()) continue;
            for (const auto& st : j) {
                if (!st.is_object()) continue;
                Candidate c;
                c.url = st.value("url_resolved", "");
                if (c.url.empty()) c.url = st.value("url", "");
                c.score = 90;
                c.name = trim(st.value("name", ""));
                // Tags are in no order: the first that reads like a genre (not "97.1 fm")
                const std::string tags = st.value("tags", "");
                for (size_t p = 0; p <= tags.size() && c.genre.empty();) {
                    size_t e = tags.find(',', p);
                    if (e == std::string::npos) e = tags.size();
                    const std::string tag = trim(tags.substr(p, e - p));
                    if (!tag.empty() && tag.size() <= 24 && !contains(lower(c.name), lower(tag)) && std::none_of(tag.begin(), tag.end(), [](unsigned char ch) { return std::isdigit(ch); })) c.genre = tag;
                    p = e + 1;
                }
                if (!c.genre.empty()) c.genre[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(c.genre[0])));
                c.language = trim(st.value("language", ""));
                if (!c.language.empty()) c.language[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(c.language[0])));
                c.country = trim(st.value("country", ""));
                c.logo = st.value("favicon", "");
                if (title.empty()) title = c.name;
                addCandidate(list, seen, c);
            }
            return true;
        } catch (const nlohmann::json::exception& e) {
            Logger::instance().warn("RadioDiscovery", "radio-browser.info answered something unreadable: {}", e.what());
        }
    }
    return true;
}

// radio.garden pages are built by scripts too: for a station link (/listen/<name>/<id>) its public API names
// the channel, and its listen address redirects to the station's own stream (that address is the one saved,
// so the station keeps playing without radio.garden). The logo comes from the station's own website.
bool radioGarden(const std::string& url, std::vector<Candidate>& list, std::set<std::string>& seen, const std::atomic<bool>& stop,
                 std::string& title) {
    const std::string host = parseUrl(url).host;
    if (host != "radio.garden" && !endsWith(host, ".radio.garden")) return false;
    const size_t listen = url.find("/listen/");
    if (listen == std::string::npos) return false; // a place / search page: the general scan answers
    const size_t slash = url.find('/', listen + 8);
    if (slash == std::string::npos) return false;
    const size_t end = url.find_first_of("/?#", slash + 1);
    const std::string id = url.substr(slash + 1, end == std::string::npos ? std::string::npos : end - slash - 1);
    if (id.empty() || id.size() > 16 || !std::all_of(id.begin(), id.end(), [](unsigned char ch) { return std::isalnum(ch) || ch == '-' || ch == '_'; })) {
        return false;
    }

    Candidate c;
    c.score = 95;
    std::string website;
    const Fetched info = fetchUrl("https://radio.garden/api/ara/content/channel/" + id, 256 * 1024, stop);
    if (info.ok && !info.body.empty()) {
        try {
            const auto j = nlohmann::json::parse(info.body);
            const auto data = j.value("data", nlohmann::json::object());
            c.name = trim(data.value("title", ""));
            if (data.contains("country") && data["country"].is_object()) c.country = trim(data["country"].value("title", ""));
            website = data.value("website", "");
        } catch (const nlohmann::json::exception& e) {
            Logger::instance().warn("RadioDiscovery", "radio.garden answered something unreadable: {}", e.what());
        }
    } else {
        Logger::instance().warn("RadioDiscovery", "radio.garden channel '{}' could not be read: {}", id, info.error);
    }
    // The logo (from the station's website) is read while the listen address is followed
    std::thread logoJob;
    if (!website.empty()) {
        logoJob = std::thread([&c, &stop, website]() {
            const Fetched site = fetchUrl(website, kLogoPageBytes, stop, false, kLogoPageTimeoutMs);
            if (site.ok && !site.binary && !site.body.empty()) c.logo = pageInfo(site.body, site.finalUrl).logo;
            else if (!originOf(website).empty()) c.logo = originOf(website) + "/favicon.ico"; // a slow site: its icon, if any
        });
    }
    // The redirect's target (an audio answer is not read); the listen address itself when there was none
    const std::string listenUrl = "https://radio.garden/api/ara/content/listen/" + id + "/channel.mp3";
    const Fetched stream = fetchUrl(listenUrl, kMaxPlaylistBytes, stop);
    c.url = (stream.ok && !stream.finalUrl.empty()) ? stream.finalUrl : listenUrl;
    if (logoJob.joinable()) logoJob.join();
    if (title.empty()) title = c.name;
    addCandidate(list, seen, c);
    return true;
}

// gtaradio.net (the GTA games' radio stations): a game's page (/<lang>/<game>, e.g. /en/sa) has no stream links,
// its script builds the station list. That script holds the stations' keys ("/resources/images/games/<game>/
// stations/<key>.png"), names ({"bounce-fm":"Bounce FM", csr:"CSR 103.9", ...}) and logos; every station is an
// Icecast stream at https://audio.gtaradio.net/<game>/<key>. All of a game's stations are returned to pick from.
constexpr const char* kGtaRadioStreams = "https://audio.gtaradio.net";
constexpr size_t kGtaRadioMaxStations = 24;

// The string literal right after `pos` ("..."), empty when there is none
std::string jsStringAt(const std::string& js, size_t pos) {
    if (pos >= js.size() || js[pos] != '"') return "";
    const size_t close = js.find('"', pos + 1);
    if (close == std::string::npos || close - pos > 256) return "";
    return js.substr(pos + 1, close - pos - 1);
}

bool gtaRadio(const std::string& url, std::vector<Candidate>& list, std::set<std::string>& seen, const std::atomic<bool>& stop,
              std::string& title) {
    const std::string host = parseUrl(url).host;
    if (host != "gtaradio.net" && host != "www.gtaradio.net") return false;
    const Fetched page = fetchUrl(url, kMaxPageBytes, stop);
    if (!page.ok || page.binary) return false;
    // The game's script: the page's module script (the shared audio-*.js is imported by it)
    std::string scriptUrl;
    for (size_t pos = 0; scriptUrl.empty() && (pos = page.body.find("<script", pos)) != std::string::npos; ++pos) {
        const size_t end = page.body.find('>', pos);
        if (end == std::string::npos) break;
        const std::string tag = page.body.substr(pos, end - pos);
        if (!contains(tag, "type=\"module\"")) continue;
        const std::string src = tagAttr(tag, "src");
        if (endsWith(lower(src), ".js") && !contains(src, "/audio-")) scriptUrl = resolveUrl(page.finalUrl, src);
    }
    if (scriptUrl.empty()) {
        Logger::instance().info("RadioDiscovery", "gtaradio.net: no game script on {} (the site's home page?)", url);
        return false; // the general scan answers
    }
    const Fetched script = fetchUrl(scriptUrl, kMaxPageBytes, stop);
    if (!script.ok || script.binary || script.body.empty()) {
        Logger::instance().warn("RadioDiscovery", "gtaradio.net: the game script {} could not be read: {}", scriptUrl, script.error);
        return false;
    }
    const std::string& js = script.body;
    const std::string keyPrefix = "\"/resources/images/games/";
    const PageInfo info = pageInfo(page.body, page.finalUrl);
    // "GTA San Andreas Radio — ..." -> "GTA San Andreas" (the stations' genre line)
    std::string game = info.title.substr(0, info.title.find(" Radio"));
    if (game.size() > 40) game.clear();

    size_t added = 0;
    std::set<std::string> keys;
    for (size_t pos = 0; added < kGtaRadioMaxStations && (pos = js.find(keyPrefix, pos)) != std::string::npos; ++pos) {
        const std::string path = jsStringAt(js, pos); // /resources/images/games/sa/stations/bounce-fm.png
        const size_t stations = path.find("/stations/");
        if (stations == std::string::npos || !endsWith(path, ".png")) continue;
        const std::string gameKey = path.substr(keyPrefix.size() - 1, stations - (keyPrefix.size() - 1));
        const std::string key = path.substr(stations + 10, path.size() - stations - 10 - 4);
        if (gameKey.empty() || key.empty() || gameKey.find('/') != std::string::npos || !keys.insert(gameKey + "/" + key).second) continue;

        Candidate c;
        c.url = std::string(kGtaRadioStreams) + "/" + gameKey + "/" + key;
        c.score = 95;
        c.genre = game;
        // Name: "key":"Name" or key:"Name" (an identifier key is written bare)
        size_t at = js.find("\"" + key + "\":\"");
        if (at != std::string::npos) c.name = jsStringAt(js, at + key.size() + 3);
        for (const char before : {',', '{'}) {
            if (!c.name.empty()) break;
            at = js.find(std::string(1, before) + key + ":\"");
            if (at != std::string::npos) c.name = jsStringAt(js, at + key.size() + 2);
        }
        if (c.name.empty()) c.name = key;
        // Logo: "<path>":<var>, then <var>="/build/...png"
        const size_t mapAt = pos + path.size() + 2;
        if (mapAt < js.size() && js[mapAt] == ':') {
            size_t e = mapAt + 1;
            while (e < js.size() && (std::isalnum(static_cast<unsigned char>(js[e])) || js[e] == '_' || js[e] == '$')) ++e;
            const std::string var = js.substr(mapAt + 1, e - mapAt - 1);
            for (const char before : {',', ' '}) {
                if (var.empty() || !c.logo.empty()) break;
                const size_t def = js.find(std::string(1, before) + var + "=\"");
                if (def != std::string::npos) c.logo = resolveUrl(script.finalUrl, jsStringAt(js, def + var.size() + 2));
            }
        }
        addCandidate(list, seen, c);
        ++added;
    }
    if (added == 0) {
        Logger::instance().info("RadioDiscovery", "gtaradio.net: no stations in {}", scriptUrl);
        return false;
    }
    if (title.empty()) title = !game.empty() ? game + " Radio" : info.title;
    Logger::instance().info("RadioDiscovery", "gtaradio.net: {} stations of '{}'", added, game);
    return true;
}

// ── Opening a candidate like the radio would ──

struct Found {
    std::string url, name, genre, language, country, codec, logo, error;
    int bitrate = 0; // kbit/s, 0: unknown
    bool hls = false;
    int score = 0;
};

bool genericName(const std::string& name) {
    const std::string l = lower(trim(name));
    return l.empty() || l == "no name" || l == "unnamed" || l == "unspecified" || l == "unknown" || l == "stream" || l == "radio" ||
           l == "icecast" || l == "shoutcast" || l == "untitled" || l == "-" || startsWith(l, "this is my server") ||
           startsWith(l, "unspecified");
}

// "BBC Radio 1 - The biggest new pop and all-day vibes" -> "BBC Radio 1" (a long name with a slogan)
std::string shortName(std::string name) {
    name = trim(name);
    if (name.size() > 32) {
        size_t cut = std::string::npos;
        for (const char* sep : {" - ", " | ", " || ", ": ", " :: ", " – "}) {
            const size_t p = name.find(sep);
            if (p != std::string::npos && p >= 3 && p < cut) cut = p;
        }
        if (cut != std::string::npos) name = trim(name.substr(0, cut));
    }
    if (name.size() > 60) {
        size_t cut = 60;
        while (cut > 0 && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80) --cut; // not inside a UTF-8 letter
        name = trim(name.substr(0, cut));
    }
    return name;
}

std::string headerValue(const std::string& headers, const std::string& name) {
    const std::string l = lower(headers);
    size_t pos = 0;
    while ((pos = l.find(name + ":", pos)) != std::string::npos) {
        if (pos == 0 || l[pos - 1] == '\n') {
            const size_t s = pos + name.size() + 1;
            const size_t e = headers.find_first_of("\r\n", s);
            return trim(headers.substr(s, e == std::string::npos ? std::string::npos : e - s));
        }
        pos += name.size();
    }
    return "";
}

std::string codecLabel(AVCodecID id) {
    switch (id) {
        case AV_CODEC_ID_MP3:    return "MP3";
        case AV_CODEC_ID_AAC:    return "AAC";
        case AV_CODEC_ID_OPUS:   return "Opus";
        case AV_CODEC_ID_VORBIS: return "Vorbis";
        case AV_CODEC_ID_FLAC:   return "FLAC";
        default: {
            std::string n = avcodec_get_name(id);
            for (auto& c : n) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return n;
        }
    }
}

bool probe(const Candidate& c, const std::atomic<bool>& stop, Found& out) {
    out.url = c.url;
    Deadline d{&stop, nowMs() + kProbeTimeoutMs};
    AVFormatContext* in = avformat_alloc_context();
    if (!in) return false;
    in->interrupt_callback.callback = deadlineCallback;
    in->interrupt_callback.opaque = &d;
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "user_agent", kUserAgent, 0);
    av_dict_set(&opts, "icy", "1", 0);
    av_dict_set(&opts, "rw_timeout", std::to_string(kProbeTimeoutMs * 1000LL).c_str(), 0);
    av_dict_set(&opts, "probesize", "65536", 0);
    av_dict_set(&opts, "analyzeduration", "1000000", 0);
    int ret = avformat_open_input(&in, c.url.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        out.error = avError(ret);
        return false; // avformat_open_input freed `in`
    }
    bool ok = false;
    if ((ret = avformat_find_stream_info(in, nullptr)) < 0) {
        out.error = avError(ret);
    } else {
        const int ai = av_find_best_stream(in, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        bool video = false;
        for (unsigned i = 0; i < in->nb_streams; ++i) {
            const AVStream* st = in->streams[i];
            if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !(st->disposition & AV_DISPOSITION_ATTACHED_PIC)) video = true;
        }
        if (ai < 0) {
            out.error = "no audio";
        } else if (video) {
            out.error = "a video (TV) stream";
        } else if (in->duration != AV_NOPTS_VALUE && in->duration > 0) {
            out.error = "a recording, not a live station";
        } else {
            const AVStream* st = in->streams[ai];
            out.codec = codecLabel(st->codecpar->codec_id);
            out.hls = in->iformat && contains(lower(in->iformat->name), "hls");
            uint8_t* v = nullptr;
            std::string headers;
            if (av_opt_get(in, "icy_metadata_headers", AV_OPT_SEARCH_CHILDREN, &v) >= 0 && v) headers = reinterpret_cast<char*>(v);
            av_freep(&v);
            out.name = toValidUtf8(headerValue(headers, "icy-name"));
            out.genre = toValidUtf8(headerValue(headers, "icy-genre"));
            out.bitrate = std::atoi(headerValue(headers, "icy-br").c_str());
            if (out.bitrate <= 0 && st->codecpar->bit_rate > 0) out.bitrate = static_cast<int>(st->codecpar->bit_rate / 1000);
            if (out.bitrate <= 0) {
                const AVDictionaryEntry* vb = av_dict_get(st->metadata, "variant_bitrate", nullptr, 0);
                if (vb) out.bitrate = std::atoi(vb->value) / 1000;
            }
            if (out.bitrate <= 0 && in->bit_rate > 0) out.bitrate = static_cast<int>(in->bit_rate / 1000);
            if (out.bitrate > 2000) out.bitrate = 0; // nonsense
            ok = true;
        }
    }
    avformat_close_input(&in);
    return ok;
}

std::string sanitizeName(const std::string& name) {
    std::string out;
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') out += c;
        else if (!out.empty() && out.back() != '_') out += '_';
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (out.size() > 60) out = out.substr(0, 60);
    return out.empty() ? "station" : out;
}

// A picture's type from its first bytes ("" when it is none the screens show)
std::string imageExtension(const std::string& data, const std::string& mime) {
    auto at = [&](size_t i) { return i < data.size() ? static_cast<unsigned char>(data[i]) : 0; };
    if (at(0) == 0x89 && data.compare(1, 3, "PNG") == 0) return ".png";
    if (at(0) == 0xFF && at(1) == 0xD8) return ".jpg";
    if (data.compare(0, 4, "GIF8") == 0) return ".gif";
    if (data.compare(0, 4, "RIFF") == 0 && data.size() > 12 && data.compare(8, 4, "WEBP") == 0) return ".webp";
    if (at(0) == 0 && at(1) == 0 && at(2) == 1 && at(3) == 0) return ".ico";
    const std::string head = lower(data.substr(0, 512));
    if (contains(mime, "svg") || contains(head, "<svg")) return ".svg";
    return "";
}

std::filesystem::path pathFromUtf8(const std::string& s) {
#if defined(_WIN32)
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring ws(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), ws.data(), n);
    return std::filesystem::path(ws);
#else
    return std::filesystem::path(s);
#endif
}

std::string dumpJson(const nlohmann::json& j) {
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

} // anonymous namespace

// ─── RadioDiscovery ──────────────────────────────────────────────────────────

RadioDiscovery::~RadioDiscovery() {
    stop_ = true;
    std::lock_guard lock(mutex_);
    for (auto& job : jobs_) {
        if (job.thread.joinable()) job.thread.join();
    }
}

void RadioDiscovery::start(std::function<void()> work) {
    std::lock_guard lock(mutex_);
    // Finished jobs are joined here (a search never outlives this object)
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        if (it->done->load()) {
            if (it->thread.joinable()) it->thread.join();
            it = jobs_.erase(it);
        } else {
            ++it;
        }
    }
    Job job;
    auto done = job.done;
    job.thread = std::thread([work = std::move(work), done]() {
        work();
        done->store(true);
    });
    jobs_.push_back(std::move(job));
}

void RadioDiscovery::discover(const std::string& requestId, const std::string& rawUrl, Sink sink) {
    start([this, requestId, rawUrl, sink]() {
        auto send = [&](nlohmann::json j) {
            j["requestId"] = requestId;
            if (sink.send) sink.send(dumpJson(j));
        };
        // hint: why nothing was found, when it is known (a site behind a bot check)
        auto finish = [&](const std::vector<Found>& streams, const std::string& title, const std::string& error,
                          const std::string& hint = "") {
            nlohmann::json list = nlohmann::json::array();
            for (const auto& s : streams) {
                list.push_back({{"url", s.url}, {"name", s.name}, {"genre", s.genre}, {"language", s.language}, {"country", s.country},
                                {"codec", s.codec}, {"bitrate", s.bitrate}, {"hls", s.hls}, {"logo", s.logo}});
            }
            send({{"event", "RADIO_DISCOVER_RESULT"}, {"status", !error.empty() ? "error" : streams.empty() ? "empty" : "success"},
                  {"streams", list}, {"title", title}, {"error", error}, {"hint", hint}});
        };

        std::string url = trim(rawUrl);
        if (!url.empty() && !contains(url, "://")) url = "https://" + url; // "somafm.com/groovesalad"
        const UrlParts parts = parseUrl(url);
        const bool hostOk = !parts.host.empty() && parts.host.find_first_of(" \t") == std::string::npos &&
                            (contains(parts.host, ".") || parts.host == "localhost");
        if ((parts.scheme != "http" && parts.scheme != "https") || !hostOk) {
            finish({}, "", "That is not a web address (it should look like https://...)");
            return;
        }
        Logger::instance().info("RadioDiscovery", "'{}': looking for radio streams at {}", requestId, url);
        send({{"event", "RADIO_DISCOVER_PROGRESS"}, {"stage", "page"}, {"checked", 0}, {"total", 0}});

        std::vector<Candidate> candidates;
        std::set<std::string> seen;
        std::string title;
        std::string pageLogo;

        // A site that lists a whole set of stations is checked in full (gtaradio.net: up to kGtaRadioMaxStations)
        size_t maxProbes = kMaxProbes;
        if (gtaRadio(url, candidates, seen, stop_, title)) {
            maxProbes = std::max(kMaxProbes, kGtaRadioMaxStations);
        } else if (!radioBrowser(url, candidates, seen, stop_, title) && !radioGarden(url, candidates, seen, stop_, title)) {
            const Fetched page = fetchUrl(url, kMaxPageBytes, stop_);
            if (stop_) return;
            if (!page.ok) {
                // Some stream servers refuse plain reads but still play: the link itself is tried
                Logger::instance().info("RadioDiscovery", "'{}': could not read {} ({}); trying it as a stream", requestId, url, page.error);
                addCandidate(candidates, seen, {url, 100});
            } else if (page.binary) {
                addCandidate(candidates, seen, {url, 100});
            } else if (isHlsPlaylist(page.body)) {
                addCandidate(candidates, seen, {url, 100});
            } else if (isPlaylistUrl(url) || contains(page.mime, "mpegurl") || contains(page.mime, "scpls") ||
                       contains(lower(page.body.substr(0, 512)), "[playlist]")) {
                for (const auto& e : playlistEntries(page.body, page.finalUrl)) addCandidate(candidates, seen, {e, 95});
            } else {
                const PageInfo info = pageInfo(page.body, page.finalUrl);
                title = info.title;
                pageLogo = info.logo;
                scanPage(page.body, page.finalUrl, candidates, seen);
                Logger::instance().info("RadioDiscovery", "'{}': page '{}' ({} KB), {} possible streams", requestId, title,
                                        page.body.size() / 1024, candidates.size());
            }
        }

        // Playlists found on the page are opened: their first entries are the candidates
        std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
        std::vector<Candidate> expanded;
        std::vector<const Candidate*> lists;
        for (const auto& c : candidates) {
            if (!isPlaylistUrl(c.url)) expanded.push_back(c);
            else if (lists.size() < kMaxPlaylists) lists.push_back(&c);
        }
        std::vector<Fetched> listBodies(lists.size());
        {
            std::vector<std::thread> fetches;
            for (size_t i = 0; i < lists.size(); ++i) {
                fetches.emplace_back([&, i]() { listBodies[i] = fetchUrl(lists[i]->url, kMaxPlaylistBytes, stop_); });
            }
            for (auto& t : fetches) t.join();
        }
        if (stop_) return;
        for (size_t li = 0; li < lists.size(); ++li) {
            const Candidate& c = *lists[li];
            const Fetched& pl = listBodies[li];
            if (!pl.ok) continue;
            if (pl.binary || isHlsPlaylist(pl.body)) {
                expanded.push_back(c); // named .m3u but plays itself
                continue;
            }
            size_t taken = 0;
            for (const auto& e : playlistEntries(pl.body, pl.finalUrl)) {
                if (taken >= kEntriesPerList) break;
                if (seen.count(e) && e != c.url) continue;
                seen.insert(e);
                Candidate entry = c;
                entry.url = e;
                expanded.push_back(entry);
                ++taken;
            }
        }
        std::stable_sort(expanded.begin(), expanded.end(), [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
        if (expanded.size() > maxProbes) expanded.resize(maxProbes);

        // Every candidate is opened (a few at a time)
        std::vector<Found> found(expanded.size());
        std::vector<char> works(expanded.size(), 0);
        std::atomic<int> checked{0};
        const int total = static_cast<int>(expanded.size());
        send({{"event", "RADIO_DISCOVER_PROGRESS"}, {"stage", "checking"}, {"checked", 0}, {"total", total}});
        for (size_t first = 0; first < expanded.size() && !stop_; first += kProbesAtOnce) {
            std::vector<std::thread> batch;
            for (size_t i = first; i < std::min(expanded.size(), first + kProbesAtOnce); ++i) {
                batch.emplace_back([&, i]() {
                    works[i] = probe(expanded[i], stop_, found[i]) ? 1 : 0;
                    const int n = ++checked;
                    if (!works[i]) {
                        Logger::instance().info("RadioDiscovery", "'{}': not a station: {} ({})", requestId, expanded[i].url, found[i].error);
                    }
                    send({{"event", "RADIO_DISCOVER_PROGRESS"}, {"stage", "checking"}, {"checked", n}, {"total", total}});
                });
            }
            for (auto& t : batch) t.join();
        }
        if (stop_) return;

        // The ones that play: named, best first (codec, bit rate), the same stream once
        std::vector<Found> streams;
        for (size_t i = 0; i < expanded.size(); ++i) {
            if (!works[i]) continue;
            Found f = found[i];
            const Candidate& c = expanded[i];
            if (!c.name.empty()) f.name = c.name;
            else if (genericName(f.name)) f.name = !title.empty() ? title : parseUrl(f.url).host;
            if (!c.genre.empty()) f.genre = c.genre;
            if (genericName(f.genre)) f.genre.clear();
            f.genre = f.genre.substr(0, f.genre.find_first_of(",;/"));
            f.genre = trim(f.genre);
            f.name = shortName(f.name);
            f.language = c.language;
            f.country = c.country;
            f.logo = c.logo;
            f.score = c.score + (f.codec == "AAC" || f.codec == "Opus" ? 6 : f.codec == "MP3" ? 4 : 0) +
                      std::min(f.bitrate, 320) / 16 + (startsWith(lower(f.url), "https://") ? 2 : 0);
            streams.push_back(std::move(f));
        }
        // The page's logo is the station's when the page is about one station (a directory page lists many: its
        // icon is the directory's)
        std::set<std::string> names;
        for (const auto& s : streams) names.insert(lower(s.name));
        if (names.size() == 1) {
            for (auto& s : streams) {
                if (s.logo.empty()) s.logo = pageLogo;
            }
        }
        std::stable_sort(streams.begin(), streams.end(), [](const Found& a, const Found& b) { return a.score > b.score; });
        std::vector<Found> unique;
        std::set<std::string> keys;
        for (auto& s : streams) {
            const std::string key = lower(s.name) + "|" + s.codec + "|" + std::to_string(s.bitrate);
            if (keys.insert(key).second) unique.push_back(std::move(s));
        }
        Logger::instance().info("RadioDiscovery", "'{}': {} station stream(s) found of {} checked", requestId, unique.size(), total);
        // Nothing plays: was it a bot check (Cloudflare "Just a moment...")? Then the page's player gets its streams
        // only in a real browser, and saying so helps more than "no streams" (the user can paste the stream itself)
        std::string hint;
        if (unique.empty() && !stop_) {
            std::vector<std::string> checkUrls{url};
            for (size_t i = 0; i < expanded.size() && checkUrls.size() < 4; ++i) checkUrls.push_back(expanded[i].url);
            for (const auto& u : checkUrls) {
                const Fetched f = fetchUrl(u, 64 * 1024, stop_, false, 6000);
                const std::string b = lower(f.body);
                if (f.ok && !f.binary && (contains(b, "challenges.cloudflare.com") || contains(b, "<title>just a moment") ||
                                          contains(b, "cf-chl") || contains(b, "attention required! | cloudflare"))) {
                    Logger::instance().info("RadioDiscovery", "'{}': {} answers with a bot check", requestId, u);
                    hint = "This site hides its streams behind a bot check, so they can only be played on its own page. "
                           "Paste the station's stream link instead, or find the station on another site.";
                    break;
                }
            }
        }
        finish(unique, shortName(title), "", hint);
    });
}

void RadioDiscovery::saveLogo(const std::string& requestId, const std::string& url, const std::string& targetDir,
                              const std::string& name, Sink sink) {
    start([this, requestId, url, targetDir, name, sink]() {
        auto reply = [&](const std::string& file, const std::string& error) {
            if (!sink.send) return;
            sink.send(dumpJson({{"event", "RADIO_LOGO_SAVED"}, {"requestId", requestId}, {"status", error.empty() ? "success" : "error"},
                                {"file", file}, {"error", error}}));
        };
        const UrlParts parts = parseUrl(url);
        if ((parts.scheme != "http" && parts.scheme != "https") || targetDir.empty()) {
            reply("", "No logo address");
            return;
        }
        const Fetched f = fetchUrl(url, kMaxLogoBytes, stop_, true);
        const std::string& data = f.body;
        if (!f.ok || data.empty()) {
            reply("", "The logo could not be downloaded" + (f.error.empty() ? std::string() : ": " + f.error));
            return;
        }
        const std::string ext = imageExtension(data, f.mime);
        if (ext.empty()) {
            reply("", "The logo is not a picture the screens can show");
            return;
        }
        const std::string file = sanitizeName(name) + ext;
        std::error_code ec;
        const std::filesystem::path dir = pathFromUtf8(targetDir);
        std::filesystem::create_directories(dir, ec);
        std::ofstream out(dir / pathFromUtf8(file), std::ios::binary | std::ios::trunc);
        if (!out) {
            reply("", "The logo could not be saved in " + targetDir);
            return;
        }
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.close();
        Logger::instance().info("RadioDiscovery", "'{}': logo saved as {} ({} KB)", requestId, file, data.size() / 1024);
        reply(file, "");
    });
}

} // namespace ifs
