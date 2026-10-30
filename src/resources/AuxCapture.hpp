#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace ifs {

/// Aux (the screen's Aux app): VB-CABLE's "CABLE Output" (what Windows apps play into "CABLE Input") on the bus
/// speakers. The game's own browser refuses audio inputs (NotAllowedError), so the user's Google Chrome (else
/// Microsoft Edge) captures it, hidden, with its own small profile and its sound output off, and sends it to the
/// game UI's sound system over WebRTC (stereo Opus, 256 kbit/s). Windows' microphone access must be on: VB-CABLE
/// counts as a microphone ("Permission denied by system" otherwise: error "blocked").
///
/// Sound system -> service: AUX_START {requestId, deviceLabel ("" = the first "CABLE Output")},
///                          AUX_ANSWER {requestId, sdp}, AUX_STOP {requestId}
/// Service -> sound system: AUX_STATE {state, detail, label}, AUX_OFFER {sdp},
///                          AUX_ERROR {error: blocked | nocable | missing | nobrowser | <message>, detail}, AUX_STOPPED
/// One capture at a time. AUX_START while it runs (a reloaded game UI) keeps the browser and offers the link again;
/// AUX_STOP closes the browser. The game UI gone for a while closes it too.
class AuxCapture {
public:
    struct Sink {
        std::function<void(const std::string&)> send;
        std::function<bool()> isConnected;
    };

    AuxCapture() = default;
    ~AuxCapture();

    AuxCapture(const AuxCapture&)            = delete;
    AuxCapture& operator=(const AuxCapture&) = delete;

    void start(const std::string& requestId, const std::string& deviceLabel, Sink sink);
    void answer(const std::string& requestId, const std::string& sdp);
    void stop(const std::string& requestId);

    struct Session;

private:
    std::mutex mutex_;
    std::shared_ptr<Session> session_;
};

} // namespace ifs
