#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <deque>

#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "tts/TtsService.hpp"

namespace ifs {

class ConnectionManager;
class ResourceManager;

/// Manages a single persistent WebSocket connection.
///
/// Threading: all async operations run on the owning io_context thread.
/// send() and sendBinary() are thread-safe and can be called from background worker callbacks.
class WebSocketSession : public std::enable_shared_from_this<WebSocketSession> {
public:
    WebSocketSession(boost::asio::ip::tcp::socket socket,
                     ConnectionManager& manager,
                     ResourceManager& resourceManager,
                     TtsService& ttsService);
    ~WebSocketSession();

    /// Start the WebSocket handshake, then enter the async read loop.
    void run();

    /// Queue a text message for sending. Thread-safe (posts to strand/io_context).
    void send(const std::string& message);

    /// Queue a binary frame (TTS audio). Thread-safe.
    void sendBinary(std::string frame);

    /// False once the connection has closed or failed. Thread-safe.
    bool isOpen() const { return open_.load(); }

private:
    void onAccept(boost::beast::error_code ec);
    void doRead();
    void onRead(boost::beast::error_code ec, std::size_t bytes_transferred);
    void onMessage(const std::string& message);
    TtsService::Sink makeTtsSink();
    void enqueue(std::string payload, bool binary);
    void doWrite();
    void onWrite(boost::beast::error_code ec, std::size_t bytes_transferred);

    boost::beast::websocket::stream<boost::beast::tcp_stream> ws_;
    boost::beast::flat_buffer    buffer_;
    ConnectionManager&           manager_;
    ResourceManager&             resourceManager_;
    TtsService&                  ttsService_;
    std::atomic<bool>            open_{false};

    struct OutgoingMessage {
        std::string payload;
        bool        binary = false;
    };
    std::deque<OutgoingMessage>  writeQueue_;
    std::size_t                  queuedText_        = 0;  // text messages waiting
    std::size_t                  queuedBinaryBytes_ = 0;  // audio bytes waiting
    bool                         writing_ = false;

    static constexpr std::size_t MAX_QUEUE_SIZE         = 64;                // text messages
    static constexpr std::size_t MAX_BINARY_QUEUE_BYTES = 32 * 1024 * 1024;  // ~11 min of 24 kHz PCM
};

} // namespace ifs
