#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace ifs {

/// TEMPORARY: the in-game WebRTC check (Settings > Diagnostics > "WebRTC check") for the planned web apps
/// (YouTube in a hidden Chrome, streamed to the vehicle screen).
///
/// WEBRTC_PROBE_START {requestId}: starts the user's Google Chrome (else Microsoft Edge) hidden, with its own
/// profile next to the service and its sound output disabled (a Windows job object ends it with the service).
/// Chrome is driven over the DevTools protocol: a blank page draws a moving test pattern with a frame counter
/// and a 440 Hz tone, and offers them over WebRTC (no internet involved). The offer goes to the screen, the
/// screen's answer comes back:
///   events WEBRTC_PROBE_STATE {state, detail}, WEBRTC_PROBE_OFFER {sdp}, WEBRTC_PROBE_STATS {stats},
///          WEBRTC_PROBE_ERROR {error}, WEBRTC_PROBE_STOPPED
///   WEBRTC_PROBE_ANSWER {requestId, sdp}, WEBRTC_PROBE_STOP {requestId}
/// audioOnly (the game UI's sound system asking): the tone only, no picture.
/// One check at a time; it stops by itself after kTimeout.
class WebRtcProbe {
public:
    struct Sink {
        std::function<void(const std::string&)> send;
        std::function<bool()> isConnected;
    };

    WebRtcProbe() = default;
    ~WebRtcProbe();

    WebRtcProbe(const WebRtcProbe&)            = delete;
    WebRtcProbe& operator=(const WebRtcProbe&) = delete;

    void start(const std::string& requestId, Sink sink, bool audioOnly = false);
    void answer(const std::string& requestId, const std::string& sdp);
    void stop(const std::string& requestId);

    struct Session;

private:
    std::mutex mutex_;
    std::shared_ptr<Session> session_;
};

} // namespace ifs
