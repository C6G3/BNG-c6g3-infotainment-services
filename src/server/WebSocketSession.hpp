#pragma once

#include <memory>
#include <string>
#include <deque>

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>

namespace ifs {

class ConnectionManager;

/// Manages a single persistent WebSocket connection.
///
/// Threading: all async operations run on the owning io_context thread.
/// send() is safe to call from the io_context thread (heartbeat timer, etc.).
class WebSocketSession : public std::enable_shared_from_this<WebSocketSession> {
public:
    WebSocketSession(boost::asio::ip::tcp::socket socket,
                     ConnectionManager& manager);
    ~WebSocketSession();

    /// Start the WebSocket handshake, then enter the async read loop.
    void run();

    /// Queue a text message for sending. Bounded queue — drops if full.
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
    std::deque<std::string>      writeQueue_;
    bool                         writing_ = false;

    static constexpr std::size_t MAX_QUEUE_SIZE = 16;
};

} // namespace ifs
