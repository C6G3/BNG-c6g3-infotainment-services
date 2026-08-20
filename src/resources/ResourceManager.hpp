#pragma once

#include "resources/Resource.hpp"
#include <functional>
#include <memory>

namespace ifs {

/// Manager for external resources and media scanning operations.
class ResourceManager {
public:
    ResourceManager();
    ~ResourceManager();

    ResourceManager(const ResourceManager&)            = delete;
    ResourceManager& operator=(const ResourceManager&) = delete;

    using ScanCallback = std::function<void(const ScanMediaResult&)>;

    /// Scans external media asynchronously on a dedicated worker thread.
    /// onComplete is invoked on the worker thread when the scan finishes.
    void scanExternalMediaAsync(const ScanMediaRequest& request, ScanCallback onComplete);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ifs
