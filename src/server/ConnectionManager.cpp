#include "server/ConnectionManager.hpp"
#include "server/WebSocketSession.hpp"
#include "core/Logger.hpp"

namespace ifs {

void ConnectionManager::join(std::shared_ptr<WebSocketSession> session) {
    std::lock_guard lock(mutex_);
    sessions_.insert(std::move(session));
}

void ConnectionManager::leave(std::shared_ptr<WebSocketSession> session) {
    std::lock_guard lock(mutex_);
    sessions_.erase(session);
}

void ConnectionManager::broadcast(const std::string& message) {
    std::lock_guard lock(mutex_);
    for (const auto& session : sessions_) {
        session->send(message);
    }
}

std::size_t ConnectionManager::connectionCount() const {
    std::lock_guard lock(mutex_);
    return sessions_.size();
}

} // namespace ifs
