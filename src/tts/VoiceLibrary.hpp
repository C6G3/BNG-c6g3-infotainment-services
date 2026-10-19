#pragma once

#include "core/Error.hpp"
#include "tts/TtsTypes.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace ifs {

/// Kokoro voice files: discovery in the voices folder and the slot table the engine loads.
///
/// A Kokoro voice is a raw float32 style tensor of shape (510, 1, 256) with no header.
/// The engine expects one file holding exactly as many voices as the model has speaker
/// slots (the model's n_speakers). Its own voices.bin has that many, so its size gives
/// the slot count. We fill the slots from the voices folder and pad unused slots with
/// the first voice; the padding is never addressed.
///
/// Stateless; safe to call from any thread.
class VoiceLibrary {
public:
    static constexpr std::size_t VOICE_FILE_BYTES = 510 * 1 * 256 * sizeof(float);

    /// All valid voices in `dir`, sorted by id. Wrong-sized files are skipped with a warning.
    static std::vector<VoiceInfo> scan(const std::filesystem::path& dir);

    /// Identifies a voice set (ids, sizes, timestamps) so changes on disk can be detected.
    static std::string signature(const std::vector<VoiceInfo>& voices);

    /// Number of speaker slots in an engine package, from the size of its voices.bin.
    static Result<std::size_t> slotCount(const std::filesystem::path& engineDir);

    /// Writes the slot table for `voices` to `outPath` and returns voice id -> speaker id.
    /// Voices beyond `slots` are left out with a warning.
    static Result<std::unordered_map<std::string, int32_t>> writeSlotTable(
        const std::vector<VoiceInfo>& voices,
        std::size_t slots,
        const std::filesystem::path& outPath);

    /// espeak-ng language for a Kokoro voice id, from its first letter
    /// (a = American English "en-us", b = British English "en", e = Spanish "es", ...).
    static std::string languageFor(const std::string& voiceId);
};

} // namespace ifs
