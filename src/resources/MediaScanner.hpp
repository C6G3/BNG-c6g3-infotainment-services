#pragma once

#include "resources/Resource.hpp"
#include <filesystem>
#include <set>
#include <string>

namespace ifs {

/// Scanner that recursively scans directories, resolves shortcuts,
/// extracts audio metadata/covers and image thumbnails.
class MediaScanner {
public:
    MediaScanner() = default;

    /// Executes media scan synchronously. Called on worker thread.
    ScanMediaResult scan(const ScanMediaRequest& request, ProgressCallback onProgress = nullptr);

private:
    struct FileEntry {
        std::filesystem::path path;
        ScanMediaRequest request;
    };

    void collectFiles(const std::filesystem::path& dir,
                      const ScanMediaRequest& request,
                      bool isImageMode,
                      std::set<std::string>& extensionsSet,
                      std::set<std::filesystem::path>& visitedDirs,
                      std::vector<FileEntry>& outFiles);

    void processAudioFile(const std::filesystem::path& filePath,
                          const ScanMediaRequest& request,
                          std::vector<AudioTrackInfo>& outTracks);

    void processImageFile(const std::filesystem::path& filePath,
                          const ScanMediaRequest& request,
                          std::vector<ImageItemInfo>& outItems);

    bool extractAudioMetadataAndCover(const std::filesystem::path& filePath,
                                      const std::filesystem::path& thumbnailDir,
                                      AudioTrackInfo& outTrack);

    bool extractAndGenerateImageThumbnail(const std::filesystem::path& filePath,
                                          const std::filesystem::path& thumbnailDir,
                                          int maxWidth,
                                          int maxHeight,
                                          ImageItemInfo& outItem);
};

} // namespace ifs
