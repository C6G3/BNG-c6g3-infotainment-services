#include "resources/MediaScanner.hpp"
#include "platform/windows/ShortcutResolver.hpp"
#include "core/Logger.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb/stb_image_resize2.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/dict.h>
#include <libavutil/imgutils.h>
}

#include <fstream>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace ifs {

namespace {

std::string toUtf8(const std::filesystem::path& path) {
#if defined(_WIN32)
    const std::wstring& ws = path.native();
    if (ws.empty()) return "";
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (sizeNeeded <= 0) return "";
    std::string result(sizeNeeded, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), &result[0], sizeNeeded, nullptr, nullptr);
    return result;
#else
    auto u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
#endif
}

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
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.front()))) str.erase(0, 1);
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.back()))) str.pop_back();
    return str;
}

void parseFilenameFallback(const std::string& stem, std::string& outArtist, std::string& outTitle) {
    auto dashPos = stem.find(" - ");
    if (dashPos == std::string::npos) {
        dashPos = stem.find(" \xE2\x80\x93 "); // UTF-8 en dash (–)
        if (dashPos != std::string::npos) {
            outArtist = cleanString(stem.substr(0, dashPos));
            outTitle  = cleanString(stem.substr(dashPos + 5));
            return;
        }
        dashPos = stem.find(" \xE2\x80\x94 "); // UTF-8 em dash (—)
        if (dashPos != std::string::npos) {
            outArtist = cleanString(stem.substr(0, dashPos));
            outTitle  = cleanString(stem.substr(dashPos + 5));
            return;
        }
    }
    if (dashPos != std::string::npos) {
        outArtist = cleanString(stem.substr(0, dashPos));
        outTitle  = cleanString(stem.substr(dashPos + 3));
    } else {
        outArtist = "Unknown Artist";
        outTitle  = stem;
    }
}

