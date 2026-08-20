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

    /// Scans external media asynchronously on a dedicated worker thread.
    void scanExternalMediaAsync(const ScanMediaRequest& request, ScanCallback onComplete);

    /// Caches an external media file to targetDir asynchronously on a worker thread.
    void cacheExternalMediaAsync(const CacheMediaRequest& request, CacheCallback onComplete);

    /// Synchronous caching implementation.
    CacheMediaResult cacheMedia(const CacheMediaRequest& request);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ifs
