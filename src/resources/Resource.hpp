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

/// Parameters for SCAN_EXTERNAL_MEDIA request.
struct ScanMediaRequest {
    std::string requestId;
    std::string targetPath;
    std::vector<std::string> mediaTypes;
    std::vector<std::string> extensions;
    std::string outputThumbnailDir;
};

/// Result of SCAN_EXTERNAL_MEDIA operation.
struct ScanMediaResult {
    std::string requestId;
    std::string targetPath;
    std::string status = "success"; // "success" or "error"
    std::string error;
    std::vector<AudioTrackInfo> tracks;

    nlohmann::json toJson() const {
        nlohmann::json trackArray = nlohmann::json::array();
        for (const auto& t : tracks) {
            trackArray.push_back(t.toJson());
        }

        nlohmann::json res = {
            {"event",      "EXTERNAL_MEDIA_SCANNED"},
            {"requestId",  requestId},
            {"targetPath", targetPath},
            {"status",     status},
            {"tracks",     trackArray}
        };

        if (!error.empty()) {
            res["error"] = error;
        }

        return res;
    }
};

} // namespace ifs
