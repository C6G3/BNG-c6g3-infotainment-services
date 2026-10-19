#pragma once

#include "core/Error.hpp"
#include "tts/TtsTypes.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct SherpaOnnxOfflineTts;

namespace ifs {

/// RAII wrapper around one sherpa-onnx Kokoro TTS engine (sherpa-onnx-c-api.dll).
///
/// Threading: not thread-safe. TtsService owns it and calls it from its worker only.
class KokoroEngine {
public:
    /// Called with each sentence's samples as soon as it is synthesised.
    /// The pointer is valid only during the call. Return false to stop generation.
    using ChunkCallback = std::function<bool(const float* samples, int32_t count)>;

    /// Outcome of generate(): true = finished, false = stopped by the callback.
    using GenerateResult = Result<bool>;

    /// Files in `engineDir` that must exist for load() to succeed. Empty when complete.
    static std::vector<std::string> missingFiles(const std::filesystem::path& engineDir);

    /// Loads the model on the CPU with `voiceTable` as its speaker slots. Takes a few seconds.
    static Result<std::unique_ptr<KokoroEngine>> load(const TtsConfig& config,
                                                      const std::filesystem::path& voiceTable);

    ~KokoroEngine();
    KokoroEngine(const KokoroEngine&)            = delete;
    KokoroEngine& operator=(const KokoroEngine&) = delete;

    int32_t sampleRate() const { return sampleRate_; }

    /// Synthesises `text` sentence by sentence. `language` is an espeak-ng voice (e.g. "en-us", "en").
    GenerateResult generate(const std::string& text, int32_t speakerId, float speed,
                            const std::string& language, const ChunkCallback& onChunk);

private:
    explicit KokoroEngine(const SherpaOnnxOfflineTts* handle);

    const SherpaOnnxOfflineTts* handle_     = nullptr;
    int32_t                     sampleRate_ = 24000;
};

} // namespace ifs
