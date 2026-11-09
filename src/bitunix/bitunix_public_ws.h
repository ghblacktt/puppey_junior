#pragma once
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <string>
#include <memory>
#include <vector>
#include <functional>
#include <atomic>
#include "ch/ch_writer.h"


namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = boost::asio::ip::tcp;

class BitunixPublicWS : public std::enable_shared_from_this<BitunixPublicWS> {
public:
    using BookCallback = std::function<void(
            const std::string& symbol,
            double bid, double ask,
            double bid_size, double ask_size)>;

    BitunixPublicWS(net::io_context& ioc, ssl::context& ctx,
                    const std::vector<std::string>& symbols);
    using TradeCallback = std::function<void(const std::string&, double, double, bool)>;   // symbol, price, size, is_buyer_maker

    void set_trade_callback(TradeCallback cb) { on_trade_ = std::move(cb); }
    void run(const std::string& host = "fapi.bitunix.com");
    void stop();

    void set_book_callback(BookCallback cb) { on_book_ = std::move(cb); }
    void set_ch_writer(std::shared_ptr<ch::Writer> w) { ch_writer_ = std::move(w); }
private:
    std::shared_ptr<ch::Writer> ch_writer_;
    std::unordered_map<std::string, std::pair<double,double>> last_bbo_;
    TradeCallback on_trade_;
    void create_ws();
    void on_resolve(beast::error_code ec, const tcp::resolver::results_type& results);
    void on_connect(beast::error_code ec, const tcp::resolver::results_type::endpoint_type);
    void on_ssl_handshake(beast::error_code ec);
    void on_handshake(beast::error_code ec);
    void on_read(beast::error_code ec, std::size_t bytes);
    void on_write(beast::error_code ec, std::size_t bytes);
    void subscribe_next(size_t idx);
    void send_ping();
    void process_message(const char* data, std::size_t size);
    void reconnect();
    void on_reconnect_timer(beast::error_code ec);

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
    BookCallback on_book_;
    std::vector<char> json_buffer_;
    std::atomic<bool> running_{false};
    int reconnect_attempts_ = 0;
};