#pragma once

#include "resources/Resource.hpp"
#include <filesystem>
#include <set>
#include <string>

namespace ifs {

/// Scanner that recursively scans directories, resolves shortcuts,
/// and extracts metadata and embedded thumbnails from media files using FFmpeg.
class MediaScanner {
public:
    MediaScanner() = default;

    /// Executes media scan synchronously. Called on worker thread.
    ScanMediaResult scan(const ScanMediaRequest& request);

private:
    void scanDirectory(const std::filesystem::path& dir,
                       const ScanMediaRequest& request,
                       std::set<std::string>& extensionsSet,
                       std::set<std::filesystem::path>& visitedDirs,
                       std::vector<AudioTrackInfo>& outTracks);

    void processFile(const std::filesystem::path& filePath,
                     const ScanMediaRequest& request,
                     std::vector<AudioTrackInfo>& outTracks);

    bool extractMetadataAndCover(const std::filesystem::path& filePath,
                                 const std::filesystem::path& thumbnailDir,
                                 AudioTrackInfo& outTrack);
};

} // namespace ifs
