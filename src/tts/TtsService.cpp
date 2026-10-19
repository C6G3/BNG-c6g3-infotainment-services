#include "tts/TtsService.hpp"
#include "tts/TextSplitter.hpp"
#include "tts/VoiceLibrary.hpp"
#include "core/Logger.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace ifs {

namespace {

constexpr float MIN_SPEED = 0.5f;
constexpr float MAX_SPEED = 2.0f;

void appendUint32(std::string& out, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xFF));
    }
}

/// One binary frame: "C6PC" | streamId | chunkSeq | int16 LE samples.
std::string buildFrame(uint32_t streamId, uint32_t chunkSeq, const float* samples, std::size_t count) {
    std::string frame;
    frame.reserve(TTS_FRAME_HEADER_BYTES + count * 2);
    frame.append(TTS_FRAME_MAGIC, sizeof(TTS_FRAME_MAGIC));
    appendUint32(frame, streamId);
    appendUint32(frame, chunkSeq);
    for (std::size_t i = 0; i < count; ++i) {
        float clamped = std::clamp(samples[i], -1.0f, 1.0f);
        auto  pcm     = static_cast<int16_t>(clamped * 32767.0f);
        auto  bits    = static_cast<uint16_t>(pcm);
        frame.push_back(static_cast<char>(bits & 0xFF));
        frame.push_back(static_cast<char>(bits >> 8));
    }
    return frame;
}

} // anonymous namespace

TtsService::TtsService(TtsConfig config)
    : config_(std::move(config)) {}

TtsService::~TtsService() {
    stop();
}

void TtsService::start() {
    worker_ = std::jthread([this](std::stop_token stopToken) { run(stopToken); });
}

void TtsService::stop() {
    if (!worker_.joinable()) return;

    worker_.request_stop();
    wake_.notify_all();
    worker_.join();
}

// ─── Requests (any thread) ───────────────────────────────────────────────────

VoicesResult TtsService::listVoices(const std::string& requestId) const {
    VoicesResult result;
    result.requestId       = requestId;
    result.defaultVoice    = config_.defaultVoice;
    result.voices          = VoiceLibrary::scan(config_.voicesDir);
    result.engineAvailable = KokoroEngine::missingFiles(config_.engineDir).empty();
    result.engineLoaded    = engineLoaded_.load();
    if (!result.engineAvailable) {
        result.status = "error";
        result.error  = "Kokoro engine files are missing in " + config_.engineDir.string() +
                        " (see assistant/voice-engine/README.md)";
    }
    return result;
}

void TtsService::speak(SpeakRequest request, Sink sink) {
    auto reject = [&](const std::string& reason) {
        Logger::instance().warn("TTS", "Rejected TTS_SPEAK '{}': {}", request.requestId, reason);
        sink.sendText(SpeakError{request.requestId, reason}.toJson().dump());
    };

    if (request.text.find_first_not_of(" \t\r\n") == std::string::npos) {
        reject("text is empty");
        return;
    }
    if (request.text.size() > config_.maxTextChars) {
        reject("text is " + std::to_string(request.text.size()) + " bytes; the limit is " +
               std::to_string(config_.maxTextChars) + " (assistant.tts.max_text_chars)");
        return;
    }
    if (!(request.speed > 0.0f)) {
        reject("speed must be greater than 0");
        return;
    }
    request.speed = std::clamp(request.speed, MIN_SPEED, MAX_SPEED);
    if (request.voice.empty()) {
        request.voice = config_.defaultVoice;
    }

    {
        std::lock_guard lock(mutex_);
        queue_.push_back(Job{std::move(request), std::move(sink),
                             std::make_shared<std::atomic<bool>>(false)});
    }
    wake_.notify_one();
}

void TtsService::preload(std::string requestId, Sink sink) {
    {
        std::lock_guard lock(mutex_);
        Job job{SpeakRequest{}, std::move(sink), std::make_shared<std::atomic<bool>>(false), true};
        job.request.requestId = std::move(requestId);
        queue_.push_back(std::move(job));
    }
    wake_.notify_one();
}

