#pragma once
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <memory>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <atomic>
#include <thread>
#include "screener.h"

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = boost::asio::ip::tcp;

class ScannerClient : public std::enable_shared_from_this<ScannerClient> {
public:
    ScannerClient(net::io_context& ioc, ssl::context& ctx,
                  const std::vector<std::string>& symbols,
                  Screener& screener);
    ~ScannerClient();

    void run(const std::string& host = "stream.bybit.com");
    void stop();

private:
    struct OrderbookUpdate {
        std::string symbol;
        uint64_t    timestamp_ms = 0;
        uint64_t    update_id = 0;
        std::string update_type;
        std::vector<double> bid_prices;
        std::vector<double> bid_sizes;
        std::vector<double> ask_prices;
        std::vector<double> ask_sizes;
    };

    struct TradeTick {
        std::string symbol;
        uint64_t    timestamp_ms = 0;
        double      price = 0.0;
        double      volume = 0.0;
        bool        is_buyer_maker = false;
    };

    void create_ws();
    void on_resolve(beast::error_code ec, const tcp::resolver::results_type& results);
    void on_connect(beast::error_code ec, const tcp::resolver::results_type::endpoint_type);
    void on_ssl_handshake(beast::error_code ec);
    void on_handshake(beast::error_code ec);
    void on_read(beast::error_code ec, std::size_t bytes);
    void on_write(beast::error_code ec, std::size_t bytes);

    void subscribe_next_batch(size_t start_idx);
    void send_ping();
    void process_message(const char* data, std::size_t size);
    void reconnect();

    // ClickHouse writers
    void add_book_update(const OrderbookUpdate& u);
    void add_trade_tick(const TradeTick& t);
    void book_writer_loop();
    void trade_writer_loop();
    void send_batch_books_to_clickhouse(const std::vector<OrderbookUpdate>& batch);
    void send_batch_trades_to_clickhouse(const std::vector<TradeTick>& batch);

    net::io_context& ioc_;
    ssl::context& ctx_;
    tcp::resolver resolver_;
    std::unique_ptr<websocket::stream<beast::ssl_stream<beast::tcp_stream>>> ws_;
    beast::flat_buffer buffer_;
    net::steady_timer ping_timer_;
    net::steady_timer reconnect_timer_;
    net::steady_timer subscribe_delay_timer_;
    std::string host_;
    std::vector<std::string> symbols_;
    Screener& screener_;
    std::vector<char> json_buffer_;
    int reconnect_attempts_ = 0;
    bool reconnecting_ = false;
    std::atomic<bool> running_{false};

    // Book updates queue
    std::deque<OrderbookUpdate> book_queue_;
    std::mutex                  book_queue_mutex_;
    std::thread                 book_writer_thread_;
    std::atomic<bool>           book_writer_running_{false};

    // Trade ticks queue
    std::deque<TradeTick>       trade_queue_;
    std::mutex                  trade_queue_mutex_;
    std::thread                 trade_writer_thread_;
    std::atomic<bool>           trade_writer_running_{false};
};