#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ifs {

/// Radio app, "Add station": finds the radio streams behind any link the user pastes.
///
/// RADIO_DISCOVER {requestId, url}:
///   1. the link itself (a stream, an HLS .m3u8, a .pls / .m3u playlist),
///   2. radio-browser.info station / search links, radio.garden station links and gtaradio.net game pages (all
///      three are built by scripts: their public APIs / the game's script answer; a gtaradio.net page gives all
///      its stations to pick from),
///   3. any other web page: downloaded (at most kMaxPageBytes) and searched for stream addresses (.m3u8 /
///      .mp3 / .aac / .ogg / .opus links, Icecast / Shoutcast ports, <audio> / <source>, "stream" fields,
///      .pls / .m3u playlists, which are opened).
///   Every candidate (at most kMaxProbes) is opened like the radio would open it; the ones that send audio are
///   returned with their name (Icecast icy-name, else the page title), genre, codec, bit rate and logo address.
///   Events: RADIO_DISCOVER_PROGRESS {stage, checked, total}, RADIO_DISCOVER_RESULT {status, streams[], title,
///   error}.
/// RADIO_SAVE_LOGO {requestId, url, targetDir, name}: downloads a station logo (png / jpg / gif / webp / ico /
///   svg, at most 2 MB) as targetDir/<name>.<ext>. Event: RADIO_LOGO_SAVED {status, file, error}.
class RadioDiscovery {
public:
    struct Sink {
        std::function<void(const std::string&)> send;
        std::function<bool()> isConnected;
    };

    RadioDiscovery() = default;
    ~RadioDiscovery();

    RadioDiscovery(const RadioDiscovery&)            = delete;
    RadioDiscovery& operator=(const RadioDiscovery&) = delete;

    void discover(const std::string& requestId, const std::string& url, Sink sink);
    void saveLogo(const std::string& requestId, const std::string& url, const std::string& targetDir,
                  const std::string& name, Sink sink);

private:
    struct Job {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
    };
    void start(std::function<void()> work);

    std::mutex mutex_;
    std::vector<Job> jobs_;
    std::atomic<bool> stop_{false};
};

} // namespace ifs
