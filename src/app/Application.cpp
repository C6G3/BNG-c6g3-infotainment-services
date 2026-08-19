#include "app/Application.hpp"
#include "server/WebSocketServer.hpp"
#include "core/Logger.hpp"
#include "core/Types.hpp"

#include <nlohmann/json.hpp>
#include <csignal>
#include <filesystem>
#include <thread>
#include <chrono>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

namespace ifs {

// ─── File-scope state for signal / console-ctrl handlers ─────────────────────
namespace {
    std::atomic<bool>* g_running = nullptr;

    void onSignal(int /*sig*/) {
        if (g_running) g_running->store(false);
    }

#ifdef _WIN32
    BOOL WINAPI onConsoleCtrl(DWORD ctrlType) {
        if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_CLOSE_EVENT) {
            if (g_running) g_running->store(false);
            return TRUE;
        }
        return FALSE;
    }
#endif
} // anonymous namespace

// ─── Construction / Destruction ──────────────────────────────────────────────

Application::Application()
    : server_(std::make_unique<WebSocketServer>()) {}

Application::~Application() {
    shutdown();
    g_running = nullptr;
}

// ─── Initialise ──────────────────────────────────────────────────────────────

bool Application::initialize(int argc, char* argv[]) {
    auto& log = Logger::instance();

    // Determine config path (first arg or default)
    std::filesystem::path configPath = "config/config.default.json";
    if (argc > 1)
        configPath = argv[1];

    // Load configuration (logger is at INFO until config overrides it)
    config_.load(configPath);
    log.setLevel(Logger::parseLevel(config_.getLogLevel()));

    log.info("App", "{} v{} initializing...", SERVICE_NAME, SERVICE_VERSION);

    // Wire up OS signal handlers for graceful shutdown
    setupSignalHandlers();

    log.info("App", "Initialization complete");
    return true;
}

// ─── Run ─────────────────────────────────────────────────────────────────────

int Application::run() {
    auto& log = Logger::instance();

    startTime_ = std::chrono::steady_clock::now();

    if (!server_->start(config_.getHost(), config_.getPort())) {
        log.critical("App", "Failed to start WebSocket server on ws://{}:{}",
                     config_.getHost(), config_.getPort());
        return 1;
    }

    // Start periodic heartbeat broadcasting
    server_->startHeartbeat(
        std::chrono::milliseconds(config_.getHeartbeatIntervalMs()),
        [this]() -> std::string {
            auto now = std::chrono::system_clock::now();
            auto ts  = std::chrono::duration_cast<std::chrono::seconds>(
                           now.time_since_epoch()).count();

            nlohmann::json heartbeat = {
                {"event", "heartbeat"},
                {"data", {
                    {"status",         "ok"},
                    {"service",        SERVICE_NAME},
                    {"uptime_seconds", getUptimeSeconds()},
                    {"timestamp",      ts},
                    {"connections",    server_->connectionCount()}
                }}
            };
            return heartbeat.dump();
        });

    running_ = true;
    log.info("App", "{} is running — press Ctrl+C to stop", SERVICE_NAME);

    // Block until shutdown() is called (from a signal handler or elsewhere)
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    log.info("App", "Shutting down...");
    server_->stop();
    log.info("App", "Shutdown complete");
    return 0;
}

// ─── Shutdown ────────────────────────────────────────────────────────────────

void Application::shutdown() {
    running_ = false;
}

// ─── Uptime ──────────────────────────────────────────────────────────────────

double Application::getUptimeSeconds() const {
    auto elapsed = std::chrono::steady_clock::now() - startTime_;
    return std::chrono::duration<double>(elapsed).count();
}

// ─── Signal Handlers ─────────────────────────────────────────────────────────

void Application::setupSignalHandlers() {
    g_running = &running_;

    std::signal(SIGINT,  onSignal);
    std::signal(SIGTERM, onSignal);

#ifdef _WIN32
    SetConsoleCtrlHandler(onConsoleCtrl, TRUE);
#endif
}

} // namespace ifs
