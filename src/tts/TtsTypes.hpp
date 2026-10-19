#pragma once

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ifs {

/// Text-to-speech settings from the "assistant" section of the config.
/// All paths are absolute once Config::load() has resolved them.
struct TtsConfig {
    std::filesystem::path engineDir;      // Kokoro package: model.onnx, voices.bin, tokens.txt, espeak-ng-data/, lexicons
    std::filesystem::path voicesDir;      // one <voice>.bin per voice
    std::filesystem::path cacheDir;       // runtime files built by the service (voice slot table)
    std::string           defaultVoice   = "af_heart";
    int32_t               numThreads     = 3;      // CPU threads for synthesis (shared with BeamNG)
    uint32_t              maxTextChars   = 2000;
    uint32_t              chunkSamples   = 12000;  // samples per binary frame (0.5 s at 24 kHz)
    bool                  preload        = false;  // load the engine at startup instead of on first TTS_SPEAK
    uint32_t              pauseMs        = 200;    // silence for a "..." pause mark (plus ~180 ms the voice leaves itself)
};

/// One voice file found in the voices folder.
struct VoiceInfo {
    std::string           id;        // file stem, e.g. "af_heart"
    std::string           language;  // espeak-ng language used for phonemes, e.g. "en-us"
    std::string           gender;    // "female", "male" or "unknown"
    std::filesystem::path path;

    nlohmann::json toJson() const {
        return {
            {"id",       id},
            {"language", language},
            {"gender",   gender}
        };
    }
};

/// TTS_SPEAK request payload.
struct SpeakRequest {
    std::string requestId;
    std::string text;
    std::string voice;          // empty = configured default voice
    float       speed = 1.0f;   // > 1 is faster
};

/// Audio format of every TTS stream: mono signed 16-bit little-endian PCM.
inline constexpr int32_t     TTS_CHANNELS      = 1;
inline constexpr const char* TTS_SAMPLE_FORMAT = "pcm_s16le";

/// Binary audio frame layout: "C6PC" | uint32 LE streamId | uint32 LE chunkSeq | int16 LE samples.
inline constexpr char        TTS_FRAME_MAGIC[4]  = {'C', '6', 'P', 'C'};
inline constexpr std::size_t TTS_FRAME_HEADER_BYTES = 12;

/// TTS_VOICES response.
struct VoicesResult {
    std::string            requestId;
    std::string            status = "success";
    std::string            error;
    bool                   engineAvailable = false;  // model files present on disk
    bool                   engineLoaded    = false;  // model currently in memory
    std::string            defaultVoice;
    int32_t                sampleRate = 24000;
    std::vector<VoiceInfo> voices;

    nlohmann::json toJson() const {
        nlohmann::json res = {
            {"event",     "TTS_VOICES"},
            {"requestId", requestId},
            {"status",    status}
        };
        if (!error.empty()) {
            res["error"] = error;
        }
        nlohmann::json voiceArray = nlohmann::json::array();
        for (const auto& v : voices) {
            voiceArray.push_back(v.toJson());
        }
        res["data"] = {
            {"engineAvailable", engineAvailable},
            {"engineLoaded",    engineLoaded},
            {"defaultVoice",    defaultVoice},
            {"sampleRate",      sampleRate},
            {"voices",          voiceArray}
        };
        return res;
    }
};

/// TTS_STARTED event, sent before the first audio frame of a stream.
struct SpeakStarted {
    std::string requestId;
    uint32_t    streamId   = 0;
    std::string voice;
    int32_t     sampleRate = 24000;

    nlohmann::json toJson() const {
        return {
            {"event",     "TTS_STARTED"},
            {"requestId", requestId},
            {"status",    "success"},
            {"data", {
                {"streamId",   streamId},
                {"voice",      voice},
                {"sampleRate", sampleRate},
                {"channels",   TTS_CHANNELS},
                {"format",     TTS_SAMPLE_FORMAT}
            }}
        };
    }
};

/// TTS_PRELOADED response: the engine is in memory (or failed to load).
struct PreloadResult {
    std::string requestId;
    std::string status = "success";
    std::string error;
    bool        alreadyLoaded = false;  // nothing to do: the engine was loaded before this request
    double      loadSeconds   = 0.0;

    nlohmann::json toJson() const {
        nlohmann::json res = {
            {"event",     "TTS_PRELOADED"},
            {"requestId", requestId},
            {"status",    status},
            {"data", {
                {"engineLoaded",  status == "success"},
                {"alreadyLoaded", alreadyLoaded},
                {"loadSeconds",   std::round(loadSeconds * 1000.0) / 1000.0}
            }}
        };
        if (!error.empty()) {
            res["error"] = error;
        }
        return res;
    }
};

/// TTS_FINISHED event, sent after the last audio frame (status "success" or "cancelled").
struct SpeakFinished {
    std::string requestId;
    std::string status = "success";
    uint32_t    streamId          = 0;
    uint32_t    chunks            = 0;
    double      durationSeconds   = 0.0;  // audio produced
    double      generationSeconds = 0.0;  // wall time spent synthesising

    nlohmann::json toJson() const {
        double rtf = durationSeconds > 0.0 ? generationSeconds / durationSeconds : 0.0;
        return {
            {"event",     "TTS_FINISHED"},
            {"requestId", requestId},
            {"status",    status},
            {"data", {
                {"streamId",          streamId},
                {"chunks",            chunks},
                {"durationSeconds",   std::round(durationSeconds * 1000.0) / 1000.0},
                {"generationSeconds", std::round(generationSeconds * 1000.0) / 1000.0},
                {"realTimeFactor",    std::round(rtf * 1000.0) / 1000.0}
            }}
        };
    }
};

/// TTS_ERROR event.
struct SpeakError {
    std::string requestId;
    std::string error;

    nlohmann::json toJson() const {
        return {
            {"event",     "TTS_ERROR"},
            {"requestId", requestId},
            {"status",    "error"},
            {"error",     error}
        };
    }
};

} // namespace ifs
