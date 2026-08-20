#pragma once

#include <memory>
#include <string>
#include <deque>

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>

namespace ifs {

class ConnectionManager;
class ResourceManager;

/// Manages a single persistent WebSocket connection.
///
/// Threading: all async operations run on the owning io_context thread.
/// send() is thread-safe and can be called from background worker callbacks.
class WebSocketSession : public std::enable_shared_from_this<WebSocketSession> {
public:
    WebSocketSession(boost::asio::ip::tcp::socket socket,
                     ConnectionManager& manager,
                     ResourceManager& resourceManager);
    ~WebSocketSession();

    /// Start the WebSocket handshake, then enter the async read loop.
    void run();

    /// Queue a text message for sending. Thread-safe (posts to strand/io_context).
    void send(const std::string& message);

private:
    void onAccept(boost::beast::error_code ec);
    void doRead();
    void onRead(boost::beast::error_code ec, std::size_t bytes_transferred);
    void onMessage(const std::string& message);
    void doWrite();
    void onWrite(boost::beast::error_code ec, std::size_t bytes_transferred);

    boost::beast::websocket::stream<boost::beast::tcp_stream> ws_;
    boost::beast::flat_buffer    buffer_;
    ConnectionManager&           manager_;
    ResourceManager&             resourceManager_;
    std::deque<std::string>      writeQueue_;
    bool                         writing_ = false;

    static constexpr std::size_t MAX_QUEUE_SIZE = 64;
};

} // namespace ifs
