#pragma once

#include "resources/Resource.hpp"
#include "tts/TtsTypes.hpp"
#include <string>
#include <cstdint>
#include <filesystem>

namespace ifs {

class Config {
public:
    bool load(const std::filesystem::path& path);
    void loadDefaults();

    const std::string&    getHost()                const { return host_; }
    uint16_t              getPort()                const { return port_; }
    uint32_t              getHeartbeatIntervalMs() const { return heartbeatIntervalMs_; }
    const std::string&    getLogLevel()            const { return logLevel_; }
    const ProgressConfig& getProgressConfig()      const { return progressConfig_; }
    const TtsConfig&      getTtsConfig()           const { return ttsConfig_; }
    const std::string&    getProcessPriority()     const { return processPriority_; }

private:
    /// Applies the "assistant" section; relative paths resolve against `baseDir`.
    void loadAssistant(const nlohmann::json& assistant, const std::filesystem::path& baseDir);

    std::string    host_               = "127.0.0.1";
    uint16_t       port_               = 38123;
    uint32_t       heartbeatIntervalMs_ = 2500;
    std::string    logLevel_           = "INFO";
    ProgressConfig progressConfig_;
    TtsConfig      ttsConfig_;
    std::string    processPriority_    = "below_normal";  // yields the CPU to BeamNG
};

} // namespace ifs
