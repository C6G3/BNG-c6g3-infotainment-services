#include "tts/KokoroEngine.hpp"
#include "core/Logger.hpp"

#include <sherpa-onnx/c-api/c-api.h>

#include <nlohmann/json.hpp>
#include <cstring>

namespace fs = std::filesystem;

namespace ifs {

namespace {

/// Kokoro v1.0 full-precision model (sherpa-onnx package kokoro-multi-lang-v1_0).
fs::path modelFile(const fs::path& engineDir) {
    return engineDir / "model.onnx";
}

/// The multi-language model requires a lexicon. We always pass a language, so non-Chinese
/// text goes through espeak-ng and only Chinese text uses the lexicon. lexicon-zh is enough
/// and loads far faster than the 6 MB English one, which is only a fallback.
std::string lexiconList(const fs::path& engineDir) {
    for (const char* name : {"lexicon-zh.txt", "lexicon-us-en.txt"}) {
        auto path = engineDir / name;
        if (fs::exists(path)) return path.string();
    }
    return {};
}

struct CallbackContext {
    const KokoroEngine::ChunkCallback* onChunk = nullptr;
    bool                               stopped = false;
};

int32_t onGeneratedAudio(const float* samples, int32_t count, float /*progress*/, void* arg) {
    auto* context = static_cast<CallbackContext*>(arg);
    if (context->stopped)
        return 0;
    if (count > 0 && !(*context->onChunk)(samples, count)) {
        context->stopped = true;
        return 0;
    }
    return 1;
}

} // anonymous namespace

std::vector<std::string> KokoroEngine::missingFiles(const fs::path& engineDir) {
    std::vector<std::string> missing;
    if (!fs::exists(modelFile(engineDir)))         missing.push_back("model.onnx");
    if (!fs::exists(engineDir / "tokens.txt"))     missing.push_back("tokens.txt");
    if (!fs::exists(engineDir / "voices.bin"))     missing.push_back("voices.bin");
    if (!fs::is_directory(engineDir / "espeak-ng-data")) missing.push_back("espeak-ng-data/");
    if (lexiconList(engineDir).empty())            missing.push_back("lexicon-us-en.txt or lexicon-zh.txt");
    return missing;
}

Result<std::unique_ptr<KokoroEngine>> KokoroEngine::load(const TtsConfig& config,
                                                         const fs::path& voiceTable) {
    auto missing = missingFiles(config.engineDir);
    if (!missing.empty()) {
        std::string list;
        for (const auto& m : missing) list += (list.empty() ? "" : ", ") + m;
        return Error(ErrorCode::ResourceNotFound,
                     "Kokoro engine files missing in " + config.engineDir.string() + ": " + list);
    }

    // sherpa copies these strings during creation; they only need to outlive the call.
    const std::string model   = modelFile(config.engineDir).string();
    const std::string voices  = voiceTable.string();
    const std::string tokens  = (config.engineDir / "tokens.txt").string();
    const std::string dataDir = (config.engineDir / "espeak-ng-data").string();
    const std::string lexicon = lexiconList(config.engineDir);

    SherpaOnnxOfflineTtsConfig ttsConfig;
    std::memset(&ttsConfig, 0, sizeof(ttsConfig));
    ttsConfig.model.kokoro.model        = model.c_str();
    ttsConfig.model.kokoro.voices       = voices.c_str();
    ttsConfig.model.kokoro.tokens       = tokens.c_str();
    ttsConfig.model.kokoro.data_dir     = dataDir.c_str();
    ttsConfig.model.kokoro.lexicon      = lexicon.c_str();
    ttsConfig.model.kokoro.length_scale = 1.0f;
    ttsConfig.model.num_threads         = config.numThreads;
    ttsConfig.model.provider            = "cpu";
    ttsConfig.model.debug               = 0;
    ttsConfig.max_num_sentences         = 1;  // one callback per sentence = lowest latency to first audio

    const SherpaOnnxOfflineTts* handle = SherpaOnnxCreateOfflineTts(&ttsConfig);
    if (!handle) {
        return Error(ErrorCode::InternalError,
                     "sherpa-onnx rejected the Kokoro configuration (see its console output): " +
                     config.engineDir.string());
    }

    return std::unique_ptr<KokoroEngine>(new KokoroEngine(handle));
}

KokoroEngine::KokoroEngine(const SherpaOnnxOfflineTts* handle)
    : handle_(handle)
    , sampleRate_(SherpaOnnxOfflineTtsSampleRate(handle)) {}

KokoroEngine::~KokoroEngine() {
    if (handle_) {
        SherpaOnnxDestroyOfflineTts(handle_);
    }
}

KokoroEngine::GenerateResult KokoroEngine::generate(const std::string& text, int32_t speakerId,
                                                    float speed, const std::string& language,
                                                    const ChunkCallback& onChunk) {
    const std::string extra = nlohmann::json{{"lang", language}}.dump();

    SherpaOnnxGenerationConfig genConfig;
    std::memset(&genConfig, 0, sizeof(genConfig));
    genConfig.sid           = speakerId;
    genConfig.speed         = speed;
    genConfig.silence_scale = 0.2f;  // sherpa's default pause between sentences
    genConfig.extra         = extra.c_str();

    CallbackContext context{&onChunk, false};
    const SherpaOnnxGeneratedAudio* audio = SherpaOnnxOfflineTtsGenerateWithConfig(
        handle_, text.c_str(), &genConfig, onGeneratedAudio, &context);

    if (audio) {
        SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
    } else if (!context.stopped) {
        return Error(ErrorCode::InternalError, "Kokoro produced no audio for this text");
    }
    return !context.stopped;
}

} // namespace ifs
