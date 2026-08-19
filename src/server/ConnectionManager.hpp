#pragma once

#include <memory>
#include <set>
#include <string>
#include <mutex>

namespace ifs {

class WebSocketSession;

/// Tracks active WebSocket sessions and provides broadcasting.
/// Thread-safe: all methods may be called from any thread.
class ConnectionManager {
public:
    void join(std::shared_ptr<WebSocketSession> session);
    void leave(std::shared_ptr<WebSocketSession> session);
    void broadcast(const std::string& message);
    std::size_t connectionCount() const;

private:
    mutable std::mutex mutex_;
    std::set<std::shared_ptr<WebSocketSession>> sessions_;
};

} // namespace ifs
