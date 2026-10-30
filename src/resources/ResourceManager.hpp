#pragma once

#include "resources/Resource.hpp"
#include "resources/VideoStreamer.hpp"
#include "resources/RadioStreamer.hpp"
#include "resources/RadioDiscovery.hpp"
#include "resources/WebRtcProbe.hpp"
#include "resources/WebAppHost.hpp"
#include "resources/AuxCapture.hpp"
#include "security/AppLock.hpp"
#include <functional>
#include <memory>

namespace ifs {

/// Manager for external resources, media scanning, and caching operations.
class ResourceManager {
public:
    ResourceManager();
    ~ResourceManager();

    ResourceManager(const ResourceManager&)            = delete;
    ResourceManager& operator=(const ResourceManager&) = delete;

    using ScanCallback  = std::function<void(const ScanMediaResult&)>;
    using CacheCallback = std::function<void(const CacheMediaResult&)>;

    /// Scans external media asynchronously on a dedicated worker thread with optional progress reporting.
    void scanExternalMediaAsync(const ScanMediaRequest& request,
                                ScanCallback onComplete,
                                ProgressCallback onProgress = nullptr);

    /// Caches an external media file to targetDir asynchronously on a worker thread with optional progress reporting.
    void cacheExternalMediaAsync(const CacheMediaRequest& request,
                                 CacheCallback onComplete,
                                 ProgressCallback onProgress = nullptr);

    /// Synchronous caching implementation with optional progress reporting.
    CacheMediaResult cacheMedia(const CacheMediaRequest& request,
                                ProgressCallback onProgress = nullptr);

    /// Converts an image (e.g. PNG to WebP) asynchronously on a dedicated worker thread with optional progress reporting.
    void convertImageAsync(const ConvertImageRequest& request,
                           ConvertCallback onComplete,
                           ProgressCallback onProgress = nullptr);

    /// Synchronous image conversion implementation with optional progress reporting.
    ConvertImageResult convertImage(const ConvertImageRequest& request,
                                    ProgressCallback onProgress = nullptr);

    /// Sets progress reporting thresholds.
    void setProgressConfig(const ProgressConfig& config);

    /// Gets current progress reporting thresholds.
    const ProgressConfig& getProgressConfig() const;

    /// Progressive video for the Videos app (VIDEO_STREAM_* messages).
    VideoStreamer& videoStreamer();

    /// Live radio for the Radio app (RADIO_STREAM_* messages).
    RadioStreamer& radioStreamer();

    /// The Radio app's "Add station" (RADIO_DISCOVER, RADIO_SAVE_LOGO messages).
    RadioDiscovery& radioDiscovery();

    /// TEMPORARY in-game WebRTC check for the web apps (WEBRTC_PROBE_* messages).
    WebRtcProbe& webRtcProbe();

    /// Web apps (YouTube) in the user's hidden browser, streamed over WebRTC (WEBAPP_* messages).
    WebAppHost& webAppHost();

    /// The Aux app's VB-CABLE input, captured by the hidden browser and streamed over WebRTC (AUX_* messages).
    AuxCapture& auxCapture();

    /// The app lock's password for the protected web apps (APPLOCK_* messages).
    AppLock& appLock();

    /// Runs a job on a worker thread (the app lock's password hashing takes about half a second).
    void runAsync(std::function<void()> job);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ifs
