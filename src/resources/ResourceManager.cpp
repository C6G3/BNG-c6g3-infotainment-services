#include "resources/ResourceManager.hpp"
#include "resources/MediaScanner.hpp"
#include "platform/windows/ShortcutResolver.hpp"
#include "core/Logger.hpp"

#include <thread>
#include <mutex>
#include <vector>
#include <algorithm>

namespace ifs {

namespace {

std::string computeCachedPath(const std::string& targetDir, const std::string& filename) {
    std::string normalized = targetDir;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    if (!normalized.empty() && normalized.back() != '/') {
        normalized.push_back('/');
    }

    auto artPos = normalized.find("/art/");
    if (artPos != std::string::npos) {
        return normalized.substr(artPos) + filename;
    }

    auto artPos2 = normalized.find("art/");
    if (artPos2 != std::string::npos) {
        return "/" + normalized.substr(artPos2) + filename;
    }

    return normalized + filename;
}

} // anonymous namespace

struct ResourceManager::Impl {
    MediaScanner scanner;
    std::mutex   workersMutex;
    std::vector<std::thread> workers;

    ~Impl() {
        std::lock_guard lock(workersMutex);
        for (auto& w : workers) {
            if (w.joinable()) {
                w.join();
            }
        }
    }
};

ResourceManager::ResourceManager()
    : impl_(std::make_unique<Impl>()) {}

ResourceManager::~ResourceManager() = default;

void ResourceManager::scanExternalMediaAsync(const ScanMediaRequest& request, ScanCallback onComplete) {
    Logger::instance().info("ResourceManager", "Starting async scan for request '{}' on target '{}'",
                            request.requestId, request.targetPath);

    std::thread worker([this, request, onComplete = std::move(onComplete)]() {
        ScanMediaResult result = impl_->scanner.scan(request);
        if (onComplete) {
            onComplete(result);
        }
    });

    std::lock_guard lock(impl_->workersMutex);
    auto it = impl_->workers.begin();
    while (it != impl_->workers.end()) {
        if (!it->joinable()) {
            it = impl_->workers.erase(it);
        } else {
            ++it;
        }
    }
    impl_->workers.push_back(std::move(worker));
}

CacheMediaResult ResourceManager::cacheMedia(const CacheMediaRequest& request) {
    CacheMediaResult result;
    result.requestId = request.requestId;

    std::filesystem::path src(request.sourcePath);
    if (ShortcutResolver::isShortcut(src)) {
        if (auto resolved = ShortcutResolver::resolve(src)) {
            src = *resolved;
        }
    }

    if (!std::filesystem::exists(src) || !std::filesystem::is_regular_file(src)) {
        Logger::instance().warn("ResourceManager", "Source file not found: {}", request.sourcePath);
        result.status = "error";
        result.error  = "Source file does not exist or is not a regular file";
        return result;
    }

    std::string outFilename = request.filename.empty() ? src.filename().string() : request.filename;
    result.filename = outFilename;

    std::filesystem::path dstDir(request.targetDir);
    std::error_code ec;
    std::filesystem::create_directories(dstDir, ec);
    if (ec) {
        Logger::instance().error("ResourceManager", "Failed to create target directory {}: {}",
                                 request.targetDir, ec.message());
        result.status = "error";
        result.error  = "Failed to create target directory: " + ec.message();
        return result;
    }

    std::filesystem::path dstFile = dstDir / outFilename;
    std::filesystem::copy_file(src, dstFile, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        Logger::instance().error("ResourceManager", "Failed to copy '{}' to '{}': {}",
                                 src.string(), dstFile.string(), ec.message());
        result.status = "error";
        result.error  = "Failed to copy file: " + ec.message();
        return result;
    }

    result.status     = "success";
    result.cachedPath = computeCachedPath(request.targetDir, outFilename);

    Logger::instance().info("ResourceManager", "Cached '{}' -> '{}' (virtual: '{}')",
                            src.string(), dstFile.string(), result.cachedPath);
    return result;
}

void ResourceManager::cacheExternalMediaAsync(const CacheMediaRequest& request, CacheCallback onComplete) {
    Logger::instance().info("ResourceManager", "Starting async cache for request '{}' from '{}'",
                            request.requestId, request.sourcePath);

    std::thread worker([this, request, onComplete = std::move(onComplete)]() {
        CacheMediaResult result = cacheMedia(request);
        if (onComplete) {
            onComplete(result);
        }
    });

    std::lock_guard lock(impl_->workersMutex);
    auto it = impl_->workers.begin();
    while (it != impl_->workers.end()) {
        if (!it->joinable()) {
            it = impl_->workers.erase(it);
        } else {
            ++it;
        }
    }
    impl_->workers.push_back(std::move(worker));
}

} // namespace ifs
