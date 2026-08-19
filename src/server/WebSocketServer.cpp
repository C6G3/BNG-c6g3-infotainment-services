#include "server/WebSocketServer.hpp"
#include "server/WebSocketSession.hpp"
#include "server/ConnectionManager.hpp"
#include "core/Logger.hpp"

#include <boost/beast/core.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/post.hpp>

#include <thread>
#include <memory>

namespace beast = boost::beast;
namespace net   = boost::asio;
using tcp       = net::ip::tcp;

namespace ifs {

// ─── Listener: async TCP accept loop ─────────────────────────────────────────

class WsListener : public std::enable_shared_from_this<WsListener> {
public:
    WsListener(net::io_context& ioc, tcp::endpoint endpoint,
               ConnectionManager& manager)
        : ioc_(ioc)
        , acceptor_(ioc)
        , manager_(manager)
    {
        beast::error_code ec;

        acceptor_.open(endpoint.protocol(), ec);
        if (ec) {
            Logger::instance().error("WebSocket", "Socket open failed: {}",
                                      ec.message());
            return;
        }

        acceptor_.set_option(net::socket_base::reuse_address(true), ec);
        if (ec) {
            Logger::instance().error("WebSocket", "Set reuse_address failed: {}",
                                      ec.message());
            return;
        }

        acceptor_.bind(endpoint, ec);
        if (ec) {
            Logger::instance().error("WebSocket", "Bind failed: {}", ec.message());
            return;
        }

        acceptor_.listen(net::socket_base::max_listen_connections, ec);
        if (ec) {
            Logger::instance().error("WebSocket", "Listen failed: {}", ec.message());
            return;
        }
    }

    bool isOpen() const { return acceptor_.is_open(); }

    void run() { doAccept(); }

private:
    void doAccept() {
        acceptor_.async_accept(
            ioc_,
            [self = shared_from_this()](beast::error_code ec, tcp::socket socket) {
                self->onAccept(ec, std::move(socket));
            });
    }

    void onAccept(beast::error_code ec, tcp::socket socket) {
        if (ec) {
            if (ec != net::error::operation_aborted)
                Logger::instance().error("WebSocket", "Accept error: {}",
                                          ec.message());
            return;
        }

        // Each accepted connection becomes a WebSocket session
        std::make_shared<WebSocketSession>(std::move(socket), manager_)->run();
        doAccept();
    }

    net::io_context&   ioc_;
    tcp::acceptor      acceptor_;
    ConnectionManager& manager_;
};

// ─── WebSocketServer PIMPL ───────────────────────────────────────────────────

struct WebSocketServer::Impl {
    net::io_context              ioc{1};  // single-threaded
    std::shared_ptr<WsListener>  listener;
    std::thread                  workerThread;
    ConnectionManager            connectionManager;

    // Heartbeat
    net::steady_timer            heartbeatTimer{ioc};
    HeartbeatProvider            heartbeatProvider;
    std::chrono::milliseconds    heartbeatInterval{2500};
    bool                         heartbeatActive = false;
};

WebSocketServer::WebSocketServer()
    : impl_(std::make_unique<Impl>()) {}

WebSocketServer::~WebSocketServer() {
    stop();
}

bool WebSocketServer::start(const std::string& host, uint16_t port) {
    auto& log = Logger::instance();

    try {
        auto address  = net::ip::make_address(host);
        auto endpoint = tcp::endpoint{address, port};

        impl_->listener = std::make_shared<WsListener>(
            impl_->ioc, endpoint, impl_->connectionManager);

        if (!impl_->listener->isOpen()) {
            log.error("WebSocket", "Failed to bind to {}:{}", host, port);
            return false;
        }

        impl_->listener->run();

        // Run the io_context on a dedicated worker thread
        impl_->workerThread = std::thread([this]() {
            impl_->ioc.run();
        });

        log.info("WebSocket", "Listening on ws://{}:{}", host, port);
        return true;

    } catch (const std::exception& e) {
        log.error("WebSocket", "Failed to start: {}", e.what());
        return false;
    }
}

void WebSocketServer::stop() {
    if (!impl_) return;

    impl_->heartbeatActive = false;
    impl_->heartbeatTimer.cancel();
    impl_->ioc.stop();

    if (impl_->workerThread.joinable())
        impl_->workerThread.join();

    Logger::instance().info("WebSocket", "Server stopped");
}

void WebSocketServer::broadcast(const std::string& message) {
    impl_->connectionManager.broadcast(message);
}

std::size_t WebSocketServer::connectionCount() const {
    return impl_->connectionManager.connectionCount();
}

// ─── Heartbeat Timer ─────────────────────────────────────────────────────────

void WebSocketServer::startHeartbeat(std::chrono::milliseconds interval,
                                      HeartbeatProvider provider) {
    impl_->heartbeatInterval = interval;
    impl_->heartbeatProvider = std::move(provider);
    impl_->heartbeatActive   = true;

    // Ensure the first timer setup runs on the io_context thread
    net::post(impl_->ioc, [this]() {
        scheduleNextHeartbeat();
    });
}

void WebSocketServer::scheduleNextHeartbeat() {
    if (!impl_->heartbeatActive) return;

    impl_->heartbeatTimer.expires_after(impl_->heartbeatInterval);
    impl_->heartbeatTimer.async_wait(
        [this](boost::system::error_code ec) {
            if (ec || !impl_->heartbeatActive) return;

            if (impl_->heartbeatProvider &&
                impl_->connectionManager.connectionCount() > 0) {
                auto message = impl_->heartbeatProvider();
                impl_->connectionManager.broadcast(message);
            }

            scheduleNextHeartbeat();
        });
}

} // namespace ifs
