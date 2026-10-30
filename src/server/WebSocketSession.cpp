#include "server/WebSocketSession.hpp"
#include "server/ConnectionManager.hpp"
#include "resources/ResourceManager.hpp"
#include "resources/Resource.hpp"
#include "tts/TtsService.hpp"
#include "core/Logger.hpp"
#include "core/Types.hpp"
#include "core/UserData.hpp"

#include <boost/beast/http/field.hpp>
#include <boost/asio/post.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <thread>

namespace beast     = boost::beast;
namespace websocket = beast::websocket;
namespace http      = beast::http;
namespace net       = boost::asio;

namespace ifs {

namespace {
// The web apps' browser profiles (Chrome / Edge, Brave): every sign-in, cookie and cache of the web apps. Their browser
// is closed first by the caller; its last processes may hold files a moment longer. True when both are gone
bool deleteWebAppProfiles() {
    bool ok = true;
    for (const char* name : {"profile", "profile_brave"}) {
        const auto profile = userDataDir() / "webapp" / name;
        std::error_code ec;
        for (int i = 0; i < 20 && std::filesystem::exists(profile, ec); i++) {
            std::filesystem::remove_all(profile, ec);
            if (ec) std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        if (std::filesystem::exists(profile, ec)) {
            ok = false;
            Logger::instance().warn("WebSocket", "The web app profile '{}' could not be deleted: {}", name, ec.message());
        }
    }
    return ok;
}

// The web app picture's codec from the screen (Settings > Display): one of the known ones, else VP9 (the default)
std::string pictureCodec(const nlohmann::json& j) {
    const std::string c = j.value("codec", "");
    return (c == "vp9" || c == "h264" || c == "vp8" || c == "av1") ? c : "vp9";
}
} // namespace

WebSocketSession::WebSocketSession(net::ip::tcp::socket socket,
                                   ConnectionManager& manager,
                                   ResourceManager& resourceManager,
                                   TtsService& ttsService)
    : ws_(std::move(socket))
    , manager_(manager)
    , resourceManager_(resourceManager)
    , ttsService_(ttsService) {}

WebSocketSession::~WebSocketSession() = default;

// ─── Lifecycle ───────────────────────────────────────────────────────────────

void WebSocketSession::run() {
    ws_.set_option(websocket::stream_base::timeout::suggested(
        beast::role_type::server));

    ws_.set_option(websocket::stream_base::decorator(
        [](websocket::response_type& res) {
            res.set(http::field::server, SERVICE_NAME);
        }));

    ws_.async_accept(
        [self = shared_from_this()](beast::error_code ec) {
            self->onAccept(ec);
        });
}

void WebSocketSession::onAccept(beast::error_code ec) {
    if (ec) {
        Logger::instance().debug("WebSocket", "Handshake failed: {}", ec.message());
        return;
    }

    manager_.join(shared_from_this());
    open_ = true;

    Logger::instance().info("WebSocket", "Client connected (total: {})",
                            manager_.connectionCount());

    // Send welcome event immediately
    nlohmann::json welcome = {
        {"event", "welcome"},
        {"data", {
            {"service", SERVICE_NAME},
            {"version", SERVICE_VERSION}
        }}
    };
    send(welcome.dump());

    doRead();
}

// ─── Reading ─────────────────────────────────────────────────────────────────

void WebSocketSession::doRead() {
    ws_.async_read(buffer_,
        [self = shared_from_this()](beast::error_code ec, std::size_t bytes) {
            self->onRead(ec, bytes);
        });
}

void WebSocketSession::onRead(beast::error_code ec, std::size_t /*bytes*/) {
    if (ec) {
        if (ec != websocket::error::closed &&
            ec != net::error::operation_aborted) {
            Logger::instance().debug("WebSocket", "Read error: {}", ec.message());
        }
        open_ = false;
        manager_.leave(shared_from_this());
        Logger::instance().info("WebSocket", "Client disconnected (total: {})",
                                manager_.connectionCount());
        return;
    }

    std::string message = beast::buffers_to_string(buffer_.data());
    buffer_.consume(buffer_.size());

    // (the app lock's messages carry a password: never logged)
    if (message.find("\"APPLOCK_") != std::string::npos) Logger::instance().debug("WebSocket", "[IN] app lock message");
    else Logger::instance().debug("WebSocket", "[IN] {}", message);

    onMessage(message);
    doRead();
}

// ─── Message Handling ────────────────────────────────────────────────────────

void WebSocketSession::onMessage(const std::string& message) {
    try {
        auto json   = nlohmann::json::parse(message);
        auto action = json.value("action", "");

        if (action == "ping") {
            auto now = std::chrono::system_clock::now();
            auto ts  = std::chrono::duration_cast<std::chrono::seconds>(
                           now.time_since_epoch()).count();

            nlohmann::json pong = {
                {"event", "pong"},
                {"data",  {{"timestamp", ts}}}
            };
            send(pong.dump());

        } else if (action == "SCAN_EXTERNAL_MEDIA") {
            ScanMediaRequest req;
            req.requestId          = json.value("requestId", "");
            req.targetPath         = json.value("targetPath", "");
            req.category           = json.value("category", "");
            req.outputThumbnailDir = json.value("outputThumbnailDir", "");
            req.maxThumbnailWidth  = json.value("maxThumbnailWidth", 256);
            req.maxThumbnailHeight = json.value("maxThumbnailHeight", 256);

            if (json.contains("mediaTypes") && json["mediaTypes"].is_array()) {
                req.mediaTypes = json["mediaTypes"].get<std::vector<std::string>>();
            }
            if (json.contains("extensions") && json["extensions"].is_array()) {
                req.extensions = json["extensions"].get<std::vector<std::string>>();
            }

            std::string mediaTypesStr;
            for (size_t i = 0; i < req.mediaTypes.size(); ++i) {
                if (i > 0) mediaTypesStr += ", ";
                mediaTypesStr += req.mediaTypes[i];
            }

            Logger::instance().info("WebSocket", "Processing SCAN_EXTERNAL_MEDIA: target='{}', mediaTypes=[{}], reqId='{}'",
                                    req.targetPath, mediaTypesStr, req.requestId);

            // Progress callback forwarding OPERATION_PROGRESS events to the client
            auto onProgress = [weakSelf = weak_from_this()](const OperationProgress& prog) {
                if (auto self = weakSelf.lock()) {
                    self->send(prog.toJson().dump());
                }
            };

            // Execute scan on background worker thread
            resourceManager_.scanExternalMediaAsync(req, [weakSelf = weak_from_this()](const ScanMediaResult& result) {
                if (auto self = weakSelf.lock()) {
                    self->send(result.toJson().dump());
                }
            }, onProgress);

        } else if (action == "CACHE_EXTERNAL_MEDIA") {
            CacheMediaRequest req;
            req.requestId  = json.value("requestId", "");
            req.sourcePath = json.value("sourcePath", "");
            req.targetDir  = json.value("targetDir", "");
            req.filename   = json.value("filename", "");
            req.mediaType  = json.value("mediaType", "");

            Logger::instance().info("WebSocket", "Processing CACHE_EXTERNAL_MEDIA: src='{}', targetDir='{}', mediaType='{}', reqId='{}'",
                                    req.sourcePath, req.targetDir, req.mediaType, req.requestId);

            // Progress callback forwarding OPERATION_PROGRESS events to the client
            auto onProgress = [weakSelf = weak_from_this()](const OperationProgress& prog) {
                if (auto self = weakSelf.lock()) {
                    self->send(prog.toJson().dump());
                }
            };

            // Execute file caching on background worker thread
            resourceManager_.cacheExternalMediaAsync(req, [weakSelf = weak_from_this()](const CacheMediaResult& result) {
                if (auto self = weakSelf.lock()) {
                    self->send(result.toJson().dump());
                }
            }, onProgress);

        } else if (action == "VIDEO_STREAM_OPEN") {
            VideoStreamOpenRequest req;
            req.requestId  = json.value("requestId", "");
            req.sourcePath = json.value("sourcePath", "");
            req.targetDir  = json.value("targetDir", "");
            req.startTime  = json.value("startTime", 0.0);

            Logger::instance().info("WebSocket", "Processing VIDEO_STREAM_OPEN: src='{}', targetDir='{}', start={:.1f}s, reqId='{}'",
                                    req.sourcePath, req.targetDir, req.startTime, req.requestId);

            VideoStreamer::Sink sink;
            sink.send = [weakSelf = weak_from_this()](const std::string& msg) {
                if (auto self = weakSelf.lock()) self->send(msg);
            };
            sink.isConnected = [weakSelf = weak_from_this()]() {
                auto self = weakSelf.lock();
                return self && self->isOpen();
            };
            resourceManager_.videoStreamer().open(req, std::move(sink));

        } else if (action == "VIDEO_STREAM_SEEK") {
            resourceManager_.videoStreamer().seek(json.value("requestId", ""), json.value("time", 0.0));

        } else if (action == "VIDEO_STREAM_POSITION") {
            resourceManager_.videoStreamer().position(json.value("requestId", ""), json.value("time", 0.0));

        } else if (action == "VIDEO_STREAM_CLOSE") {
            Logger::instance().info("WebSocket", "Processing VIDEO_STREAM_CLOSE: reqId='{}'", json.value("requestId", ""));
            resourceManager_.videoStreamer().close(json.value("requestId", ""));

        } else if (action == "RADIO_STREAM_OPEN") {
            RadioStreamOpenRequest req;
            req.requestId      = json.value("requestId", "");
            req.url            = json.value("url", "");
            req.targetDir      = json.value("targetDir", "");
            req.maxBufferBytes = static_cast<int64_t>(json.value("maxBufferMB", 4.0) * 1024.0 * 1024.0);
            req.bitRate        = json.value("bitRate", static_cast<int64_t>(96000));

            Logger::instance().info("WebSocket", "Processing RADIO_STREAM_OPEN: url='{}', targetDir='{}', reqId='{}'",
                                    req.url, req.targetDir, req.requestId);

            RadioStreamer::Sink sink;
            sink.send = [weakSelf = weak_from_this()](const std::string& msg) {
                if (auto self = weakSelf.lock()) self->send(msg);
            };
            sink.isConnected = [weakSelf = weak_from_this()]() {
                auto self = weakSelf.lock();
                return self && self->isOpen();
            };
            resourceManager_.radioStreamer().open(req, std::move(sink));

        } else if (action == "RADIO_STREAM_KEEPALIVE") {
            resourceManager_.radioStreamer().keepAlive(json.value("requestId", ""));

        } else if (action == "RADIO_STREAM_CLOSE") {
            Logger::instance().info("WebSocket", "Processing RADIO_STREAM_CLOSE: reqId='{}'", json.value("requestId", ""));
            resourceManager_.radioStreamer().close(json.value("requestId", ""));

        } else if (action == "WEBRTC_PROBE_START") {
            WebRtcProbe::Sink sink;
            sink.send = [weakSelf = weak_from_this()](const std::string& msg) {
                if (auto self = weakSelf.lock()) self->send(msg);
            };
            sink.isConnected = [weakSelf = weak_from_this()]() {
                auto self = weakSelf.lock();
                return self && self->isOpen();
            };
            const bool audioOnly = json.value("audioOnly", false);
            Logger::instance().info("WebSocket", "Processing WEBRTC_PROBE_START: reqId='{}'{}", json.value("requestId", ""), audioOnly ? " (sound only)" : "");
            resourceManager_.webRtcProbe().start(json.value("requestId", ""), std::move(sink), audioOnly);

        } else if (action == "WEBRTC_PROBE_ANSWER") {
            resourceManager_.webRtcProbe().answer(json.value("requestId", ""), json.value("sdp", ""));

        } else if (action == "WEBRTC_PROBE_STOP") {
            resourceManager_.webRtcProbe().stop(json.value("requestId", ""));

        } else if (action == "WEBAPP_OPEN" || action == "WEBAPP_SOUND_ATTACH") {
            WebAppHost::Sink sink;
            sink.send = [weakSelf = weak_from_this()](const std::string& msg) {
                if (auto self = weakSelf.lock()) self->send(msg);
            };
            sink.isConnected = [weakSelf = weak_from_this()]() {
                auto self = weakSelf.lock();
                return self && self->isOpen();
            };
            const std::string reqId = json.value("requestId", "");
            if (action == "WEBAPP_OPEN") {
                const std::string url = json.value("url", "");
                Logger::instance().info("WebSocket", "Processing WEBAPP_OPEN: url='{}', reqId='{}'", url, reqId);
                // A locked app (security/AppLock.hpp) starts only with an unlock token; the one already running
                // (a reloaded screen takes it back) stays unlocked until it closes
                const std::string running = resourceManager_.webAppHost().runningUrl();
                const bool reopening = !running.empty() && AppLock::appIdForUrl(running) == AppLock::appIdForUrl(url);
                // The browser's desktop window (WEBAPP_DESKTOP: signing in) holds the profile until it is closed
                if (resourceManager_.webAppHost().desktopWindowOpen()) {
                    send(nlohmann::json{{"event", "WEBAPP_ERROR"}, {"requestId", reqId}, {"desktop", true},
                                        {"error", "The browser is open on your desktop. Close that window, then open the app again."}}.dump());
                    return;
                }
                if (!reopening && !resourceManager_.appLock().mayOpen(url, json.value("unlock", ""))) {
                    Logger::instance().warn("WebSocket", "WEBAPP_OPEN refused: '{}' is locked", url);
                    send(nlohmann::json{{"event", "WEBAPP_ERROR"}, {"requestId", reqId}, {"locked", true},
                                        {"error", "This app is locked."}}.dump());
                    return;
                }
                resourceManager_.webAppHost().open(reqId, url, json.value("titleMatch", ""), json.value("siteMode", ""), json.value("height", 0), json.value("chromeFirst", false), json.value("fill", ""), json.value("press", ""), json.value("pointer", "") == "mouse", json.value("incognito", false), std::move(sink));
                resourceManager_.webAppHost().picture(reqId, pictureCodec(json), json.value("textHint", true), false);
            } else {
                Logger::instance().info("WebSocket", "Processing WEBAPP_SOUND_ATTACH: reqId='{}'", reqId);
                resourceManager_.webAppHost().attachSound(reqId, std::move(sink));
            }

        } else if (action == "WEBAPP_ANSWER") {
            resourceManager_.webAppHost().answer(json.value("requestId", ""), json.value("to", ""), json.value("sdp", ""));

        } else if (action == "WEBAPP_INPUT") {
            resourceManager_.webAppHost().input(json.value("requestId", ""), json.dump());

        } else if (action == "WEBAPP_NAV") {
            // (no way into a locked app through another one's address bar)
            if (json.value("nav", "") == "url" && resourceManager_.appLock().isLockedUrl(json.value("url", ""))) {
                Logger::instance().warn("WebSocket", "WEBAPP_NAV refused: '{}' is locked", json.value("url", ""));
                return;
            }
            resourceManager_.webAppHost().navigate(json.value("requestId", ""), json.value("nav", ""), json.value("url", ""));

        } else if (action.rfind("APPLOCK_", 0) == 0) {
            // The app lock (security/AppLock.hpp): hashing a password takes ~0.5 s, so off this connection's thread
            const std::string reqId = json.value("requestId", "");
            ResourceManager& rm = resourceManager_;
            rm.runAsync([&rm, action, json, reqId, weakSelf = weak_from_this()]() {
                AppLock& lock = rm.appLock();
                const std::string password = json.value("password", "");
                const auto locked = json.contains("locked") && json["locked"].is_array()
                    ? json["locked"].get<std::vector<std::string>>() : std::vector<std::string>{};
                nlohmann::json r;
                if (action == "APPLOCK_STATUS") r = lock.status();
                else if (action == "APPLOCK_SETUP") r = lock.setup(password, locked);
                else if (action == "APPLOCK_UNLOCK") r = lock.unlock(password, json.value("appId", ""));
                else if (action == "APPLOCK_CHANGE") r = lock.change(password, json.value("newPassword", ""));
                else if (action == "APPLOCK_SET_LOCKED") r = lock.setLocked(password, locked);
                else if (action == "APPLOCK_RESET") {
                    // Signs the web apps out: their browser (closed first) and its profiles (Chrome / Edge, Brave)
                    // go with the password
                    rm.webAppHost().closeAll();
                    deleteWebAppProfiles();
                    r = lock.reset();
                } else r = {{"ok", false}, {"error", "Unknown app lock action"}};
                r["event"] = "APPLOCK_RESULT";
                r["requestId"] = reqId;
                r["action"] = action;
                if (auto self = weakSelf.lock()) self->send(r.dump());
            });

        } else if (action == "WEBAPP_CLEAR_DATA") {
            // Settings > Clear browser data: the web apps' browser profiles go (every sign-in, cookie, cache and saved
            // game progress of YouTube, Spotify, Discord, Poki...). The running web app closes first; the desktop
            // window (WEBAPP_DESKTOP) holds the profile, so it has to be closed by the user
            const std::string reqId = json.value("requestId", "");
            ResourceManager& rm = resourceManager_;
            rm.runAsync([&rm, reqId, weakSelf = weak_from_this()]() {
                nlohmann::json r{{"event", "WEBAPP_CLEAR_DATA_RESULT"}, {"requestId", reqId}};
                if (rm.webAppHost().desktopWindowOpen()) {
                    r["ok"] = false;
                    r["error"] = "Close the browser window on your desktop first.";
                } else {
                    rm.webAppHost().closeAll();
                    const bool ok = deleteWebAppProfiles();
                    r["ok"] = ok;
                    if (!ok) r["error"] = "Some files are in use. Close the web app and try again.";
                    Logger::instance().info("WebSocket", "WEBAPP_CLEAR_DATA: {}", ok ? "the web app profiles are deleted" : "some files stayed");
                }
                if (auto self = weakSelf.lock()) self->send(r.dump());
            });

        } else if (action == "WEBAPP_DESKTOP") {
            // A visible window of the web apps' browser on the desktop (signing in where the hidden browser is refused:
            // Google, Spotify). Closing the running web app waits for its browser, so off this connection's thread.
            // A locked app's site only while that app runs (it was unlocked to open): its sign-in page moved here.
            // Discord's invite links always: the launcher's Discord tile opens the invite here (Discord itself no
            // longer runs in the vehicle, 2026-10-30), and Discord was locked by default
            const std::string reqId = json.value("requestId", "");
            const std::string url = json.value("url", "");
            const bool chromeFirst = json.value("chromeFirst", false);
            ResourceManager& rm = resourceManager_;
            const std::string running = rm.webAppHost().runningUrl();
            const bool invite = url.rfind("https://discord.gg/", 0) == 0 || url.rfind("https://discord.com/invite/", 0) == 0;
            const bool locked = !invite && rm.appLock().isLockedUrl(url) &&
                                !(!running.empty() && AppLock::appIdForUrl(running) == AppLock::appIdForUrl(url));
            if (url.rfind("https://", 0) != 0 || locked) {
                send(nlohmann::json{{"event", "WEBAPP_DESKTOP_RESULT"}, {"requestId", reqId}, {"ok", false}, {"error", "Not allowed"}}.dump());
                return;
            }
            rm.runAsync([&rm, reqId, url, chromeFirst, weakSelf = weak_from_this()]() {
                bool focused = false;
                const std::string error = rm.webAppHost().openOnDesktop(url, chromeFirst, &focused);
                nlohmann::json r{{"event", "WEBAPP_DESKTOP_RESULT"}, {"requestId", reqId}, {"ok", error.empty()}, {"focused", focused}};
                if (!error.empty()) r["error"] = error;
                if (auto self = weakSelf.lock()) self->send(r.dump());
            });

        } else if (action == "WEBAPP_PICTURE") {
            // Settings > Display changed while the app runs: a new codec starts its picture links again
            resourceManager_.webAppHost().picture(json.value("requestId", ""), pictureCodec(json), json.value("textHint", true), true);

        } else if (action == "WEBAPP_CAST") {
            resourceManager_.webAppHost().cast(json.value("requestId", ""), json.value("screen", ""), json.value("on", false));

        } else if (action == "WEBAPP_CLOSE") {
            Logger::instance().info("WebSocket", "Processing WEBAPP_CLOSE: reqId='{}'", json.value("requestId", ""));
            resourceManager_.webAppHost().close(json.value("requestId", ""));

        } else if (action == "AUX_START") {
            AuxCapture::Sink sink;
            sink.send = [weakSelf = weak_from_this()](const std::string& msg) {
                if (auto self = weakSelf.lock()) self->send(msg);
            };
            sink.isConnected = [weakSelf = weak_from_this()]() {
                auto self = weakSelf.lock();
                return self && self->isOpen();
            };
            Logger::instance().info("WebSocket", "Processing AUX_START: reqId='{}', input='{}'", json.value("requestId", ""), json.value("deviceLabel", ""));
            resourceManager_.auxCapture().start(json.value("requestId", ""), json.value("deviceLabel", ""), std::move(sink));

        } else if (action == "AUX_ANSWER") {
            resourceManager_.auxCapture().answer(json.value("requestId", ""), json.value("sdp", ""));

        } else if (action == "AUX_STOP") {
            Logger::instance().info("WebSocket", "Processing AUX_STOP: reqId='{}'", json.value("requestId", ""));
            resourceManager_.auxCapture().stop(json.value("requestId", ""));

        } else if (action == "RADIO_DISCOVER" || action == "RADIO_SAVE_LOGO") {
            RadioDiscovery::Sink sink;
            sink.send = [weakSelf = weak_from_this()](const std::string& msg) {
                if (auto self = weakSelf.lock()) self->send(msg);
            };
            sink.isConnected = [weakSelf = weak_from_this()]() {
                auto self = weakSelf.lock();
                return self && self->isOpen();
            };
            const std::string reqId = json.value("requestId", "");
            const std::string url   = json.value("url", "");
            Logger::instance().info("WebSocket", "Processing {}: url='{}', reqId='{}'", action, url, reqId);
            if (action == "RADIO_DISCOVER") {
                resourceManager_.radioDiscovery().discover(reqId, url, std::move(sink));
            } else {
                resourceManager_.radioDiscovery().saveLogo(reqId, url, json.value("targetDir", ""), json.value("name", ""), std::move(sink));
            }

        } else if (action == "CONVERT_IMAGE") {
            ConvertImageRequest req;
            req.requestId       = json.value("requestId", "");
            req.sourcePath      = json.value("sourcePath", "");
            req.destinationPath = json.value("destinationPath", "");
            req.targetFormat    = json.value("targetFormat", "webp");
            req.quality         = json.value("quality", 88);
            req.lossless        = json.value("lossless", false);
            req.level           = json.value("level", "");
            req.keepSource      = json.value("keepSource", true);
            if (json.contains("tiles") && json["tiles"].is_array()) {
                for (const auto& tile : json["tiles"]) {
                    if (tile.is_string()) req.tiles.push_back(tile.get<std::string>());
                }
                req.tileColumns = json.value("tileColumns", 0);
            }

            Logger::instance().info("WebSocket", "Processing CONVERT_IMAGE: src='{}', dst='{}', targetFormat='{}', quality={}, lossless={}, level='{}', keepSource={}, tiles={} ({} per row), reqId='{}'",
                                    req.sourcePath, req.destinationPath, req.targetFormat, req.quality, req.lossless, req.level, req.keepSource, req.tiles.size(), req.tileColumns, req.requestId);

            // Progress callback forwarding OPERATION_PROGRESS events to the client
            auto onProgress = [weakSelf = weak_from_this()](const OperationProgress& prog) {
                if (auto self = weakSelf.lock()) {
                    self->send(prog.toJson().dump());
                }
            };

            // Execute image conversion on background worker thread
            resourceManager_.convertImageAsync(req, [weakSelf = weak_from_this()](const ConvertImageResult& result) {
                if (auto self = weakSelf.lock()) {
                    self->send(result.toJson().dump());
                }
            }, onProgress);

        } else if (action == "TTS_LIST_VOICES") {
            auto requestId = json.value("requestId", "");
            Logger::instance().info("WebSocket", "Processing TTS_LIST_VOICES: reqId='{}'", requestId);
            send(ttsService_.listVoices(requestId).toJson().dump());

        } else if (action == "TTS_SPEAK") {
            SpeakRequest req;
            req.requestId = json.value("requestId", "");
            req.text      = json.value("text", "");
            req.voice     = json.value("voice", "");
            req.speed     = json.value("speed", 1.0f);

            Logger::instance().info("WebSocket", "Processing TTS_SPEAK: voice='{}', speed={}, chars={}, reqId='{}'",
                                    req.voice, req.speed, req.text.size(), req.requestId);

            ttsService_.speak(std::move(req), makeTtsSink());

        } else if (action == "TTS_PRELOAD") {
            auto requestId = json.value("requestId", "");
            Logger::instance().info("WebSocket", "Processing TTS_PRELOAD: reqId='{}'", requestId);
            ttsService_.preload(std::move(requestId), makeTtsSink());

        } else if (action == "TTS_STOP") {
            ttsService_.cancel(this, json.value("requestId", ""));

        } else if (!action.empty()) {
            Logger::instance().debug("WebSocket", "Unknown action: {}", action);
        }

    } catch (const nlohmann::json::exception& e) {
        Logger::instance().warn("WebSocket", "Invalid message: {}", e.what());
    }
}

// Audio and events go back to this session; the TTS service holds only weak references
TtsService::Sink WebSocketSession::makeTtsSink() {
    TtsService::Sink sink;
    sink.owner    = this;
    sink.sendText = [weakSelf = weak_from_this()](const std::string& msg) {
        if (auto self = weakSelf.lock()) self->send(msg);
    };
    sink.sendBinary = [weakSelf = weak_from_this()](std::string frame) {
        if (auto self = weakSelf.lock()) self->sendBinary(std::move(frame));
    };
    sink.isConnected = [weakSelf = weak_from_this()]() {
        auto self = weakSelf.lock();
        return self && self->isOpen();
    };
    return sink;
}

// ─── Writing (thread-safe, bounded queue on io_context strand) ───────────────

void WebSocketSession::send(const std::string& message) {
    Logger::instance().debug("WebSocket", "[OUT] {}", message);
    enqueue(message, false);
}

void WebSocketSession::sendBinary(std::string frame) {
    Logger::instance().trace("WebSocket", "[OUT] binary frame, {} bytes", frame.size());
    enqueue(std::move(frame), true);
}

void WebSocketSession::enqueue(std::string payload, bool binary) {
    net::post(ws_.get_executor(), [self = shared_from_this(), payload = std::move(payload), binary]() mutable {
        // Text and audio have separate limits so a long speech stream cannot crowd out events
        if (!binary && self->queuedText_ >= MAX_QUEUE_SIZE) {
            Logger::instance().warn("WebSocket", "Write queue full, dropping message");
            return;
        }
        if (binary && self->queuedBinaryBytes_ + payload.size() > MAX_BINARY_QUEUE_BYTES) {
            Logger::instance().warn("WebSocket", "Audio queue full ({} bytes pending), dropping a {} byte frame",
                                    self->queuedBinaryBytes_, payload.size());
            return;
        }

        if (binary) self->queuedBinaryBytes_ += payload.size();
        else        ++self->queuedText_;
        self->writeQueue_.push_back(OutgoingMessage{std::move(payload), binary});

        if (!self->writing_) {
            self->doWrite();
        }
    });
}

void WebSocketSession::doWrite() {
    if (writeQueue_.empty()) {
        writing_ = false;
        return;
    }

    writing_ = true;

    ws_.binary(writeQueue_.front().binary);
    ws_.async_write(
        net::buffer(writeQueue_.front().payload),
        [self = shared_from_this()](beast::error_code ec, std::size_t bytes) {
            self->onWrite(ec, bytes);
        });
}

void WebSocketSession::onWrite(beast::error_code ec, std::size_t /*bytes*/) {
    if (ec) {
        Logger::instance().debug("WebSocket", "Write error: {}", ec.message());
        open_ = false;
        manager_.leave(shared_from_this());
        return;
    }

    const auto& sent = writeQueue_.front();
    if (sent.binary) queuedBinaryBytes_ -= sent.payload.size();
    else             --queuedText_;
    writeQueue_.pop_front();
    doWrite();
}

} // namespace ifs
