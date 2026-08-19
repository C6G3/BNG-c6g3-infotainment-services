#include "server/WebSocketSession.hpp"
#include "server/ConnectionManager.hpp"
#include "core/Logger.hpp"
#include "core/Types.hpp"

#include <boost/beast/http/field.hpp>
#include <nlohmann/json.hpp>
#include <chrono>

namespace beast     = boost::beast;
namespace websocket = beast::websocket;
namespace http      = beast::http;
namespace net       = boost::asio;

namespace ifs {

WebSocketSession::WebSocketSession(net::ip::tcp::socket socket,
                                   ConnectionManager& manager)
    : ws_(std::move(socket))
    , manager_(manager) {}

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
        // Normal close and shutdown cancellation are not errors
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

        } else if (!action.empty()) {
            Logger::instance().debug("WebSocket", "Unknown action: {}", action);
        }

    } catch (const nlohmann::json::exception& e) {
        Logger::instance().warn("WebSocket", "Invalid message: {}", e.what());
    }
}

// ─── Writing (bounded queue, one async_write at a time) ──────────────────────

void WebSocketSession::send(const std::string& message) {
    if (writeQueue_.size() >= MAX_QUEUE_SIZE) {
        Logger::instance().warn("WebSocket", "Write queue full, dropping message");
        return;
    }

    writeQueue_.push_back(message);

    if (!writing_)
        doWrite();
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
