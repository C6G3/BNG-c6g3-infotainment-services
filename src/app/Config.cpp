#include "app/Config.hpp"
#include "core/Logger.hpp"

#include <nlohmann/json.hpp>
#include <fstream>

namespace ifs {

bool Config::load(const std::filesystem::path& path) {
    auto& log = Logger::instance();

    if (!std::filesystem::exists(path)) {
        log.warn("Config", "Configuration file not found: {}, using defaults",
                 path.string());
        loadDefaults();
        return true;
    }

    try {
        std::ifstream file(path);
        if (!file.is_open()) {
            log.error("Config", "Failed to open configuration file: {}",
                      path.string());
            loadDefaults();
            return false;
        }

        auto json = nlohmann::json::parse(file);

        if (json.contains("server")) {
            auto& server = json["server"];
            if (server.contains("host"))
                host_ = server["host"].get<std::string>();
            if (server.contains("port"))
                port_ = server["port"].get<uint16_t>();
            if (server.contains("heartbeat_interval_ms"))
                heartbeatIntervalMs_ = server["heartbeat_interval_ms"].get<uint32_t>();
        }

        if (json.contains("logging")) {
            auto& logging = json["logging"];
            if (logging.contains("level"))
                logLevel_ = logging["level"].get<std::string>();
        }

        log.info("Config", "Loaded configuration from {}", path.string());
        log.info("Config", "  Server: ws://{}:{}", host_, port_);
        log.info("Config", "  Heartbeat interval: {}ms", heartbeatIntervalMs_);
        log.info("Config", "  Log level: {}", logLevel_);

        return true;

    } catch (const nlohmann::json::exception& e) {
        log.error("Config", "Failed to parse configuration: {}", e.what());
        loadDefaults();
        return false;
    }
}

void Config::loadDefaults() {
    host_               = "127.0.0.1";
    port_               = 38123;
    heartbeatIntervalMs_ = 2500;
    logLevel_           = "INFO";

    Logger::instance().info("Config", "Using default configuration (ws://{}:{})",
                            host_, port_);
}

} // namespace ifs
