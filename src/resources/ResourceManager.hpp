#pragma once

#include "resources/Resource.hpp"
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ifs
