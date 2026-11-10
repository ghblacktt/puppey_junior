#pragma once

#include "ch/ch_writer.h"
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "true_price/true_price_engine.h"

namespace net = boost::asio;
namespace ssl = net::ssl;
namespace beast = boost::beast;
namespace websocket = beast::websocket;
using tcp = net::ip::tcp;

class BinancePublicWS : public std::enable_shared_from_this<BinancePublicWS> {
public:
    using BookCallback  = std::function<void(const std::string&, double, double, double, double)>;
    using TradeCallback = std::function<void(const std::string&, double, double, bool)>;
    void set_tp_engine(tp::TruePriceEngine* e) { tp_engine_ = e; }
    BinancePublicWS(net::io_context& ioc, ssl::context& ctx,
                    const std::vector<std::string>& symbols);
    ~BinancePublicWS();

    void set_book_callback(BookCallback cb)   { on_book_  = std::move(cb); }
    void set_trade_callback(TradeCallback cb) { on_trade_ = std::move(cb); }
    void set_ch_writer(std::shared_ptr<ch::Writer> w) { ch_writer_ = std::move(w); }

    void run();
    void stop();

private:
    enum class Channel { BookTicker, AggTrade };

    struct Connection {
        Channel     channel;
        std::string path_prefix;   // "/public/" or "/market/"
        std::string suffix;        // "@bookTicker" or "@aggTrade"
        std::unique_ptr<websocket::stream<beast::ssl_stream<beast::tcp_stream>>> ws;
        beast::flat_buffer buffer;
        std::unique_ptr<net::steady_timer> reconnect_timer;
        std::unique_ptr<net::steady_timer> ping_timer;
        int  reconnect_attempts = 0;
        bool subscribed = false;
    };
    std::unordered_map<std::string, double> last_bbo_sz_;
    tp::TruePriceEngine* tp_engine_ = nullptr;

    std::string build_stream_path(const Connection& c) const;

    void connect_channel(std::shared_ptr<Connection> c);
    void on_resolve(std::shared_ptr<Connection> c, beast::error_code ec,
                    const tcp::resolver::results_type& results);
    void on_connect(std::shared_ptr<Connection> c, beast::error_code ec,
                    const tcp::resolver::results_type::endpoint_type&);
    void on_ssl_handshake(std::shared_ptr<Connection> c, beast::error_code ec);
    void on_ws_handshake(std::shared_ptr<Connection> c, beast::error_code ec);
    void on_read(std::shared_ptr<Connection> c, beast::error_code ec, std::size_t bytes);
    void send_ping(std::shared_ptr<Connection> c);
    void schedule_reconnect(std::shared_ptr<Connection> c);

    void process_message(std::shared_ptr<Connection> c,
                         const char* data, std::size_t size);
    void handle_book_ticker(const char* data, std::size_t size);
    void handle_agg_trade(const char* data, std::size_t size);

    net::io_context& ioc_;
    ssl::context& ctx_;
    std::string host_ = "fstream.binance.com";
    std::vector<std::string> symbols_;

    // BBO cache для is_through
    std::unordered_map<std::string, std::pair<double,double>> last_bbo_;
    std::vector<char> json_buffer_;

    std::shared_ptr<Connection> book_conn_;
    std::shared_ptr<Connection> trade_conn_;

    BookCallback  on_book_;
    TradeCallback on_trade_;
    std::shared_ptr<ch::Writer> ch_writer_;

    bool running_ = false;
};