#pragma once

#include <atomic>
#include <filesystem>
#include <string>

namespace ifs {

struct VideoTranscodeResult {
    bool ok = false;
    std::string error;
    bool hasAudio = false;    // false: the source has no sound, no audio file was written
    double duration = 0.0;    // seconds
};

/// The sound of a video for the sound system (the streaming player's track; the picture is streamed in
/// pieces by VideoStreamer): an Ogg audio track (Opus 48 kHz stereo; Opus/Vorbis sources are copied as they
/// are), played through the cabin speakers. Written as "<name>.part" and renamed when complete, so a
/// cancelled or failed run never leaves a playable-looking half file in the cache.
class VideoTranscoder {
public:
    /// hasAudio false (and no file) when the video has no sound. `cancel` stops it at the next packet.
    static VideoTranscodeResult transcodeAudio(const std::filesystem::path& src,
                                               const std::filesystem::path& audioOut,
                                               const std::atomic<bool>* cancel);
};

} // namespace ifs
