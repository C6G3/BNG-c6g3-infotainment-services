#pragma once

#include <string>
#include <cstdint>
#include <memory>
#include <functional>
#include <chrono>

namespace ifs {

/// WebSocket server built on Boost.Beast + Boost.Asio.
/// All Beast/Asio types are hidden behind PIMPL.
class WebSocketServer {
public:
    WebSocketServer();
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
    /// @param interval  Time between heartbeats.
    /// @param provider  Callback returning the heartbeat JSON string.
    using HeartbeatProvider = std::function<std::string()>;
    void startHeartbeat(std::chrono::milliseconds interval,
                        HeartbeatProvider provider);

private:
    void scheduleNextHeartbeat();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ifs
