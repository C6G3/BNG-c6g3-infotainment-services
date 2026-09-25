#include "resources/ResourceManager.hpp"
#include "resources/MediaScanner.hpp"
#include "platform/windows/ShortcutResolver.hpp"
#include "core/Logger.hpp"

#include "stb_image.h"
#include <webp/encode.h>
#include <webp/decode.h>

#include <thread>
#include <mutex>
#include <vector>
#include <algorithm>
#include <fstream>

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

std::string normalizePathSeparators(const std::filesystem::path& path) {
    std::string s = toUtf8(path);
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

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

std::string computeVfsPath(const std::string& path) {
    std::string normalized = path;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    auto artPos = normalized.find("/art/");
    if (artPos != std::string::npos) {
        return normalized.substr(artPos);
    }
    auto artPos2 = normalized.find("art/");
    if (artPos2 != std::string::npos) {
        return "/" + normalized.substr(artPos2);
    }
    return normalized;
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

    std::string outFilename = request.filename.empty() ? toUtf8(src.filename()) : request.filename;
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
                                     toUtf8(src), toUtf8(dstFile), ec.message());
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

ConvertImageResult ResourceManager::convertImage(const ConvertImageRequest& request, ProgressCallback onProgress) {
    ConvertImageResult result;
    result.requestId = request.requestId;

    std::filesystem::path src(request.sourcePath);
    if (ShortcutResolver::isShortcut(src)) {
        if (auto resolved = ShortcutResolver::resolve(src)) {
            src = *resolved;
        }
    }

    if (!std::filesystem::exists(src) || !std::filesystem::is_regular_file(src)) {
        Logger::instance().warn("ResourceManager", "Source file not found for conversion: {}", request.sourcePath);
        result.status = "error";
        result.error  = "Source file does not exist or is not a regular file";
        return result;
    }

    std::error_code ec;
    uint64_t totalBytes = std::filesystem::file_size(src, ec);
    if (ec) totalBytes = 0;

    const bool shouldReportProgress = onProgress && (totalBytes >= impl_->progressConfig.minFileSizeBytes);

    if (shouldReportProgress) {
        OperationProgress prog;
        prog.requestId = request.requestId;
        prog.action    = "CONVERTING";
        prog.progress  = 0.0;
        prog.current   = 0;
        prog.total     = totalBytes;
        prog.unit      = "bytes";
        prog.detail    = toUtf8(src.filename());
        onProgress(prog);
    }

    // Read source file into memory
    std::vector<unsigned char> srcBuffer;
    {
        std::ifstream file(src, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            result.status = "error";
            result.error  = "Failed to open source file for reading";
            return result;
        }
        std::streamsize size = file.tellg();
        if (size <= 0) {
            result.status = "error";
            result.error  = "Source file is empty";
            return result;
        }
        file.seekg(0, std::ios::beg);
        srcBuffer.resize(static_cast<size_t>(size));
        if (!file.read(reinterpret_cast<char*>(srcBuffer.data()), size)) {
            result.status = "error";
            result.error  = "Failed to read source file content";
            return result;
        }
    }

    int width = 0, height = 0, channels = 0;
    unsigned char* imgData = stbi_load_from_memory(srcBuffer.data(), static_cast<int>(srcBuffer.size()), &width, &height, &channels, 0);

    // If STB failed, attempt WebP decode if source is WebP
    if (!imgData) {
        imgData = WebPDecodeRGBA(srcBuffer.data(), srcBuffer.size(), &width, &height);
        if (imgData) {
            channels = 4;
        }
    }

    if (!imgData || width <= 0 || height <= 0) {
        Logger::instance().error("ResourceManager", "Failed to decode image '{}'", request.sourcePath);
        result.status = "error";
        result.error  = "Failed to decode source image";
        return result;
    }

    // Prepare destination path
    std::string outDestPath = request.destinationPath;
    if (outDestPath.empty()) {
        std::filesystem::path defaultDst = src.parent_path() / (src.stem().string() + ".webp");
        outDestPath = normalizePathSeparators(defaultDst);
    }
    std::filesystem::path dst(outDestPath);

    // Create destination directories if needed
    std::filesystem::create_directories(dst.parent_path(), ec);
    if (ec) {
        stbi_image_free(imgData);
        result.status = "error";
        result.error  = "Failed to create destination directory: " + ec.message();
        return result;
    }

    // Encoding
    uint8_t* webpData = nullptr;
    size_t webpSize = 0;
    float quality = static_cast<float>(request.quality > 0 ? std::clamp(request.quality, 1, 100) : 88);

    if (channels == 4) {
        webpSize = WebPEncodeRGBA(imgData, width, height, width * 4, quality, &webpData);
    } else if (channels == 3) {
        webpSize = WebPEncodeRGB(imgData, width, height, width * 3, quality, &webpData);
    } else {
        // Convert 1 or 2 channels to RGBA
        std::vector<unsigned char> rgba(width * height * 4);
        for (int i = 0; i < width * height; ++i) {
            unsigned char v = imgData[i * channels];
            unsigned char a = (channels == 2) ? imgData[i * channels + 1] : 255;
            rgba[i * 4 + 0] = v;
            rgba[i * 4 + 1] = v;
            rgba[i * 4 + 2] = v;
            rgba[i * 4 + 3] = a;
        }
        webpSize = WebPEncodeRGBA(rgba.data(), width, height, width * 4, quality, &webpData);
    }

    stbi_image_free(imgData);

    if (webpSize == 0 || !webpData) {
        Logger::instance().error("ResourceManager", "Failed to encode image to WebP for '{}'", request.sourcePath);
        result.status = "error";
        result.error  = "Failed to encode image to WebP";
        return result;
    }

    // Write WebP to disk
    {
        std::ofstream outFile(dst, std::ios::binary | std::ios::trunc);
        if (!outFile.is_open()) {
            WebPFree(webpData);
            result.status = "error";
            result.error  = "Failed to open destination file for writing";
            return result;
        }
        outFile.write(reinterpret_cast<const char*>(webpData), webpSize);
        if (!outFile) {
            WebPFree(webpData);
            result.status = "error";
            result.error  = "Failed to write WebP data to destination file";
            return result;
        }
    }
    WebPFree(webpData);

    // If keepSource is false and src != dst, remove source file
    if (!request.keepSource && std::filesystem::weakly_canonical(src, ec) != std::filesystem::weakly_canonical(dst, ec)) {
        std::filesystem::remove(src, ec);
    }

    if (shouldReportProgress) {
        OperationProgress prog;
        prog.requestId = request.requestId;
        prog.action    = "CONVERTING";
        prog.progress  = 100.0;
        prog.current   = totalBytes;
        prog.total     = totalBytes;
        prog.unit      = "bytes";
        prog.detail    = toUtf8(dst.filename());
        onProgress(prog);
    }

    result.status = "success";
    result.data.sourcePath      = request.sourcePath;
    result.data.destinationPath = outDestPath;
    result.data.vfsPath         = computeVfsPath(outDestPath);
    result.data.format          = request.targetFormat.empty() ? "webp" : request.targetFormat;
    result.data.level           = request.level;
    result.data.width           = width;
    result.data.height          = height;
    result.data.fileSize        = webpSize;

    Logger::instance().info("ResourceManager", "Converted image '{}' -> '{}' (format='{}', size={} bytes, {}x{})",
                            request.sourcePath, outDestPath, result.data.format, result.data.fileSize, width, height);

    return result;
}

void ResourceManager::convertImageAsync(const ConvertImageRequest& request,
                                        ConvertCallback onComplete,
                                        ProgressCallback onProgress) {
    Logger::instance().info("ResourceManager", "Starting async image conversion for request '{}' from '{}'",
                            request.requestId, request.sourcePath);

    std::thread worker([this, request, onComplete = std::move(onComplete), onProgress = std::move(onProgress)]() {
        ConvertImageResult result = convertImage(request, onProgress);
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