void TtsService::cancel(const void* owner, const std::string& requestId) {
    std::lock_guard lock(mutex_);
    bool found = false;
    for (auto& job : queue_) {
        if (job.sink.owner == owner && job.request.requestId == requestId) {
            job.cancelled->store(true);
            found = true;
        }
    }
    if (active_ && active_->sink.owner == owner && active_->request.requestId == requestId) {
        active_->cancelled->store(true);
        found = true;
    }
    Logger::instance().info("TTS", "TTS_STOP '{}': {}", requestId,
                            found ? "cancelling" : "no queued or active request with this id");
}

// ─── Worker ──────────────────────────────────────────────────────────────────

void TtsService::run(std::stop_token stopToken) {
    auto& log = Logger::instance();

    if (config_.preload) {
        std::string error;
        if (!ensureEngine(VoiceLibrary::scan(config_.voicesDir), error)) {
            log.error("TTS", "Preload failed: {}", error);
        }
    }

    while (!stopToken.stop_requested()) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock lock(mutex_);
            if (!wake_.wait(lock, stopToken, [this] { return !queue_.empty(); }))
                break;  // stop requested
            job = std::make_shared<Job>(std::move(queue_.front()));
            queue_.pop_front();
            active_ = job;
        }

        try {
            if (job->preloadOnly) {
                processPreload(*job);
            } else {
                process(*job, stopToken);
            }
        } catch (const std::exception& e) {
            log.error("TTS", "Request '{}' failed: {}", job->request.requestId, e.what());
            job->sink.sendText(SpeakError{job->request.requestId,
                                          std::string("internal error: ") + e.what()}.toJson().dump());
        }

        std::lock_guard lock(mutex_);
        active_.reset();
    }

    engine_.reset();
    engineLoaded_ = false;
}

void TtsService::processPreload(Job& job) {
    PreloadResult result;
    result.requestId = job.request.requestId;

    const KokoroEngine* before = engine_.get();
    auto t0 = std::chrono::steady_clock::now();
    std::string error;
    if (!ensureEngine(VoiceLibrary::scan(config_.voicesDir), error)) {
        result.status = "error";
        result.error  = error;
        Logger::instance().warn("TTS", "TTS_PRELOAD '{}' failed: {}", result.requestId, error);
    } else {
        result.alreadyLoaded = before != nullptr && engine_.get() == before;
        result.loadSeconds   = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        Logger::instance().info("TTS", "TTS_PRELOAD '{}': {}", result.requestId,
                                result.alreadyLoaded ? "engine already loaded" : "engine loaded");
    }

    if (job.sink.isConnected()) {
        job.sink.sendText(result.toJson().dump());
    }
}

