#include "server/WebSocketSession.hpp"
#include "server/ConnectionManager.hpp"
#include "resources/ResourceManager.hpp"
#include "resources/Resource.hpp"
#include "core/Logger.hpp"
#include "core/Types.hpp"

#include <boost/beast/http/field.hpp>
#include <boost/asio/post.hpp>
#include <nlohmann/json.hpp>
#include <chrono>

namespace beast     = boost::beast;
namespace websocket = beast::websocket;
namespace http      = beast::http;
namespace net       = boost::asio;

namespace ifs {

WebSocketSession::WebSocketSession(net::ip::tcp::socket socket,
                                   ConnectionManager& manager,
                                   ResourceManager& resourceManager)
    : ws_(std::move(socket))
    , manager_(manager)
    , resourceManager_(resourceManager) {}

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
        manager_.leave(shared_from_this());
        Logger::instance().info("WebSocket", "Client disconnected (total: {})",
                                manager_.connectionCount());
        return;
    }

    std::string message = beast::buffers_to_string(buffer_.data());
    buffer_.consume(buffer_.size());

    Logger::instance().debug("WebSocket", "[IN] {}", message);

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

        } else if (!action.empty()) {
            Logger::instance().debug("WebSocket", "Unknown action: {}", action);
        }

    } catch (const nlohmann::json::exception& e) {
        Logger::instance().warn("WebSocket", "Invalid message: {}", e.what());
    }
}

// ─── Writing (thread-safe, bounded queue on io_context strand) ───────────────

void WebSocketSession::send(const std::string& message) {
    Logger::instance().debug("WebSocket", "[OUT] {}", message);

    net::post(ws_.get_executor(), [self = shared_from_this(), message]() {
        if (self->writeQueue_.size() >= MAX_QUEUE_SIZE) {
            Logger::instance().warn("WebSocket", "Write queue full, dropping message");
            return;
        }

        self->writeQueue_.push_back(message);

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

    ws_.async_write(
        net::buffer(writeQueue_.front()),
        [self = shared_from_this()](beast::error_code ec, std::size_t bytes) {
            self->onWrite(ec, bytes);
        });
}

void WebSocketSession::onWrite(beast::error_code ec, std::size_t /*bytes*/) {
    if (ec) {
        Logger::instance().debug("WebSocket", "Write error: {}", ec.message());
        manager_.leave(shared_from_this());
        return;
    }

    writeQueue_.pop_front();
    doWrite();
}

} // namespace ifs
