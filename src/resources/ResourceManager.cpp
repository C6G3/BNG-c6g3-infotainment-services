#include "resources/ResourceManager.hpp"
#include "resources/MediaScanner.hpp"
#include "core/Logger.hpp"

#include <thread>
#include <mutex>
#include <vector>

namespace ifs {

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

    // Cleanup finished threads and track the new one
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
