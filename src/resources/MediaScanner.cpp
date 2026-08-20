#include "resources/MediaScanner.hpp"
#include "platform/windows/ShortcutResolver.hpp"
#include "core/Logger.hpp"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

#include <fstream>
#include <algorithm>
#include <cctype>

namespace ifs {

namespace {

std::string toLower(std::string_view str) {
    std::string result;
    result.reserve(str.size());
    for (char c : str) {
        result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return result;
}

std::string normalizeExtension(std::string_view ext) {
    std::string lower = toLower(ext);
    if (!lower.empty() && lower.front() == '.') {
        lower.erase(0, 1);
    }
    return lower;
}

std::string cleanString(std::string str) {
    // Trim leading and trailing whitespace
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.front()))) str.erase(0, 1);
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.back()))) str.pop_back();
    return str;
}

void parseFilenameFallback(const std::string& stem, std::string& outArtist, std::string& outTitle) {
    // Check if filename is "Artist - Title"
    auto dashPos = stem.find(" - ");
    if (dashPos != std::string::npos) {
        outArtist = cleanString(stem.substr(0, dashPos));
        outTitle  = cleanString(stem.substr(dashPos + 3));
    } else {
        outArtist = "Unknown Artist";
        outTitle  = stem;
    }
}

std::string normalizePathSeparators(const std::filesystem::path& path) {
    std::string s = path.string();
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

} // anonymous namespace

ScanMediaResult MediaScanner::scan(const ScanMediaRequest& request) {
    ScanMediaResult result;
    result.requestId  = request.requestId;
    result.targetPath = request.targetPath;

    std::filesystem::path targetDir(request.targetPath);
    if (!std::filesystem::exists(targetDir)) {
        // Try resolving if targetPath is a .lnk
        if (ShortcutResolver::isShortcut(targetDir)) {
            if (auto resolved = ShortcutResolver::resolve(targetDir)) {
                targetDir = *resolved;
            }
        }
    }

    if (!std::filesystem::exists(targetDir)) {
        Logger::instance().warn("MediaScanner", "Target path does not exist: {}", request.targetPath);
        result.status = "error";
        result.error  = "Target path does not exist or is inaccessible";
        return result;
    }

    std::set<std::string> extensionsSet;
    for (const auto& ext : request.extensions) {
        extensionsSet.insert(normalizeExtension(ext));
    }

    std::set<std::filesystem::path> visitedDirs;
    std::vector<AudioTrackInfo> tracks;

    if (std::filesystem::is_directory(targetDir)) {
        scanDirectory(targetDir, request, extensionsSet, visitedDirs, tracks);
    } else if (std::filesystem::is_regular_file(targetDir)) {
        processFile(targetDir, request, tracks);
    }

    result.status = "success";
    result.tracks = std::move(tracks);
    Logger::instance().info("MediaScanner", "Scan complete for '{}': found {} track(s)",
                            request.targetPath, result.tracks.size());
    return result;
}

