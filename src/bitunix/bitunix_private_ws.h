#pragma once
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <string>
#include <memory>
#include <functional>
#include <atomic>
#include <chrono>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = boost::asio::ip::tcp;

class BitunixPrivateWS : public std::enable_shared_from_this<BitunixPrivateWS> {
public:
    // Колбэк на обновление ордера
    using OrderCallback = std::function<void(
            const std::string& order_id,
            const std::string& symbol,
            const std::string& side,
            const std::string& status,
            double price,
            double qty,
            double filled_qty)>;

    // Колбэк на обновление позиции
    using PositionCallback = std::function<void(
            const std::string& symbol,
            const std::string& side,
            double size,
            double avg_price,
            double unrealized_pnl)>;

    using SubscribeCallback = std::function<void()>;

    BitunixPrivateWS(net::io_context& ioc, ssl::context& ctx,
                     const std::string& api_key,
                     const std::string& api_secret,
                     OrderCallback on_order,
                     PositionCallback on_position,
                     SubscribeCallback on_subscribed);

    void run(const std::string& host = "fapi.bitunix.com");
    void stop();

private:
    net::io_context& ioc_;
    ssl::context& ctx_;
    tcp::resolver resolver_;
    std::unique_ptr<websocket::stream<beast::ssl_stream<beast::tcp_stream>>> ws_;
    beast::flat_buffer buffer_;
    net::steady_timer ping_timer_;
    net::steady_timer reconnect_timer_;
    net::steady_timer subscribe_delay_timer_;
    std::string api_key_;
    std::string api_secret_;
    std::string host_;
    OrderCallback on_order_;
    PositionCallback on_position_;
    SubscribeCallback on_subscribed_;
    std::vector<char> json_buffer_;
    std::atomic<bool> running_{false};
    std::atomic<bool> authenticated_{false};
    int reconnect_attempts_ = 0;

    void create_ws();
    void on_resolve(beast::error_code ec, const tcp::resolver::results_type& results);
    void on_connect(beast::error_code ec, const tcp::resolver::results_type::endpoint_type);
    void on_ssl_handshake(beast::error_code ec);
    void on_handshake(beast::error_code ec);
    void on_read(beast::error_code ec, std::size_t bytes);
    void on_write(beast::error_code ec, std::size_t bytes);
    void send_login();
    void subscribe();
    void send_ping();
    void process_message(const char* data, std::size_t size);
    void reconnect();
    void on_reconnect_timer(beast::error_code ec);

    static std::string sha256_hex(const std::string& input);
    static std::string generate_nonce();
    uint64_t now_ms() const;
    uint64_t now_sec() const;
};