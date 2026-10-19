#pragma once

#include "tts/KokoroEngine.hpp"
#include "tts/TtsTypes.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>

namespace ifs {

/// Text-to-speech: queues TTS_SPEAK requests and streams Kokoro audio back to the requester.
///
/// Threading: public methods are thread-safe (called from the WebSocket io thread).
/// Synthesis runs on one dedicated worker, one request at a time, in arrival order.
/// The engine is loaded by TTS_PRELOAD, on first use, or at start() when the config's preload is set,
/// and reloaded automatically when the voices folder changes.
class TtsService {
public:
    /// Where a request's events go. Called from the worker thread.
    struct Sink {
        const void*                             owner = nullptr;  // identifies the client, scopes cancel()
        std::function<void(const std::string&)> sendText;
        std::function<void(std::string)>        sendBinary;
        std::function<bool()>                   isConnected;
    };

    explicit TtsService(TtsConfig config);
    ~TtsService();

    TtsService(const TtsService&)            = delete;
    TtsService& operator=(const TtsService&) = delete;

    /// Starts the worker. Call once after configuration.
    void start();

    /// Stops the worker; the request in progress ends after its current sentence.
    void stop();

    /// Voices on disk and engine state. Cheap: a folder listing, no engine access.
    VoicesResult listVoices(const std::string& requestId) const;

    /// Validates and queues a request. Errors are reported through the sink.
    void speak(SpeakRequest request, Sink sink);

    /// Queues an engine load so the next TTS_SPEAK starts without the load delay.
    /// Answers TTS_PRELOADED through the sink; a no-op when the engine is already loaded.
    void preload(std::string requestId, Sink sink);

    /// Cancels a queued or active request of `owner`. The client gets TTS_FINISHED "cancelled".
    void cancel(const void* owner, const std::string& requestId);

private:
    struct Job {
        SpeakRequest                       request;
        Sink                               sink;
        std::shared_ptr<std::atomic<bool>> cancelled;
        bool                               preloadOnly = false;  // TTS_PRELOAD: load the engine, no speech
    };

    void run(std::stop_token stopToken);
    void process(Job& job, const std::stop_token& stopToken);
    void processPreload(Job& job);
    bool ensureEngine(const std::vector<VoiceInfo>& voices, std::string& error);  // worker thread only

    TtsConfig config_;

    mutable std::mutex           mutex_;
    std::condition_variable_any  wake_;
    std::deque<Job>              queue_;
    std::shared_ptr<Job>         active_;  // job being synthesised, for cancel()

    std::atomic<bool>     engineLoaded_{false};
    std::atomic<uint32_t> nextStreamId_{1};

    // Worker thread only
    std::unique_ptr<KokoroEngine>            engine_;
    std::string                              loadedSignature_;
    std::unordered_map<std::string, int32_t> speakerIds_;

    std::jthread worker_;  // declared last: stopped and joined before the members above are destroyed
};

} // namespace ifs
