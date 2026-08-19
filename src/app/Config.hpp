#pragma once

#include <string>
#include <cstdint>
#include <filesystem>

namespace ifs {

class Config {
public:
    bool load(const std::filesystem::path& path);
    void loadDefaults();

    const std::string& getHost()               const { return host_; }
    uint16_t           getPort()               const { return port_; }
    uint32_t           getHeartbeatIntervalMs() const { return heartbeatIntervalMs_; }
    const std::string& getLogLevel()           const { return logLevel_; }

private:
    std::string host_               = "127.0.0.1";
    uint16_t    port_               = 38123;
    uint32_t    heartbeatIntervalMs_ = 2500;
    std::string logLevel_           = "INFO";
};

} // namespace ifs
