#pragma once

#include "app/Config.hpp"
#include "resources/ResourceManager.hpp"

#include <memory>
#include <atomic>
#include <chrono>

namespace ifs {

class WebSocketServer;

class Application {
public:
    Application();
    ~Application();

    Application(const Application&)            = delete;
    Application& operator=(const Application&) = delete;

    /// Parse args, load config, initialise subsystems. Returns false on failure.
    bool initialize(int argc, char* argv[]);

    /// Start the server and block until shutdown is requested. Returns exit code.
    int run();

    /// Request a graceful shutdown (safe to call from signal handlers).
    void shutdown();

    /// Seconds elapsed since run() was called.
    double getUptimeSeconds() const;

private:
    void setupSignalHandlers();

    Config                                config_;
    ResourceManager                       resourceManager_;
    std::unique_ptr<WebSocketServer>      server_;
    std::chrono::steady_clock::time_point startTime_;
    std::atomic<bool>                     running_{false};
};

} // namespace ifs
