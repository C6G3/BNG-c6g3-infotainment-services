#pragma once

#include <string>
#include <cstdint>
#include <memory>
#include <functional>
#include <chrono>

namespace ifs {

class ResourceManager;
class TtsService;

/// WebSocket server built on Boost.Beast + Boost.Asio.
/// All Beast/Asio types are hidden behind PIMPL.
class WebSocketServer {
public:
    WebSocketServer(ResourceManager& resourceManager, TtsService& ttsService);
    ~WebSocketServer();

    WebSocketServer(const WebSocketServer&)            = delete;
    WebSocketServer& operator=(const WebSocketServer&) = delete;

    /// Start listening for WebSocket connections on host:port.
    bool start(const std::string& host, uint16_t port);

    /// Stop the server, cancel timers, and join the worker thread.
    void stop();

    /// Send a text message to all connected clients.
    void broadcast(const std::string& message);

    /// Number of active WebSocket connections.
    std::size_t connectionCount() const;

    /// Start periodic heartbeat broadcasting.
    using HeartbeatProvider = std::function<std::string()>;
    void startHeartbeat(std::chrono::milliseconds interval,
                        HeartbeatProvider provider);

private:
    void scheduleNextHeartbeat();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ifs