std::string normalizePathSeparators(const std::filesystem::path& path) {
    std::string s = toUtf8(path);
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

std::string sanitizeThumbFilename(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (size_t i = 0; i < name.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(name[i]);
        // Handle UTF-8 em dash \xE2\x80\x94 and en dash \xE2\x80\x93
        if (c == 0xE2 && i + 2 < name.size()) {
            unsigned char c1 = static_cast<unsigned char>(name[i + 1]);
            unsigned char c2 = static_cast<unsigned char>(name[i + 2]);
            if (c1 == 0x80 && (c2 == 0x93 || c2 == 0x94)) {
                out.push_back('-');
                i += 2;
                continue;
            }
        }
        // Replace invalid Windows/VFS chars and any non-ASCII (> 126 or < 32) with '_'
        if (c < 32 || c > 126 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') {
            out.push_back('_');
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    while (!out.empty() && (out.front() == ' ' || out.front() == '.')) out.erase(0, 1);
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    return out.empty() ? "thumb" : out;
}

} // anonymous namespace

ScanMediaResult MediaScanner::scan(const ScanMediaRequest& request,
                                   ProgressCallback onProgress,
                                   const ProgressConfig& progressConfig) {
    ScanMediaResult result;
    result.requestId  = request.requestId;
    result.targetPath = request.targetPath;

    // Determine mode: image vs video vs audio
    bool isImageMode = false;
    bool isVideoMode = false;
    for (const auto& t : request.mediaTypes) {
        std::string lt = toLower(t);
        if (lt == "image" || lt == "images" || lt == "picture" || lt == "pictures" || lt == "wallpaper" || lt == "wallpapers") {
            isImageMode = true;
            break;
        }
        if (lt == "video" || lt == "videos") {
            isVideoMode = true;
            break;
        }
    }
    result.primaryMediaType = isImageMode ? "image" : (isVideoMode ? "video" : "audio");

    std::filesystem::path rawTarget(request.targetPath);
    ScanMediaRequest activeRequest = request;

    std::filesystem::path targetDir = rawTarget;
    if (ShortcutResolver::isShortcut(rawTarget)) {
        if (activeRequest.category.empty()) {
            activeRequest.category = rawTarget.stem().string();
        }
        if (auto resolved = ShortcutResolver::resolve(rawTarget)) {
            targetDir = *resolved;
        }
    }

    if (!std::filesystem::exists(targetDir)) {
        Logger::instance().warn("MediaScanner", "Target path does not exist: {}", request.targetPath);
        result.status = "error";
        result.error  = "Target path does not exist or is inaccessible";
        return result;
    }

    std::set<std::string> extensionsSet;
    for (const auto& ext : activeRequest.extensions) {
        extensionsSet.insert(normalizeExtension(ext));
    }

    std::set<std::filesystem::path> visitedDirs;
    std::vector<FileEntry> filesToProcess;

    if (std::filesystem::is_directory(targetDir)) {
        collectFiles(targetDir, activeRequest, isImageMode, extensionsSet, visitedDirs, filesToProcess);
    } else if (std::filesystem::is_regular_file(targetDir)) {
        auto fileExt = normalizeExtension(targetDir.extension().string());
        if (extensionsSet.empty() || extensionsSet.count(fileExt)) {
            static const std::set<std::string> kImageExts = {"png", "jpg", "jpeg", "webp", "bmp", "dds", "tga", "tiff", "gif"};

            if (!isImageMode && !isVideoMode && kImageExts.count(fileExt)) {
                isImageMode = true;
                result.primaryMediaType = "image";
            }

            filesToProcess.push_back({targetDir, activeRequest});
        }
    }

    const uint64_t totalCount = filesToProcess.size();
    const bool shouldReportProgress = onProgress && (totalCount >= progressConfig.minScanItems);

    // Report discovery start progress
    if (shouldReportProgress) {
        OperationProgress prog;
        prog.requestId = activeRequest.requestId;
        prog.action    = "SCANNING";
        prog.progress  = 0.0;
        prog.current   = 0;
        prog.total     = totalCount;
        prog.unit      = "items";
        prog.detail    = "Discovered " + std::to_string(totalCount) + " item(s)";
        onProgress(prog);
    }

    std::vector<AudioTrackInfo> tracks;
    std::vector<ImageItemInfo> items;
    std::vector<VideoItemInfo> videos;

    for (size_t i = 0; i < filesToProcess.size(); ++i) {
        const auto& entry = filesToProcess[i];
        if (isImageMode) {
            processImageFile(entry.path, entry.request, items);
        } else if (isVideoMode) {
            processVideoFile(entry.path, entry.request, videos);
        } else {
            processAudioFile(entry.path, entry.request, tracks);
        }

        if (shouldReportProgress && totalCount > 0) {
            uint64_t current = i + 1;
            double pct = (static_cast<double>(current) / totalCount) * 100.0;
            OperationProgress prog;
            prog.requestId = activeRequest.requestId;
            prog.action    = "SCANNING";
            prog.progress  = std::min(100.0, pct);
            prog.current   = current;
            prog.total     = totalCount;
            prog.unit      = "items";
            prog.detail    = toUtf8(entry.path.filename());
            onProgress(prog);
        }
    }

    result.status = "success";
    result.tracks = std::move(tracks);
    result.items  = std::move(items);
    result.videos = std::move(videos);

    size_t count = isImageMode ? result.items.size() : (isVideoMode ? result.videos.size() : result.tracks.size());

    Logger::instance().info("MediaScanner", "Scan complete for '{}' (mode={}): found {} item(s)",
                            request.targetPath, result.primaryMediaType, count);
    return result;
}

void MediaScanner::collectFiles(const std::filesystem::path& dir,
                                const ScanMediaRequest& request,
                                bool isImageMode,
                                std::set<std::string>& extensionsSet,
                                std::set<std::filesystem::path>& visitedDirs,
                                std::vector<FileEntry>& outFiles) {
    std::error_code ec;
    auto canonicalDir = std::filesystem::weakly_canonical(dir, ec);
    if (ec) canonicalDir = dir;

    if (visitedDirs.count(canonicalDir)) {
        return;
    }
    visitedDirs.insert(canonicalDir);

    Logger::instance().debug("MediaScanner", "Scanning directory: '{}'", toUtf8(dir));

    try {
        for (const auto& entry : std::filesystem::directory_iterator(dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (ec) continue;

            const auto& path = entry.path();

            if (ShortcutResolver::isShortcut(path)) {
                if (auto resolved = ShortcutResolver::resolve(path)) {
                    Logger::instance().debug("MediaScanner", "Resolved shortcut '{}' -> '{}'",
                                            toUtf8(path), toUtf8(*resolved));

                    std::string shortcutCategory = toUtf8(path.stem());
                    ScanMediaRequest subReq = request;
                    if (subReq.category.empty()) {
                        subReq.category = shortcutCategory;
                    }

                    if (std::filesystem::is_directory(*resolved)) {
                        collectFiles(*resolved, subReq, isImageMode, extensionsSet, visitedDirs, outFiles);
                    } else if (std::filesystem::is_regular_file(*resolved)) {
                        auto ext = normalizeExtension(toUtf8(resolved->extension()));
                        if (extensionsSet.empty() || extensionsSet.count(ext)) {
                            outFiles.push_back({*resolved, subReq});
                        }
                    }
                }
                continue;
            }

            if (entry.is_directory(ec)) {
                collectFiles(path, request, isImageMode, extensionsSet, visitedDirs, outFiles);
            } else if (entry.is_regular_file(ec)) {
                auto ext = normalizeExtension(toUtf8(path.extension()));
                if (extensionsSet.empty() || extensionsSet.count(ext)) {
                    outFiles.push_back({path, request});
                }
            }
        }
    } catch (const std::exception& e) {
        Logger::instance().warn("MediaScanner", "Error scanning directory {}: {}", toUtf8(dir), e.what());
    }
}

// ─── Audio Processing ────────────────────────────────────────────────────────

void MediaScanner::processAudioFile(const std::filesystem::path& filePath,
                                    const ScanMediaRequest& request,
                                    std::vector<AudioTrackInfo>& outTracks) {
    AudioTrackInfo track;
    track.mediaType = "audio";
    track.path      = normalizePathSeparators(filePath);
    track.filename  = toUtf8(filePath.filename());

    std::filesystem::path thumbDir(request.outputThumbnailDir);
    extractAudioMetadataAndCover(filePath, thumbDir, track);

    if (!request.category.empty()) {
        track.category = request.category;
    }

    Logger::instance().debug("MediaScanner", "[Audio] Scanned '{}' (artist='{}', title='{}', album='{}', cover='{}', category='{}')",
                            track.filename, track.artist, track.title, track.album,
                            track.thumbnailFile.empty() ? "none" : track.thumbnailFile, track.category);

    outTracks.push_back(std::move(track));
}

bool MediaScanner::extractAudioMetadataAndCover(const std::filesystem::path& filePath,
                                                const std::filesystem::path& thumbnailDir,
                                                AudioTrackInfo& outTrack) {
    AVFormatContext* fmtCtx = nullptr;
    std::string utf8FilePath = toUtf8(filePath);
    int ret = avformat_open_input(&fmtCtx, utf8FilePath.c_str(), nullptr, nullptr);
    if (ret < 0) {
        std::string stem = toUtf8(filePath.stem());
        parseFilenameFallback(stem, outTrack.artist, outTrack.title);
        outTrack.album         = "";
        outTrack.category      = "music";
        outTrack.thumbnailFile = "";
        outTrack.cover         = "";
        return false;
    }

    avformat_find_stream_info(fmtCtx, nullptr);

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

    std::string stem = toUtf8(filePath.stem());
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

    bool extractedCover = false;
    for (unsigned int i = 0; i < fmtCtx->nb_streams; ++i) {
        AVStream* st = fmtCtx->streams[i];
        if (st->disposition & AV_DISPOSITION_ATTACHED_PIC) {
            const AVPacket& pkt = st->attached_pic;
            if (pkt.data && pkt.size > 0) {
                std::string imgExt = ".jpg";
                if (pkt.size >= 4 &&
                    pkt.data[0] == 0x89 && pkt.data[1] == 'P' &&
                    pkt.data[2] == 'N'  && pkt.data[3] == 'G') {
                    imgExt = ".png";
                }

                std::string thumbFileName = sanitizeThumbFilename(stem) + imgExt;

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
                break;
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

// ─── Image Processing & Thumbnail Generation ─────────────────────────────────

void MediaScanner::processImageFile(const std::filesystem::path& filePath,
                                    const ScanMediaRequest& request,
                                    std::vector<ImageItemInfo>& outItems) {
    ImageItemInfo item;
    item.mediaType = "image";
    item.filename  = toUtf8(filePath.filename());
    item.title     = toUtf8(filePath.stem());
    item.path      = normalizePathSeparators(filePath);
    item.format    = normalizeExtension(toUtf8(filePath.extension()));

    // Category: always the immediate folder of the specific file (e.g. "Captures", "pen", "Wallpapers")
    std::string folderName = toUtf8(filePath.parent_path().filename());
    if (!folderName.empty()) {
        item.category = folderName;
    } else if (!request.category.empty()) {
        item.category = request.category;
    } else {
        item.category = "Images";
    }

    std::filesystem::path thumbDir(request.outputThumbnailDir);
    int maxW = request.maxThumbnailWidth > 0 ? request.maxThumbnailWidth : 256;
    int maxH = request.maxThumbnailHeight > 0 ? request.maxThumbnailHeight : 256;

    extractAndGenerateImageThumbnail(filePath, thumbDir, maxW, maxH, item);

    Logger::instance().debug("MediaScanner", "[Image] Scanned '{}' (dimensions: {}x{}, format='{}', thumb='{}', category='{}')",
                            item.filename, item.width, item.height, item.format,
                            item.thumbnailFile.empty() ? "none" : item.thumbnailFile, item.category);

    outItems.push_back(std::move(item));
}

bool MediaScanner::extractAndGenerateImageThumbnail(const std::filesystem::path& filePath,
                                                    const std::filesystem::path& thumbnailDir,
                                                    int maxWidth,
                                                    int maxHeight,
                                                    ImageItemInfo& outItem) {
    std::string stem = toUtf8(filePath.stem());
    std::string ext  = outItem.format;
    std::string thumbExt = (ext == "jpg" || ext == "jpeg") ? ".jpg" : ".png";
    std::string thumbFileName = sanitizeThumbFilename(stem) + "_thumb" + thumbExt;

    // 1. Try stb_image from memory (supports full Unicode filesystem paths on Windows)
    int origW = 0, origH = 0, channels = 0;
    unsigned char* imgData = nullptr;

    std::ifstream imgFile(filePath, std::ios::binary | std::ios::ate);
    if (imgFile.is_open()) {
        std::streamsize fileSize = imgFile.tellg();
        if (fileSize > 0 && fileSize < (1024 * 1024 * 128)) {
            imgFile.seekg(0, std::ios::beg);
            std::vector<unsigned char> buffer(fileSize);
            if (imgFile.read(reinterpret_cast<char*>(buffer.data()), fileSize)) {
                imgData = stbi_load_from_memory(buffer.data(), static_cast<int>(fileSize), &origW, &origH, &channels, 0);
            }
        }
        imgFile.close();
    }

    if (imgData && origW > 0 && origH > 0 && channels > 0) {
        outItem.width  = origW;
        outItem.height = origH;

        // Calculate aspect-ratio preserving dimensions
        double scale = std::min(static_cast<double>(maxWidth) / origW,
                                static_cast<double>(maxHeight) / origH);
        if (scale > 1.0) scale = 1.0; // Do not upscale

        int targetW = std::max(1, static_cast<int>(std::floor(origW * scale)));
        int targetH = std::max(1, static_cast<int>(std::floor(origH * scale)));

        if (!thumbnailDir.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(thumbnailDir, ec);

            std::vector<unsigned char> resized(targetW * targetH * channels);
            stbir_pixel_layout layout = (channels == 4) ? STBIR_RGBA :
                                        (channels == 3) ? STBIR_RGB :
                                        (channels == 2) ? STBIR_2CHANNEL : STBIR_1CHANNEL;

            stbir_resize_uint8_linear(imgData, origW, origH, origW * channels,
                                      resized.data(), targetW, targetH, targetW * channels,
                                      layout);

            std::filesystem::path destThumb = thumbnailDir / thumbFileName;
            int writeSuccess = 0;
            if (thumbExt == ".jpg") {
                writeSuccess = stbi_write_jpg(destThumb.string().c_str(), targetW, targetH, channels, resized.data(), 85);
            } else {
                writeSuccess = stbi_write_png(destThumb.string().c_str(), targetW, targetH, channels, resized.data(), targetW * channels);
            }

            if (writeSuccess) {
                outItem.thumbnailFile = thumbFileName;
                Logger::instance().debug("MediaScanner", "[Image] Generated thumbnail for '{}' (orig: {}x{}, scaled: {}x{}) -> '{}'",
                                        toUtf8(filePath.filename()), origW, origH, targetW, targetH, toUtf8(destThumb));
            } else {
                Logger::instance().warn("MediaScanner", "Failed to write image thumbnail: {}", toUtf8(destThumb));
                outItem.thumbnailFile = "";
            }
        }

        stbi_image_free(imgData);
        return true;
    }

    // 2. Fallback to FFmpeg (for WebP, DDS, HEIC, AVIF, TIFF, etc.)
    AVFormatContext* fmtCtx = nullptr;
    std::string utf8FilePath = toUtf8(filePath);
    if (avformat_open_input(&fmtCtx, utf8FilePath.c_str(), nullptr, nullptr) >= 0) {
        if (avformat_find_stream_info(fmtCtx, nullptr) >= 0 && fmtCtx->nb_streams > 0) {
            int streamIdx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            if (streamIdx >= 0) {
                AVCodecParameters* codecpar = fmtCtx->streams[streamIdx]->codecpar;
                const AVCodec* decoder = avcodec_find_decoder(codecpar->codec_id);
                if (decoder) {
                    AVCodecContext* codecCtx = avcodec_alloc_context3(decoder);
                    if (codecCtx) {
                        avcodec_parameters_to_context(codecCtx, codecpar);
                        if (avcodec_open2(codecCtx, decoder, nullptr) >= 0) {
                            AVPacket* pkt   = av_packet_alloc();
                            AVFrame*  frame = av_frame_alloc();
                            bool frameDecoded = false;

                            while (av_read_frame(fmtCtx, pkt) >= 0 && !frameDecoded) {
                                if (pkt->stream_index == streamIdx) {
                                    if (avcodec_send_packet(codecCtx, pkt) >= 0) {
                                        if (avcodec_receive_frame(codecCtx, frame) >= 0) {
                                            outItem.width  = frame->width;
                                            outItem.height = frame->height;

                                            double scale = std::min(static_cast<double>(maxWidth) / frame->width,
                                                                    static_cast<double>(maxHeight) / frame->height);
                                            if (scale > 1.0) scale = 1.0;

                                            int targetW = std::max(1, static_cast<int>(std::floor(frame->width * scale)));
                                            int targetH = std::max(1, static_cast<int>(std::floor(frame->height * scale)));

                                            if (!thumbnailDir.empty()) {
                                                std::error_code ec;
                                                std::filesystem::create_directories(thumbnailDir, ec);

                                                SwsContext* swsCtx = sws_getContext(
                                                    frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                                                    targetW, targetH, AV_PIX_FMT_RGB24,
                                                    SWS_BILINEAR, nullptr, nullptr, nullptr);

                                                if (swsCtx) {
                                                    std::vector<uint8_t> rgbBuf(targetW * targetH * 3);
                                                    uint8_t* dstData[4] = { rgbBuf.data(), nullptr, nullptr, nullptr };
                                                    int dstLinesize[4] = { targetW * 3, 0, 0, 0 };

                                                    sws_scale(swsCtx, frame->data, frame->linesize, 0,
                                                              frame->height, dstData, dstLinesize);
                                                    sws_freeContext(swsCtx);

                                                    std::filesystem::path destThumb = thumbnailDir / thumbFileName;
                                                    if (stbi_write_png(destThumb.string().c_str(), targetW, targetH, 3, rgbBuf.data(), targetW * 3)) {
                                                        outItem.thumbnailFile = thumbFileName;
                                                        Logger::instance().debug("MediaScanner", "[Image] Generated thumbnail via FFmpeg for '{}' (orig: {}x{}, scaled: {}x{}) -> '{}'",
                                                                                toUtf8(filePath.filename()), frame->width, frame->height, targetW, targetH, toUtf8(destThumb));
                                                    }
                                                }
                                            }
                                            frameDecoded = true;
                                        }
                                    }
                                }
                                av_packet_unref(pkt);
                            }

                            av_frame_free(&frame);
                            av_packet_free(&pkt);
                        }
                        avcodec_free_context(&codecCtx);
                    }
                }
            }
        }
        avformat_close_input(&fmtCtx);
        if (!outItem.thumbnailFile.empty() || outItem.width > 0) {
            return true;
        }
    }

    Logger::instance().debug("MediaScanner", "[Image] Could not decode image or generate thumbnail for '{}'", filePath.string());
    outItem.thumbnailFile = "";
    return false;
}

// ─── Video Processing & Poster Generation ────────────────────────────────────

void MediaScanner::processVideoFile(const std::filesystem::path& filePath,
                                    const ScanMediaRequest& request,
                                    std::vector<VideoItemInfo>& outVideos) {
    VideoItemInfo video;
    video.mediaType = "video";
    video.filename  = toUtf8(filePath.filename());
    video.title     = toUtf8(filePath.stem());
    video.path      = normalizePathSeparators(filePath);
    video.format    = normalizeExtension(toUtf8(filePath.extension()));

    // Category: the immediate folder of the file, like images (albums in the Videos app)
    std::string folderName = toUtf8(filePath.parent_path().filename());
    if (!folderName.empty()) {
        video.category = folderName;
    } else if (!request.category.empty()) {
        video.category = request.category;
    } else {
        video.category = "Videos";
    }

    std::filesystem::path thumbDir(request.outputThumbnailDir);
    int maxW = request.maxThumbnailWidth > 0 ? request.maxThumbnailWidth : 256;
    int maxH = request.maxThumbnailHeight > 0 ? request.maxThumbnailHeight : 256;

    // A file FFmpeg cannot open or decode cannot be converted either: it is left out of the library
    if (!extractVideoInfoAndPoster(filePath, thumbDir, maxW, maxH, video)) {
        Logger::instance().warn("MediaScanner", "[Video] Skipping '{}': not a readable video", video.path);
        return;
    }

    Logger::instance().debug("MediaScanner", "[Video] Scanned '{}' ({}x{}, {:.1f}s, format='{}', poster='{}', category='{}')",
                            video.filename, video.width, video.height, video.duration, video.format,
                            video.thumbnailFile.empty() ? "none" : video.thumbnailFile, video.category);

    outVideos.push_back(std::move(video));
}

namespace {

// Posters written before embedded thumbnails were read (2026-10-22 17:28 UTC) are redone once for videos
// that carry one
constexpr std::time_t kEmbeddedPosterSince = 1792690127;

// The thumbnail a video file carries: MP4/M4V cover art (an attached picture stream, what YouTube
// downloaders embed) or an MKV/WebM image attachment (a "cover" one first, else the largest).
bool findEmbeddedPicture(AVFormatContext* fmtCtx, const uint8_t*& data, int& size) {
    data = nullptr;
    size = 0;
    int bestScore = -1;
    for (unsigned int i = 0; i < fmtCtx->nb_streams; ++i) {
        const AVStream* st = fmtCtx->streams[i];
        const uint8_t* d = nullptr;
        int n = 0;
        int score = 0;
        if ((st->disposition & AV_DISPOSITION_ATTACHED_PIC) && st->attached_pic.data && st->attached_pic.size > 0) {
            d = st->attached_pic.data;
            n = st->attached_pic.size;
            score = 2;
        } else if (st->codecpar->codec_type == AVMEDIA_TYPE_ATTACHMENT && st->codecpar->extradata &&
                   st->codecpar->extradata_size > 0) {
            const AVDictionaryEntry* mime = av_dict_get(st->metadata, "mimetype", nullptr, 0);
            if (!mime || std::string(mime->value).rfind("image/", 0) != 0) continue;
            const AVDictionaryEntry* name = av_dict_get(st->metadata, "filename", nullptr, 0);
            std::string lower = name ? name->value : "";
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
            d = st->codecpar->extradata;
            n = st->codecpar->extradata_size;
            score = (lower.find("cover") != std::string::npos && lower.find("small") == std::string::npos) ? 1 : 0;
        } else {
            continue;
        }
        if (score > bestScore || (score == bestScore && n > size)) {
            data = d;
            size = n;
            bestScore = score;
        }
    }
    return data != nullptr;
}

} // anonymous namespace

// Poster = the thumbnail embedded in the file when there is one (findEmbeddedPicture), else the first frame
// decoded after seeking to ~10% of the video (skips black intros / fade-ins).
// A poster newer than its video is reused: rescans only read the header (size + duration).
bool MediaScanner::extractVideoInfoAndPoster(const std::filesystem::path& filePath,
                                             const std::filesystem::path& thumbnailDir,
                                             int maxWidth,
                                             int maxHeight,
                                             VideoItemInfo& outVideo) {
    const std::string thumbFileName = sanitizeThumbFilename(toUtf8(filePath.stem())) + "_vthumb.jpg";
    const std::filesystem::path destThumb = thumbnailDir.empty() ? std::filesystem::path() : thumbnailDir / thumbFileName;

    bool posterIsFresh = false;
    bool posterPredatesEmbedded = false;
    if (!destThumb.empty()) {
        std::error_code ec1, ec2;
        auto thumbTime = std::filesystem::last_write_time(destThumb, ec1);
        auto srcTime   = std::filesystem::last_write_time(filePath, ec2);
        posterIsFresh = !ec1 && !ec2 && thumbTime >= srcTime;
        posterPredatesEmbedded = !ec1 && std::chrono::file_clock::to_sys(thumbTime) <
                                         std::chrono::system_clock::from_time_t(kEmbeddedPosterSince);
    }

    AVFormatContext* fmtCtx = nullptr;
    std::string utf8FilePath = toUtf8(filePath);
    if (avformat_open_input(&fmtCtx, utf8FilePath.c_str(), nullptr, nullptr) < 0) {
        Logger::instance().debug("MediaScanner", "[Video] Could not open '{}'", utf8FilePath);
        return false;
    }

    // The picture stream, never the cover art (also a "video" stream in MP4)
    auto findPictureStream = [&]() {
        int idx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (idx >= 0 && (fmtCtx->streams[idx]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            idx = -1;
            for (unsigned int i = 0; i < fmtCtx->nb_streams; ++i) {
                const AVStream* st = fmtCtx->streams[i];
                if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !(st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
                    idx = static_cast<int>(i);
                    break;
                }
            }
        }
        return idx;
    };
    int streamIdx = findPictureStream();
    bool needProbe = streamIdx < 0 || fmtCtx->duration == AV_NOPTS_VALUE ||
                     fmtCtx->streams[streamIdx]->codecpar->width <= 0;
    if (needProbe && avformat_find_stream_info(fmtCtx, nullptr) >= 0) {
        streamIdx = findPictureStream();
    }
    if (streamIdx < 0) {
        Logger::instance().debug("MediaScanner", "[Video] No video stream in '{}'", utf8FilePath);
        avformat_close_input(&fmtCtx);
        return false;
    }

    AVCodecParameters* codecpar = fmtCtx->streams[streamIdx]->codecpar;
    outVideo.width    = codecpar->width;
    outVideo.height   = codecpar->height;
    outVideo.duration = fmtCtx->duration != AV_NOPTS_VALUE
        ? static_cast<double>(fmtCtx->duration) / AV_TIME_BASE : 0.0;

    const uint8_t* embedded = nullptr;
    int embeddedSize = 0;
    const bool hasEmbedded = findEmbeddedPicture(fmtCtx, embedded, embeddedSize);

    if (posterIsFresh && !(hasEmbedded && posterPredatesEmbedded)) {
        outVideo.thumbnailFile = thumbFileName;
        avformat_close_input(&fmtCtx);
        return true;
    }
    if (destThumb.empty()) {
        avformat_close_input(&fmtCtx);
        return true;
    }

    // Scaled to fit maxWidth x maxHeight (never up) and written as the poster JPEG
    auto writeScaled = [&](const uint8_t* const* srcData, const int* srcLinesize, int srcW, int srcH,
                           AVPixelFormat srcFmt, const char* source) {
        double scale = std::min(static_cast<double>(maxWidth) / srcW, static_cast<double>(maxHeight) / srcH);
        if (scale > 1.0) scale = 1.0;
        int targetW = std::max(1, static_cast<int>(std::floor(srcW * scale)));
        int targetH = std::max(1, static_cast<int>(std::floor(srcH * scale)));

        SwsContext* swsCtx = sws_getContext(srcW, srcH, srcFmt, targetW, targetH, AV_PIX_FMT_RGB24,
                                            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!swsCtx) return false;

        std::vector<uint8_t> rgbBuf(targetW * targetH * 3);
        uint8_t* dstData[4] = { rgbBuf.data(), nullptr, nullptr, nullptr };
        int dstLinesize[4] = { targetW * 3, 0, 0, 0 };
        sws_scale(swsCtx, srcData, srcLinesize, 0, srcH, dstData, dstLinesize);
        sws_freeContext(swsCtx);

        std::error_code ec;
        std::filesystem::create_directories(thumbnailDir, ec);
        if (stbi_write_jpg(destThumb.string().c_str(), targetW, targetH, 3, rgbBuf.data(), 85)) {
            outVideo.thumbnailFile = thumbFileName;
            Logger::instance().debug("MediaScanner", "[Video] Poster for '{}' from the {} (scaled: {}x{}) -> '{}'",
                                    toUtf8(filePath.filename()), source, targetW, targetH, toUtf8(destThumb));
            return true;
        }
        Logger::instance().warn("MediaScanner", "Failed to write video poster: {}", toUtf8(destThumb));
        return false;
    };

    if (hasEmbedded) {
        int w = 0, h = 0, channels = 0;
        unsigned char* pixels = stbi_load_from_memory(embedded, embeddedSize, &w, &h, &channels, 3);
        if (pixels && w > 0 && h > 0) {
            const uint8_t* srcData[4] = { pixels, nullptr, nullptr, nullptr };
            int srcLinesize[4] = { w * 3, 0, 0, 0 };
            const bool written = writeScaled(srcData, srcLinesize, w, h, AV_PIX_FMT_RGB24, "embedded thumbnail");
            stbi_image_free(pixels);
            if (written) {
                avformat_close_input(&fmtCtx);
                return true;
            }
        } else {
            // e.g. a WebP cover (stb reads JPEG / PNG): the frame below is used
            if (pixels) stbi_image_free(pixels);
            Logger::instance().debug("MediaScanner", "[Video] Embedded thumbnail of '{}' is not JPEG/PNG; using a frame", utf8FilePath);
        }
    }

    const AVCodec* decoder = avcodec_find_decoder(codecpar->codec_id);
    AVCodecContext* codecCtx = decoder ? avcodec_alloc_context3(decoder) : nullptr;
    if (!codecCtx || avcodec_parameters_to_context(codecCtx, codecpar) < 0 || avcodec_open2(codecCtx, decoder, nullptr) < 0) {
        Logger::instance().debug("MediaScanner", "[Video] No decoder for '{}'", utf8FilePath);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&fmtCtx);
        return false;
    }

    if (outVideo.duration > 1.0) {
        int64_t start  = fmtCtx->start_time != AV_NOPTS_VALUE ? fmtCtx->start_time : 0;
        int64_t target = start + static_cast<int64_t>(outVideo.duration * 0.10 * AV_TIME_BASE);
        if (av_seek_frame(fmtCtx, -1, target, AVSEEK_FLAG_BACKWARD) < 0) {
            av_seek_frame(fmtCtx, -1, start, AVSEEK_FLAG_BACKWARD);
        }
    }

    AVPacket* pkt   = av_packet_alloc();
    AVFrame*  frame = av_frame_alloc();
    bool frameDecoded = false;
    int  packetsRead  = 0;
    constexpr int kMaxPackets = 600; // a broken file must not stall the scan

    auto writePoster = [&]() {
        writeScaled(frame->data, frame->linesize, frame->width, frame->height,
                    static_cast<AVPixelFormat>(frame->format), "frame at ~10%");
    };

    while (!frameDecoded && packetsRead < kMaxPackets && av_read_frame(fmtCtx, pkt) >= 0) {
        ++packetsRead;
        if (pkt->stream_index == streamIdx && avcodec_send_packet(codecCtx, pkt) >= 0 &&
            avcodec_receive_frame(codecCtx, frame) >= 0) {
            writePoster();
            frameDecoded = true;
        }
        av_packet_unref(pkt);
    }
    // Decoders with delay (frame threads, B-frames) hold the first frame until flushed
    if (!frameDecoded && avcodec_send_packet(codecCtx, nullptr) >= 0 && avcodec_receive_frame(codecCtx, frame) >= 0) {
        writePoster();
        frameDecoded = true;
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&fmtCtx);

    if (!frameDecoded) {
        Logger::instance().debug("MediaScanner", "[Video] Could not decode a poster frame for '{}'", utf8FilePath);
    }
    return true;
}

} // namespace ifs
