#pragma once
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <string>
#include <memory>
#include <functional>
#include <atomic>
#include <deque>
#include <mutex>
#include <nlohmann/json.hpp>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = boost::asio::ip::tcp;

class BybitApiClient; // forward

class BybitTradeWSClient : public std::enable_shared_from_this<BybitTradeWSClient> {
public:
    struct Ack {
        std::string req_id;
        int ret_code = -1;
        std::string ret_msg;
        std::string order_id;   // from data.orderId
        std::string order_link_id;
    };

    using AckCallback = std::function<void(const Ack&)>;

    BybitTradeWSClient(net::io_context& ioc, ssl::context& ctx,
                       BybitApiClient& api,
                       AckCallback on_ack);

    void run(const std::string& host = "stream.bybit.com");
    void stop();

    // thread-safe enqueue
    void create_order(const nlohmann::json& args);
    void cancel_order(const nlohmann::json& args);
    void amend_order(const nlohmann::json& args);

private:
    net::io_context& ioc_;
    ssl::context& ctx_;
    BybitApiClient& api_;
    tcp::resolver resolver_;
    std::unique_ptr<websocket::stream<beast::ssl_stream<beast::tcp_stream>>> ws_;
    beast::flat_buffer buffer_;
    net::steady_timer ping_timer_;
    net::steady_timer reconnect_timer_;
    std::string host_;

    AckCallback on_ack_;
    std::atomic<bool> authenticated_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> write_in_progress_{false};
    std::mutex queue_mtx_;
    std::deque<std::string> out_queue_;   // serialized JSON
    std::vector<char> json_buffer_;
    int reconnect_attempts_ = 0;

    void create_websocket();
    void on_resolve(beast::error_code ec, const tcp::resolver::results_type& results);
    void on_connect(beast::error_code ec, const tcp::resolver::results_type::endpoint_type ep);
    void on_ssl_handshake(beast::error_code ec);
    void on_handshake(beast::error_code ec);
    void on_read(beast::error_code ec, std::size_t bytes);
    void on_write(beast::error_code ec, std::size_t bytes);
    void send_auth();
    void send_ping();
    void do_write();
    void process_message(const char* data, std::size_t size);
    void reconnect();
    void on_reconnect_timer(beast::error_code ec);

    std::string next_req_id();
};