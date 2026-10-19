#include "tts/VoiceLibrary.hpp"
#include "core/Logger.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>

namespace fs = std::filesystem;

namespace ifs {

namespace {

std::string genderFor(const std::string& voiceId) {
    if (voiceId.size() >= 2 && voiceId[1] == 'f') return "female";
    if (voiceId.size() >= 2 && voiceId[1] == 'm') return "male";
    return "unknown";
}

std::vector<char> readVoice(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(file),
                             std::istreambuf_iterator<char>());
}

} // anonymous namespace

std::string VoiceLibrary::languageFor(const std::string& voiceId) {
    // Kokoro's own naming convention: <language letter><gender letter>_<name>.
    // Values are espeak-ng voice names (espeak-ng-data/lang/...); British English is "en".
    switch (voiceId.empty() ? '\0' : voiceId[0]) {
        case 'a': return "en-us";
        case 'b': return "en";
        case 'e': return "es";
        case 'f': return "fr";
        case 'h': return "hi";
        case 'i': return "it";
        case 'j': return "ja";
        case 'p': return "pt-br";
        case 'z': return "en-us";  // Chinese text uses the zh lexicon; embedded Latin text uses en-us
        default:
            Logger::instance().warn("TTS", "Voice '{}' has no known language prefix; using en-us",
                                    voiceId);
            return "en-us";
    }
}

std::vector<VoiceInfo> VoiceLibrary::scan(const fs::path& dir) {
    auto& log = Logger::instance();
    std::vector<VoiceInfo> voices;

    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        log.warn("TTS", "Voices folder not found: {}", dir.string());
        return voices;
    }

    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".bin")
            continue;

        auto size = entry.file_size(ec);
        if (ec || size != VOICE_FILE_BYTES) {
            log.warn("TTS", "Skipping voice '{}': expected {} bytes (float32 510x1x256), found {}",
                     entry.path().filename().string(), VOICE_FILE_BYTES, ec ? 0 : size);
            continue;
        }

        VoiceInfo info;
        info.id       = entry.path().stem().string();
        info.language = languageFor(info.id);
        info.gender   = genderFor(info.id);
        info.path     = entry.path();
        voices.push_back(std::move(info));
    }
    if (ec) {
        log.error("TTS", "Failed to read voices folder {}: {}", dir.string(), ec.message());
    }

    std::sort(voices.begin(), voices.end(),
              [](const VoiceInfo& a, const VoiceInfo& b) { return a.id < b.id; });
    return voices;
}

std::string VoiceLibrary::signature(const std::vector<VoiceInfo>& voices) {
    std::string sig;
    std::error_code ec;
    for (const auto& v : voices) {
        auto stamp = fs::last_write_time(v.path, ec).time_since_epoch().count();
        sig += v.id + ':' + std::to_string(ec ? 0 : stamp) + ';';
    }
    return sig;
}

Result<std::size_t> VoiceLibrary::slotCount(const fs::path& engineDir) {
    auto reference = engineDir / "voices.bin";
    std::error_code ec;
    auto size = fs::file_size(reference, ec);
    if (ec) {
        return Error(ErrorCode::ResourceNotFound,
                     "Engine voices.bin not found (needed for the speaker slot count): " + reference.string());
    }
    if (size == 0 || size % VOICE_FILE_BYTES != 0) {
        return Error(ErrorCode::InvalidConfig,
                     "Engine voices.bin size " + std::to_string(size) +
                     " is not a multiple of one voice (" + std::to_string(VOICE_FILE_BYTES) + " bytes)");
    }
    return static_cast<std::size_t>(size / VOICE_FILE_BYTES);
}

Result<std::unordered_map<std::string, int32_t>> VoiceLibrary::writeSlotTable(
    const std::vector<VoiceInfo>& voices,
    std::size_t slots,
    const fs::path& outPath)
{
    if (voices.empty()) {
        return Error(ErrorCode::ResourceNotFound, "No voices to load");
    }
    if (voices.size() > slots) {
        Logger::instance().warn("TTS", "{} voices found but the model has {} slots; loading the first {} by name",
                                voices.size(), slots, slots);
    }

    std::error_code ec;
    fs::create_directories(outPath.parent_path(), ec);
    if (ec) {
        return Error(ErrorCode::InternalError,
                     "Cannot create cache folder " + outPath.parent_path().string() + ": " + ec.message());
    }

    auto tmpPath = outPath;
    tmpPath += ".tmp";

    std::unordered_map<std::string, int32_t> speakerIds;
    {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            return Error(ErrorCode::InternalError, "Cannot write " + tmpPath.string());
        }

        std::vector<char> padding;
        for (std::size_t slot = 0; slot < slots; ++slot) {
            if (slot < voices.size()) {
                auto data = readVoice(voices[slot].path);
                if (data.size() != VOICE_FILE_BYTES) {
                    return Error(ErrorCode::InternalError, "Voice changed while reading: " + voices[slot].path.string());
                }
                if (slot == 0) padding = data;
                out.write(data.data(), static_cast<std::streamsize>(data.size()));
                speakerIds[voices[slot].id] = static_cast<int32_t>(slot);
            } else {
                out.write(padding.data(), static_cast<std::streamsize>(padding.size()));
            }
        }
        if (!out) {
            return Error(ErrorCode::InternalError, "Failed writing " + tmpPath.string());
        }
    }

    fs::rename(tmpPath, outPath, ec);
    if (ec) {
        return Error(ErrorCode::InternalError,
                     "Cannot replace " + outPath.string() + ": " + ec.message());
    }
    return speakerIds;
}

} // namespace ifs
