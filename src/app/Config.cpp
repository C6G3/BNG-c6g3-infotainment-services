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

        if (json.contains("process")) {
            auto& process = json["process"];
            if (process.contains("priority"))
                processPriority_ = process["priority"].get<std::string>();
        }

        if (json.contains("progress")) {
            auto& progress = json["progress"];
            if (progress.contains("min_file_size_bytes"))
                progressConfig_.minFileSizeBytes = progress["min_file_size_bytes"].get<uint64_t>();
            if (progress.contains("min_scan_items"))
                progressConfig_.minScanItems = progress["min_scan_items"].get<uint32_t>();
        }

        // Relative assistant paths resolve against the folder that contains config/
        // (the repo root in development, the dist folder in a release).
        loadAssistant(json.value("assistant", nlohmann::json::object()),
                      std::filesystem::absolute(path).parent_path().parent_path());

        log.info("Config", "Loaded configuration from {}", path.string());
        log.info("Config", "  Server: ws://{}:{}", host_, port_);
        log.info("Config", "  Heartbeat interval: {}ms", heartbeatIntervalMs_);
        log.info("Config", "  Log level: {}", logLevel_);
        log.info("Config", "  Process priority: {}", processPriority_);
        log.info("Config", "  Progress thresholds: min_file_size_bytes={}, min_scan_items={}",
                 progressConfig_.minFileSizeBytes, progressConfig_.minScanItems);
        log.info("Config", "  TTS engine: {}", ttsConfig_.engineDir.string());
        log.info("Config", "  TTS voices: {} (default {}, {} threads)",
                 ttsConfig_.voicesDir.string(), ttsConfig_.defaultVoice, ttsConfig_.numThreads);

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
    logLevel_           = "DEBUG";
    progressConfig_     = ProgressConfig{};
    loadAssistant(nlohmann::json::object(), std::filesystem::current_path());

    Logger::instance().info("Config", "Using default configuration (ws://{}:{})",
                            host_, port_);
}

void Config::loadAssistant(const nlohmann::json& assistant, const std::filesystem::path& baseDir) {
    // The assistant folder sits next to config/ in dist. A development build runs from build/ with a
    // copied config/, while assistant/ stays at the repo root one level up, so look there too.
    std::filesystem::path rootName = assistant.value("root", std::string("assistant"));
    auto root = baseDir / rootName;
    std::error_code ec;
    if (rootName.is_relative() && !std::filesystem::is_directory(root, ec)) {
        auto parentRoot = baseDir.parent_path() / rootName;
        if (std::filesystem::is_directory(parentRoot, ec)) {
            Logger::instance().info("Config", "  Assistant folder not next to config/; using {}", parentRoot.string());
            root = parentRoot;
        } else {
            Logger::instance().warn("Config", "  Assistant folder not found in {} or {}",
                                    root.string(), parentRoot.string());
        }
    }
    auto tts  = assistant.value("tts", nlohmann::json::object());

    TtsConfig defaults;
    ttsConfig_.engineDir    = root / tts.value("engine_dir", std::string("voice-engine/kokoro-multi-lang-v1_0"));
    ttsConfig_.voicesDir    = root / tts.value("voices_dir", std::string("voices"));
    ttsConfig_.cacheDir     = root / tts.value("cache_dir",  std::string(".cache"));
    ttsConfig_.defaultVoice = tts.value("default_voice",  defaults.defaultVoice);
    ttsConfig_.numThreads   = tts.value("num_threads",    defaults.numThreads);
    ttsConfig_.maxTextChars = tts.value("max_text_chars", defaults.maxTextChars);
    ttsConfig_.chunkSamples = tts.value("chunk_samples",  defaults.chunkSamples);
    ttsConfig_.preload      = tts.value("preload",        defaults.preload);
    ttsConfig_.pauseMs      = tts.value("pause_ms",       defaults.pauseMs);
}

} // namespace ifs