void MediaScanner::scanDirectory(const std::filesystem::path& dir,
                                const ScanMediaRequest& request,
                                std::set<std::string>& extensionsSet,
                                std::set<std::filesystem::path>& visitedDirs,
                                std::vector<AudioTrackInfo>& outTracks) {
    std::error_code ec;
    auto canonicalDir = std::filesystem::weakly_canonical(dir, ec);
    if (ec) canonicalDir = dir;

    if (visitedDirs.count(canonicalDir)) {
        return; // Avoid cyclic loops
    }
    visitedDirs.insert(canonicalDir);

    try {
        for (const auto& entry : std::filesystem::directory_iterator(dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (ec) continue;

            const auto& path = entry.path();

            if (ShortcutResolver::isShortcut(path)) {
                if (auto resolved = ShortcutResolver::resolve(path)) {
                    if (std::filesystem::is_directory(*resolved)) {
                        scanDirectory(*resolved, request, extensionsSet, visitedDirs, outTracks);
                    } else if (std::filesystem::is_regular_file(*resolved)) {
                        auto ext = normalizeExtension(resolved->extension().string());
                        if (extensionsSet.empty() || extensionsSet.count(ext)) {
                            processFile(*resolved, request, outTracks);
                        }
                    }
                }
                continue;
            }

            if (entry.is_directory(ec)) {
                scanDirectory(path, request, extensionsSet, visitedDirs, outTracks);
            } else if (entry.is_regular_file(ec)) {
                auto ext = normalizeExtension(path.extension().string());
                if (extensionsSet.empty() || extensionsSet.count(ext)) {
                    processFile(path, request, outTracks);
                }
            }
        }
    } catch (const std::exception& e) {
        Logger::instance().warn("MediaScanner", "Error scanning directory {}: {}", dir.string(), e.what());
    }
}

void MediaScanner::processFile(const std::filesystem::path& filePath,
                               const ScanMediaRequest& request,
                               std::vector<AudioTrackInfo>& outTracks) {
    AudioTrackInfo track;
    track.mediaType = "audio";
    track.path      = normalizePathSeparators(filePath);
    track.filename  = filePath.filename().string();

    std::filesystem::path thumbDir(request.outputThumbnailDir);
    extractMetadataAndCover(filePath, thumbDir, track);

    outTracks.push_back(std::move(track));
}

bool MediaScanner::extractMetadataAndCover(const std::filesystem::path& filePath,
                                           const std::filesystem::path& thumbnailDir,
                                           AudioTrackInfo& outTrack) {
    AVFormatContext* fmtCtx = nullptr;
    int ret = avformat_open_input(&fmtCtx, filePath.string().c_str(), nullptr, nullptr);
    if (ret < 0) {
        // Fallback to basic filename parsing if FFmpeg cannot open container
        std::string stem = filePath.stem().string();
        parseFilenameFallback(stem, outTrack.artist, outTrack.title);
        outTrack.album         = "";
        outTrack.category      = "music";
        outTrack.thumbnailFile = "";
        outTrack.cover         = "";
        return false;
    }

    avformat_find_stream_info(fmtCtx, nullptr);

    // Read metadata tags
    auto getTag = [fmtCtx](const char* key) -> std::string {
        AVDictionaryEntry* entry = av_dict_get(fmtCtx->metadata, key, nullptr, AV_DICT_IGNORE_SUFFIX);
        if (entry && entry->value) {
            return cleanString(entry->value);
        }
        return "";
    };

    std::string title  = getTag("title");
    std::string artist = getTag("artist");
    if (artist.empty()) artist = getTag("album_artist");
    if (artist.empty()) artist = getTag("performer");
    std::string album  = getTag("album");
    std::string genre  = getTag("genre");

    std::string stem = filePath.stem().string();
    if (title.empty() || artist.empty()) {
        std::string fallbackArtist, fallbackTitle;
        parseFilenameFallback(stem, fallbackArtist, fallbackTitle);
        if (title.empty())  title  = fallbackTitle;
        if (artist.empty()) artist = fallbackArtist;
    }

    outTrack.title    = title;
    outTrack.artist   = artist;
    outTrack.album    = album;
    outTrack.category = genre.empty() ? "music" : genre;

    // Search for embedded album art (attached pic)
    bool extractedCover = false;
    for (unsigned int i = 0; i < fmtCtx->nb_streams; ++i) {
        AVStream* st = fmtCtx->streams[i];
        if (st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
            const AVPacket& pkt = st->attached_pic;
            if (pkt.data && pkt.size > 0) {
                // Determine image format (JPEG vs PNG)
                std::string imgExt = ".jpg";
                if (pkt.size >= 4 &&
                    pkt.data[0] == 0x89 && pkt.data[1] == 'P' &&
                    pkt.data[2] == 'N'  && pkt.data[3] == 'G') {
                    imgExt = ".png";
                }

                // Per requirement: thumbnail uses exact base name of media file
                std::string thumbFileName = stem + imgExt;

                if (!thumbnailDir.empty()) {
                    std::error_code ec;
                    std::filesystem::create_directories(thumbnailDir, ec);

                    std::filesystem::path destPath = thumbnailDir / thumbFileName;
                    std::ofstream out(destPath, std::ios::binary);
                    if (out.is_open()) {
                        out.write(reinterpret_cast<const char*>(pkt.data), pkt.size);
                        out.close();

                        outTrack.thumbnailFile = thumbFileName;
                        outTrack.cover         = thumbFileName;
                        extractedCover         = true;
                    } else {
                        Logger::instance().warn("MediaScanner", "Failed to write thumbnail: {}", destPath.string());
                    }
                }
                break; // First attached pic found
            }
        }
    }

    if (!extractedCover) {
        outTrack.thumbnailFile = "";
        outTrack.cover         = "";
    }

    avformat_close_input(&fmtCtx);
    return true;
}

} // namespace ifs