void TtsService::process(Job& job, const std::stop_token& stopToken) {
    auto& log = Logger::instance();
    const auto& req = job.request;

    if (!job.sink.isConnected()) {
        log.debug("TTS", "Dropping '{}': client disconnected before synthesis", req.requestId);
        return;
    }
    if (job.cancelled->load()) {
        SpeakFinished cancelled;
        cancelled.requestId = req.requestId;
        cancelled.status    = "cancelled";
        job.sink.sendText(cancelled.toJson().dump());
        return;
    }

    auto voices = VoiceLibrary::scan(config_.voicesDir);
    auto voiceIt = std::find_if(voices.begin(), voices.end(),
                                [&](const VoiceInfo& v) { return v.id == req.voice; });
    if (voiceIt == voices.end()) {
        std::string available;
        for (const auto& v : voices) available += (available.empty() ? "" : ", ") + v.id;
        job.sink.sendText(SpeakError{req.requestId, "voice '" + req.voice + "' not found in " +
                          config_.voicesDir.string() + "; available: " +
                          (available.empty() ? "none" : available)}.toJson().dump());
        return;
    }

    std::string error;
    if (!ensureEngine(voices, error)) {
        job.sink.sendText(SpeakError{req.requestId, error}.toJson().dump());
        return;
    }

    auto sid = speakerIds_.find(req.voice);
    if (sid == speakerIds_.end()) {
        job.sink.sendText(SpeakError{req.requestId, "voice '" + req.voice +
                          "' did not fit in the model's speaker slots; remove unused voices"}.toJson().dump());
        return;
    }

    SpeakStarted started;
    started.requestId  = req.requestId;
    started.streamId   = nextStreamId_.fetch_add(1);
    started.voice      = req.voice;
    started.sampleRate = engine_->sampleRate();
    job.sink.sendText(started.toJson().dump());

    log.info("TTS", "Speaking '{}' (stream {}, voice {}, {} chars, speed {:.2f})",
             req.requestId, started.streamId, req.voice, req.text.size(), req.speed);

    uint32_t chunkSeq     = 0;
    uint64_t totalSamples = 0;
    const std::size_t chunkSamples = std::max<uint32_t>(config_.chunkSamples, 1);

    auto onChunk = [&](const float* samples, int32_t count) -> bool {
        if (job.cancelled->load() || stopToken.stop_requested() || !job.sink.isConnected())
            return false;

        for (std::size_t offset = 0; offset < static_cast<std::size_t>(count); offset += chunkSamples) {
            auto n = std::min(chunkSamples, static_cast<std::size_t>(count) - offset);
            job.sink.sendBinary(buildFrame(started.streamId, chunkSeq++, samples + offset, n));
        }
        totalSamples += static_cast<uint64_t>(count);
        return true;
    };

    // "..." marks an exact pause (fillers: "Ah... Hill Climb 1."). Within each segment, long sentences are
    // cut at commas so their first words play while the rest is synthesised.
    const auto segments = splitAtPauses(req.text);
    const std::vector<float> pause(static_cast<std::size_t>(engine_->sampleRate()) * config_.pauseMs / 1000, 0.0f);
    bool completed = true;

    auto t0 = std::chrono::steady_clock::now();
    for (std::size_t s = 0; s < segments.size() && completed; ++s) {
        if (s > 0 && !pause.empty() && !onChunk(pause.data(), static_cast<int32_t>(pause.size()))) {
            completed = false;
            break;
        }
        for (const auto& piece : splitForSpeech(segments[s])) {
            auto outcome = engine_->generate(piece, sid->second, req.speed, voiceIt->language, onChunk);
            if (!isOk(outcome)) {
                job.sink.sendText(SpeakError{req.requestId, getError(outcome).message}.toJson().dump());
                return;
            }
            if (!getValue(outcome)) {
                completed = false;
                break;
            }
        }
    }
    auto generationSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    SpeakFinished finished;
    finished.requestId         = req.requestId;
    finished.status            = completed ? "success" : "cancelled";
    finished.streamId          = started.streamId;
    finished.chunks            = chunkSeq;
    finished.durationSeconds   = static_cast<double>(totalSamples) / engine_->sampleRate();
    finished.generationSeconds = generationSeconds;

    if (job.sink.isConnected()) {
        job.sink.sendText(finished.toJson().dump());
    }
    log.info("TTS", "Finished '{}' ({}): {:.2f}s audio in {:.2f}s",
             req.requestId, finished.status, finished.durationSeconds, generationSeconds);
}

bool TtsService::ensureEngine(const std::vector<VoiceInfo>& voices, std::string& error) {
    auto& log = Logger::instance();

    auto signature = VoiceLibrary::signature(voices);
    if (engine_ && signature == loadedSignature_)
        return true;

    if (engine_) {
        log.info("TTS", "Voices folder changed; reloading the engine");
    }
    engine_.reset();
    engineLoaded_ = false;

    if (voices.empty()) {
        error = "no voices in " + config_.voicesDir.string() + " (see assistant/voices/README.md)";
        return false;
    }

    auto slots = VoiceLibrary::slotCount(config_.engineDir);
    if (!isOk(slots)) {
        error = getError(slots).message;
        return false;
    }

    auto tablePath = config_.cacheDir / "voices.slots.bin";
    auto ids = VoiceLibrary::writeSlotTable(voices, getValue(slots), tablePath);
    if (!isOk(ids)) {
        error = getError(ids).message;
        return false;
    }

    auto t0 = std::chrono::steady_clock::now();
    auto engine = KokoroEngine::load(config_, tablePath);
    if (!isOk(engine)) {
        error = getError(engine).message;
        return false;
    }
    auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    engine_          = std::move(std::get<std::unique_ptr<KokoroEngine>>(engine));
    speakerIds_      = getValue(ids);
    loadedSignature_ = signature;
    engineLoaded_    = true;

    log.info("TTS", "Kokoro engine loaded in {:.2f}s ({} voices, {} slots, {} threads, {} Hz)",
             seconds, speakerIds_.size(), getValue(slots), config_.numThreads, engine_->sampleRate());
    return true;
}

} // namespace ifs
