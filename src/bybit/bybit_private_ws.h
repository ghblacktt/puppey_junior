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

class BybitPrivateWSClient : public std::enable_shared_from_this<BybitPrivateWSClient> {
public:

    using PositionCallback = std::function<void(
            const std::string& symbol, const std::string& side,
            double size, double avg_price, double unrealised_pnl)>;

    // execution: execId, orderId, symbol, side, price, qty, fee, fee_ccy, exec_time, is_maker
    using ExecutionCallback = std::function<void(
            const std::string& exec_id, const std::string& order_id,
            const std::string& symbol, const std::string& side,
            double price, double qty, double fee,
            const std::string& fee_currency, uint64_t exec_time, bool is_maker)>;

    // order state change: orderId, orderLinkId, symbol, status, rejectReason
    using OrderCallback = std::function<void(
            const std::string& order_id, const std::string& order_link_id,
            const std::string& symbol, const std::string& status,
            const std::string& reject_reason)>;

    using SubscribeCallback = std::function<void()>;

    BybitPrivateWSClient(net::io_context& ioc, ssl::context& ctx,
                         const std::string& api_key,
                         const std::string& api_secret,
                         PositionCallback on_position,
                         ExecutionCallback on_execution,
                         OrderCallback on_order,
                         SubscribeCallback on_subscribed);

    void run(const std::string& host = "stream.bybit.com");
    void stop();

    BybitPrivateWSClient(net::io_context& ioc, ssl::context& ctx,
                         const std::string& api_key,
                         const std::string& api_secret,
                         PositionCallback on_position,
                         SubscribeCallback on_subscribed);

private:
    PositionCallback on_position_;
    ExecutionCallback on_execution_;
    OrderCallback on_order_;
    SubscribeCallback on_subscribed_;

    void subscribe_topics();
    void process_message(const char* data, std::size_t size);
    net::io_context& ioc_;
    ssl::context& ctx_;
    tcp::resolver resolver_;
    std::unique_ptr<websocket::stream<beast::ssl_stream<beast::tcp_stream>>> ws_;
    beast::flat_buffer buffer_;
    net::steady_timer ping_timer_;
    std::string api_key_;
    std::string api_secret_;
    std::string host_;
    net::steady_timer reconnect_timer_;

    std::vector<char> json_buffer_;
    std::atomic<bool> running_{false};
    std::atomic<bool> authenticated_{false};
    int reconnect_attempts_ = 0;

    void create_websocket();
    void on_resolve(beast::error_code ec, const tcp::resolver::results_type& results);
    void on_connect(beast::error_code ec);
    void on_ssl_handshake(beast::error_code ec);
    void on_handshake(beast::error_code ec);
    void on_read(beast::error_code ec, std::size_t bytes);
    void on_write(beast::error_code ec, std::size_t bytes);
    void send_auth();
    void subscribe_positions();
    void send_ping();
    std::string hmac_sha256(const std::string& data) const;
    uint64_t current_time_ms() const;
    void reconnect();
    void on_reconnect_timer(beast::error_code ec);
};