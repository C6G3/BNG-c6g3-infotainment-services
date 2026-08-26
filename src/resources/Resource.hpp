#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace ifs {

/// Represents extracted metadata and thumbnail info for an audio track.
struct AudioTrackInfo {
    std::string mediaType = "audio";
    std::string path;
    std::string title;
    std::string artist;
    std::string album;
    std::string filename;
    std::string thumbnailFile;
    std::string cover;
    std::string category = "music";

    nlohmann::json toJson() const {
        return {
            {"mediaType",     mediaType},
            {"path",          path},
            {"title",         title},
            {"artist",        artist},
            {"album",         album},
            {"filename",      filename},
            {"thumbnailFile", thumbnailFile},
            {"cover",         cover},
            {"category",      category}
        };
    }
};

/// Represents extracted info and generated thumbnail for an image item.
struct ImageItemInfo {
    std::string mediaType = "image";
    std::string filename;
    std::string title;
    std::string category;
    std::string path;
    std::string thumbnailFile;
    int width = 0;
    int height = 0;
    std::string format;

    nlohmann::json toJson() const {
        return {
            {"mediaType",     mediaType},
            {"filename",      filename},
            {"title",         title},
            {"category",      category},
            {"path",          path},
            {"thumbnailFile", thumbnailFile},
            {"width",         width},
            {"height",        height},
            {"format",        format}
        };
    }
};

/// Parameters for SCAN_EXTERNAL_MEDIA request.
struct ScanMediaRequest {
    std::string requestId;
    std::string targetPath;
    std::string category;
    std::vector<std::string> mediaTypes;
    std::vector<std::string> extensions;
    int maxThumbnailWidth = 256;
    int maxThumbnailHeight = 256;
    std::string outputThumbnailDir;
};

/// Result of SCAN_EXTERNAL_MEDIA operation.
struct ScanMediaResult {
    std::string requestId;
    std::string targetPath;
    std::string status = "success"; // "success" or "error"
    std::string error;
    std::string primaryMediaType = "audio"; // "audio" or "image"
    std::vector<AudioTrackInfo> tracks;
    std::vector<ImageItemInfo> items;

    nlohmann::json toJson() const {
        nlohmann::json res = {
            {"event",      "EXTERNAL_MEDIA_SCANNED"},
            {"requestId",  requestId},
            {"status",     status},
            {"targetPath", targetPath}
        };

        if (!error.empty()) {
            res["error"] = error;
        }

        res["mediaType"] = primaryMediaType;
        nlohmann::json itemArray = nlohmann::json::array();
        if (primaryMediaType == "image") {
            for (const auto& item : items) {
                itemArray.push_back(item.toJson());
            }
        } else {
            for (const auto& t : tracks) {
                itemArray.push_back(t.toJson());
            }
        }
        res["items"] = itemArray;

        return res;
    }
};

/// Parameters for CACHE_EXTERNAL_MEDIA request.
struct CacheMediaRequest {
    std::string requestId;
    std::string sourcePath;
    std::string targetDir;
    std::string filename;
    std::string mediaType; // "image", "audio", etc.
};

/// Result of CACHE_EXTERNAL_MEDIA operation.
struct CacheMediaResult {
    std::string requestId;
    std::string status = "success"; // "success" or "error"
    std::string mediaType;
    std::string sourcePath;
    std::string cachedPath;
    std::string filename;
    std::string error;

    nlohmann::json toJson() const {
        nlohmann::json res = {
            {"event",      "EXTERNAL_MEDIA_CACHED"},
            {"status",     status},
            {"requestId",  requestId}
        };

        if (!mediaType.empty()) {
            res["mediaType"] = mediaType;
        }
        if (!sourcePath.empty()) {
            res["sourcePath"] = sourcePath;
        }

        res["cachedPath"] = cachedPath;
        res["filename"]   = filename;

        if (!error.empty()) {
            res["error"] = error;
        }

        return res;
    }
};

/// Progress event model for ongoing operations (copying, downloading, scanning).
struct OperationProgress {
    std::string requestId;
    std::string action;    // e.g. "COPYING", "DOWNLOADING", "SCANNING", "PROCESSING"
    double progress = 0.0; // 0.0 to 100.0
    uint64_t current = 0;  // current bytes or items
    uint64_t total = 0;    // total bytes or items (0 if unknown)
    std::string unit = "bytes"; // "bytes" or "items"
    std::string detail;    // current file name or subtask description

    nlohmann::json toJson() const {
        return {
            {"event",     "OPERATION_PROGRESS"},
            {"requestId", requestId},
            {"action",    action},
            {"progress",  std::round(progress * 10.0) / 10.0},
            {"current",   current},
            {"total",     total},
            {"unit",      unit},
            {"detail",    detail}
        };
    }
};

using ProgressCallback = std::function<void(const OperationProgress&)>;

} // namespace ifs
