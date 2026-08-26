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
#include <cmath>

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
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.front()))) str.erase(0, 1);
    while (!str.empty() && std::isspace(static_cast<unsigned char>(str.back()))) str.pop_back();
    return str;
}

void parseFilenameFallback(const std::string& stem, std::string& outArtist, std::string& outTitle) {
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

ScanMediaResult MediaScanner::scan(const ScanMediaRequest& request, ProgressCallback onProgress) {
    ScanMediaResult result;
    result.requestId  = request.requestId;
    result.targetPath = request.targetPath;

    // Determine mode: image vs audio
    bool isImageMode = false;
    for (const auto& t : request.mediaTypes) {
        std::string lt = toLower(t);
        if (lt == "image" || lt == "images" || lt == "picture" || lt == "pictures" || lt == "wallpaper" || lt == "wallpapers") {
            isImageMode = true;
            break;
        }
    }
    result.primaryMediaType = isImageMode ? "image" : "audio";

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

            if (!isImageMode && kImageExts.count(fileExt)) {
                isImageMode = true;
                result.primaryMediaType = "image";
            }

            filesToProcess.push_back({targetDir, activeRequest});
        }
    }

    const uint64_t totalCount = filesToProcess.size();

    // Report discovery start progress
    if (onProgress) {
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

    for (size_t i = 0; i < filesToProcess.size(); ++i) {
        const auto& entry = filesToProcess[i];
        if (isImageMode) {
            processImageFile(entry.path, entry.request, items);
        } else {
            processAudioFile(entry.path, entry.request, tracks);
        }

        if (onProgress && totalCount > 0) {
            uint64_t current = i + 1;
            double pct = (static_cast<double>(current) / totalCount) * 100.0;
            OperationProgress prog;
            prog.requestId = activeRequest.requestId;
            prog.action    = "SCANNING";
            prog.progress  = std::min(100.0, pct);
            prog.current   = current;
            prog.total     = totalCount;
            prog.unit      = "items";
            prog.detail    = entry.path.filename().string();
            onProgress(prog);
        }
    }

    result.status = "success";
    result.tracks = std::move(tracks);
    result.items  = std::move(items);

    size_t count = isImageMode ? result.items.size() : result.tracks.size();

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

    Logger::instance().debug("MediaScanner", "Scanning directory: '{}'", dir.string());

    try {
        for (const auto& entry : std::filesystem::directory_iterator(dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
            if (ec) continue;

            const auto& path = entry.path();

            if (ShortcutResolver::isShortcut(path)) {
                if (auto resolved = ShortcutResolver::resolve(path)) {
                    Logger::instance().debug("MediaScanner", "Resolved shortcut '{}' -> '{}'",
                                            path.string(), resolved->string());

                    std::string shortcutCategory = path.stem().string();
                    ScanMediaRequest subReq = request;
                    if (subReq.category.empty()) {
                        subReq.category = shortcutCategory;
                    }

                    if (std::filesystem::is_directory(*resolved)) {
                        collectFiles(*resolved, subReq, isImageMode, extensionsSet, visitedDirs, outFiles);
                    } else if (std::filesystem::is_regular_file(*resolved)) {
                        auto ext = normalizeExtension(resolved->extension().string());
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
                auto ext = normalizeExtension(path.extension().string());
                if (extensionsSet.empty() || extensionsSet.count(ext)) {
                    outFiles.push_back({path, request});
                }
            }
        }
    } catch (const std::exception& e) {
        Logger::instance().warn("MediaScanner", "Error scanning directory {}: {}", dir.string(), e.what());
    }
}

// ─── Audio Processing ────────────────────────────────────────────────────────

void MediaScanner::processAudioFile(const std::filesystem::path& filePath,
                                    const ScanMediaRequest& request,
                                    std::vector<AudioTrackInfo>& outTracks) {
    AudioTrackInfo track;
    track.mediaType = "audio";
    track.path      = normalizePathSeparators(filePath);
    track.filename  = filePath.filename().string();

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
    int ret = avformat_open_input(&fmtCtx, filePath.string().c_str(), nullptr, nullptr);
    if (ret < 0) {
        std::string stem = filePath.stem().string();
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
    item.filename  = filePath.filename().string();
    item.title     = filePath.stem().string();
    item.path      = normalizePathSeparators(filePath);
    item.format    = normalizeExtension(filePath.extension().string());

    // Category: uses request.category if specified, otherwise parent folder name
    if (!request.category.empty()) {
        item.category = request.category;
    } else {
        item.category = filePath.parent_path().filename().string();
        if (item.category.empty()) {
            item.category = "Images";
        }
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
    std::string stem = filePath.stem().string();
    std::string ext  = outItem.format;
    std::string thumbExt = (ext == "jpg" || ext == "jpeg") ? ".jpg" : ".png";
    std::string thumbFileName = stem + "_thumb" + thumbExt;

    // 1. Try stb_image first (very fast for PNG/JPG/BMP/TGA/GIF)
    int origW = 0, origH = 0, channels = 0;
    unsigned char* imgData = stbi_load(filePath.string().c_str(), &origW, &origH, &channels, 0);

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
                                        filePath.filename().string(), origW, origH, targetW, targetH, destThumb.string());
            } else {
                Logger::instance().warn("MediaScanner", "Failed to write image thumbnail: {}", destThumb.string());
                outItem.thumbnailFile = "";
            }
        }

        stbi_image_free(imgData);
        return true;
    }

    // 2. Fallback to FFmpeg (for WebP, DDS, HEIC, AVIF, TIFF, etc.)
    AVFormatContext* fmtCtx = nullptr;
    if (avformat_open_input(&fmtCtx, filePath.string().c_str(), nullptr, nullptr) >= 0) {
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
                                                                                filePath.filename().string(), frame->width, frame->height, targetW, targetH, destThumb.string());
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

} // namespace ifs
