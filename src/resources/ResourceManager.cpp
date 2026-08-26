#include "resources/ResourceManager.hpp"
#include "resources/MediaScanner.hpp"
#include "platform/windows/ShortcutResolver.hpp"
#include "core/Logger.hpp"

#include <thread>
#include <mutex>
#include <vector>
#include <algorithm>
#include <fstream>

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
    MediaScanner   scanner;
    ProgressConfig progressConfig;
    std::mutex     workersMutex;
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

void ResourceManager::setProgressConfig(const ProgressConfig& config) {
    std::lock_guard lock(impl_->workersMutex);
    impl_->progressConfig = config;
}

const ProgressConfig& ResourceManager::getProgressConfig() const {
    return impl_->progressConfig;
}

void ResourceManager::scanExternalMediaAsync(const ScanMediaRequest& request,
                                             ScanCallback onComplete,
                                             ProgressCallback onProgress) {
    Logger::instance().info("ResourceManager", "Starting async scan for request '{}' on target '{}'",
                            request.requestId, request.targetPath);

    ProgressConfig cfg;
    {
        std::lock_guard lock(impl_->workersMutex);
        cfg = impl_->progressConfig;
    }

    std::thread worker([this, request, onComplete = std::move(onComplete), onProgress = std::move(onProgress), cfg]() {
        ScanMediaResult result = impl_->scanner.scan(request, onProgress, cfg);
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

CacheMediaResult ResourceManager::cacheMedia(const CacheMediaRequest& request, ProgressCallback onProgress) {
    CacheMediaResult result;
    result.requestId  = request.requestId;
    result.sourcePath = request.sourcePath;
    result.mediaType  = request.mediaType;

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
    uint64_t totalBytes = std::filesystem::file_size(src, ec);
    if (ec) totalBytes = 0;

    const bool shouldReportProgress = onProgress && (totalBytes >= impl_->progressConfig.minFileSizeBytes);

    // Report initial progress (0%)
    if (shouldReportProgress) {
        OperationProgress prog;
        prog.requestId = request.requestId;
        prog.action    = "COPYING";
        prog.progress  = 0.0;
        prog.current   = 0;
        prog.total     = totalBytes;
        prog.unit      = "bytes";
        prog.detail    = outFilename;
        onProgress(prog);
    }

    std::ifstream in(src, std::ios::binary);
    std::ofstream out(dstFile, std::ios::binary | std::ios::trunc);
    if (!in.is_open() || !out.is_open()) {
        // Fallback to std::filesystem::copy_file if streams fail
        std::filesystem::copy_file(src, dstFile, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            Logger::instance().error("ResourceManager", "Failed to copy '{}' to '{}': {}",
                                     src.string(), dstFile.string(), ec.message());
            result.status = "error";
            result.error  = "Failed to copy file: " + ec.message();
            return result;
        }
    } else {
        constexpr size_t kChunkSize = 256 * 1024; // 256 KB buffer chunks
        std::vector<char> buffer(kChunkSize);
        uint64_t copiedBytes = 0;
        double lastReportedPercent = 0.0;

        while (in.read(buffer.data(), buffer.size()) || in.gcount() > 0) {
            std::streamsize bytesRead = in.gcount();
            out.write(buffer.data(), bytesRead);
            if (!out) {
                result.status = "error";
                result.error  = "Write error during file copy";
                return result;
            }
            copiedBytes += bytesRead;

            if (shouldReportProgress && totalBytes > 0) {
                double pct = (static_cast<double>(copiedBytes) / totalBytes) * 100.0;
                if (pct - lastReportedPercent >= 5.0 || copiedBytes >= totalBytes) {
                    lastReportedPercent = pct;
                    OperationProgress prog;
                    prog.requestId = request.requestId;
                    prog.action    = "COPYING";
                    prog.progress  = std::min(100.0, pct);
                    prog.current   = copiedBytes;
                    prog.total     = totalBytes;
                    prog.unit      = "bytes";
                    prog.detail    = outFilename;
                    onProgress(prog);
                }
            }
        }
        in.close();
        out.close();
    }

    // Report final progress (100%)
    if (shouldReportProgress) {
        OperationProgress prog;
        prog.requestId = request.requestId;
        prog.action    = "COPYING";
        prog.progress  = 100.0;
        prog.current   = totalBytes;
        prog.total     = totalBytes;
        prog.unit      = "bytes";
        prog.detail    = outFilename;
        onProgress(prog);
    }

    result.status     = "success";
    result.cachedPath = computeCachedPath(request.targetDir, outFilename);

    Logger::instance().info("ResourceManager", "Cached '{}' -> '{}' (mediaType='{}', cachedPath='{}')",
                            src.string(), dstFile.string(), request.mediaType, result.cachedPath);
    return result;
}

void ResourceManager::cacheExternalMediaAsync(const CacheMediaRequest& request,
                                              CacheCallback onComplete,
                                              ProgressCallback onProgress) {
    Logger::instance().info("ResourceManager", "Starting async cache for request '{}' from '{}'",
                            request.requestId, request.sourcePath);

    std::thread worker([this, request, onComplete = std::move(onComplete), onProgress = std::move(onProgress)]() {
        CacheMediaResult result = cacheMedia(request, onProgress);
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
