#include "binance_public_ws.h"
#include <simdjson.h>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cstring>

BinancePublicWS::BinancePublicWS(net::io_context& ioc, ssl::context& ctx,
                                 const std::vector<std::string>& symbols)
        : ioc_(ioc), ctx_(ctx), symbols_(symbols)
{
    json_buffer_.resize(65536 + simdjson::SIMDJSON_PADDING);

    auto make_conn = [&](Channel ch, std::string prefix, std::string sfx) {
        auto c = std::make_shared<Connection>();
        c->channel     = ch;
        c->path_prefix = std::move(prefix);
        c->suffix      = std::move(sfx);
        c->reconnect_timer = std::make_unique<net::steady_timer>(net::make_strand(ioc));
        c->ping_timer      = std::make_unique<net::steady_timer>(net::make_strand(ioc));
        return c;
    };

    book_conn_  = make_conn(Channel::BookTicker, "/public/", "@bookTicker");
    trade_conn_ = make_conn(Channel::AggTrade,   "/market/", "@aggTrade");
}

BinancePublicWS::~BinancePublicWS() {
    stop();
}

void BinancePublicWS::run() {
    running_ = true;
    std::cout << "[BinancePubWS] starting ("
              << symbols_.size() << " symbols, 2 channels)\n";
    connect_channel(book_conn_);
    connect_channel(trade_conn_);
}

void BinancePublicWS::stop() {
    if (!running_) return;
    running_ = false;

    for (auto* c : {&book_conn_, &trade_conn_}) {
        if (!*c) continue;
        try { (*c)->reconnect_timer->cancel(); } catch (...) {}
        try { (*c)->ping_timer->cancel(); } catch (...) {}
        if ((*c)->ws) {
            beast::error_code ec;
            try { if ((*c)->ws->is_open())
                    (*c)->ws->close(websocket::close_code::normal, ec); } catch (...) {}
            (*c)->ws.reset();
        }
    }
    std::cout << "[BinancePubWS] stopped\n";
}

std::string BinancePublicWS::build_stream_path(const Connection& c) const {
    std::ostringstream ss;
    ss << c.path_prefix << "stream?streams=";
    bool first = true;
    for (const auto& sym : symbols_) {
        // Binance streams: lowercase
        std::string lower;
        lower.reserve(sym.size());
        for (char ch : sym) lower.push_back(static_cast<char>(std::tolower(ch)));
        if (!first) ss << "/";
        first = false;
        ss << lower << c.suffix;
    }
    return ss.str();
}

void BinancePublicWS::connect_channel(std::shared_ptr<Connection> c) {
    if (!running_) return;
    c->ws = std::make_unique<websocket::stream<beast::ssl_stream<beast::tcp_stream>>>(
            net::make_strand(ioc_), ctx_);
    c->ws->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
    c->ws->set_option(websocket::stream_base::decorator(
            [](websocket::request_type& req) {
                req.set(boost::beast::http::field::user_agent, "HFT-Cpp-BinanceWS");
            }));

    auto resolver = std::make_shared<tcp::resolver>(net::make_strand(ioc_));
    resolver->async_resolve(host_, "443",
                            [self = shared_from_this(), c, resolver]
                                    (beast::error_code ec, const tcp::resolver::results_type& results) {
                                self->on_resolve(c, ec, results);
                            });
}

void BinancePublicWS::on_resolve(std::shared_ptr<Connection> c, beast::error_code ec,
                                 const tcp::resolver::results_type& results) {
    if (ec) { schedule_reconnect(c); return; }
    net::async_connect(c->ws->next_layer().next_layer().socket(), results,
                       [self = shared_from_this(), c]
                               (beast::error_code ec, const tcp::resolver::results_type::endpoint_type& ep) {
                           self->on_connect(c, ec, ep);
                       });
}

void BinancePublicWS::on_connect(std::shared_ptr<Connection> c, beast::error_code ec,
                                 const tcp::resolver::results_type::endpoint_type&) {
    if (ec) { schedule_reconnect(c); return; }
    if (!SSL_set_tlsext_host_name(c->ws->next_layer().native_handle(), host_.c_str())) {
        schedule_reconnect(c); return;
    }
    c->ws->next_layer().async_handshake(ssl::stream_base::client,
                                        [self = shared_from_this(), c](beast::error_code ec) {
                                            self->on_ssl_handshake(c, ec);
                                        });
}

void BinancePublicWS::on_ssl_handshake(std::shared_ptr<Connection> c, beast::error_code ec) {
    if (ec) { schedule_reconnect(c); return; }
    c->ws->async_handshake(host_, build_stream_path(*c),
                           [self = shared_from_this(), c](beast::error_code ec) {
                               self->on_ws_handshake(c, ec);
                           });
}

void BinancePublicWS::on_ws_handshake(std::shared_ptr<Connection> c, beast::error_code ec) {
    if (ec) { schedule_reconnect(c); return; }
    c->reconnect_attempts = 0;
    std::cout << "[BinancePubWS] connected (" << c->suffix
              << ", " << symbols_.size() << " symbols)\n";
    c->ws->async_read(c->buffer,
                      [self = shared_from_this(), c](beast::error_code ec, std::size_t bytes) {
                          self->on_read(c, ec, bytes);
                      });
    send_ping(c);
}

void BinancePublicWS::on_read(std::shared_ptr<Connection> c, beast::error_code ec,
                              std::size_t bytes) {
    if (ec) { schedule_reconnect(c); return; }
    auto bufs = c->buffer.data();
    if (bufs.size() > 0) {
        process_message(c, static_cast<const char*>(bufs.data()), bufs.size());
    }
    c->buffer.consume(bytes);
    if (!running_ || !c->ws) return;
    c->ws->async_read(c->buffer,
                      [self = shared_from_this(), c](beast::error_code ec, std::size_t bytes) {
                          self->on_read(c, ec, bytes);
                      });
}

void BinancePublicWS::send_ping(std::shared_ptr<Connection> c) {
    if (!running_ || !c->ws) return;
    c->ws->async_ping({},
                      [self = shared_from_this(), c](beast::error_code ec) {
                          if (ec || !self->running_ || !c->ws) return;
                          c->ping_timer->expires_after(std::chrono::seconds(15));
                          c->ping_timer->async_wait(
                                  [self, c](beast::error_code ec2) {
                                      if (!ec2 && self->running_) self->send_ping(c);
                                  });
                      });
}

void BinancePublicWS::schedule_reconnect(std::shared_ptr<Connection> c) {
    if (!running_) return;
    c->reconnect_attempts++;
    int delay = std::min(c->reconnect_attempts * 5, 30);
    std::cerr << "[BinancePubWS] " << c->suffix
              << " reconnect in " << delay << "s (attempt "
              << c->reconnect_attempts << ")\n";
    c->reconnect_timer->expires_after(std::chrono::seconds(delay));
    c->reconnect_timer->async_wait(
            [self = shared_from_this(), c](beast::error_code ec) {
                if (ec || !self->running_) return;
                self->connect_channel(c);
            });
}

void BinancePublicWS::process_message(std::shared_ptr<Connection> c,
                                      const char* data, std::size_t size) {
    // Combined stream wraps as: {"stream":"...","data":{...}}
    if (size + simdjson::SIMDJSON_PADDING > json_buffer_.capacity()) {
        json_buffer_.resize(size + simdjson::SIMDJSON_PADDING);
    }
    std::memcpy(json_buffer_.data(), data, size);

    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    if (parser.parse(json_buffer_.data(), size, size).get(doc) != simdjson::SUCCESS) return;

    // For combined streams, extract "data" element
    simdjson::dom::element data_el;
    if (doc["data"].get(data_el) != simdjson::SUCCESS) {
        // Some messages are raw (single-stream legacy) — treat doc as data
        data_el = doc;
    }

    // Determine channel by "e" field
    std::string_view ev_type;
    if (data_el["e"].get_string().get(ev_type) != simdjson::SUCCESS) return;

    if (ev_type == "bookTicker") {
        handle_book_ticker(json_buffer_.data(), size);
    } else if (ev_type == "aggTrade") {
        handle_agg_trade(json_buffer_.data(), size);
    }
}

void BinancePublicWS::handle_book_ticker(const char* data, std::size_t size) {
    if (size + simdjson::SIMDJSON_PADDING > json_buffer_.capacity()) {
        json_buffer_.resize(size + simdjson::SIMDJSON_PADDING);
    }
    std::memcpy(json_buffer_.data(), data, size);

    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    if (parser.parse(json_buffer_.data(), size, size).get(doc) != simdjson::SUCCESS) return;

    simdjson::dom::element d;
    if (doc["data"].get(d) != simdjson::SUCCESS) d = doc;

    std::string_view sym, bid_s, ask_s, bs_s, as_s;
    if (d["s"].get_string().get(sym) != simdjson::SUCCESS) return;
    if (d["b"].get_string().get(bid_s) != simdjson::SUCCESS) return;
    if (d["a"].get_string().get(ask_s) != simdjson::SUCCESS) return;
    if (d["B"].get_string().get(bs_s) != simdjson::SUCCESS) return;
    if (d["A"].get_string().get(as_s) != simdjson::SUCCESS) return;

    uint64_t ts_event = 0;
    (void)d["E"].get_uint64().get(ts_event);

    double bid = std::strtod(bid_s.data(), nullptr);
    double ask = std::strtod(ask_s.data(), nullptr);
    double bs  = std::strtod(bs_s.data(), nullptr);
    double as  = std::strtod(as_s.data(), nullptr);
    if (bid <= 0 || ask <= 0) return;

    std::string symbol(sym);
    last_bbo_[symbol] = {bid, ask};

    if (on_book_) on_book_(symbol, bid, ask, bs, as);
    if (ch_writer_) {
        ch_writer_->push_book(ch::VenueBinance, symbol,
                              ch::now_ms(), ts_event, bid, ask, bs, as);
    }
}

void BinancePublicWS::handle_agg_trade(const char* data, std::size_t size) {
    if (size + simdjson::SIMDJSON_PADDING > json_buffer_.capacity()) {
        json_buffer_.resize(size + simdjson::SIMDJSON_PADDING);
    }
    std::memcpy(json_buffer_.data(), data, size);

    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    if (parser.parse(json_buffer_.data(), size, size).get(doc) != simdjson::SUCCESS) return;

    simdjson::dom::element d;
    if (doc["data"].get(d) != simdjson::SUCCESS) d = doc;

    std::string_view sym, px_s, sz_s;
    uint64_t ts_event = 0;
    bool m = false;
    uint64_t agg_id = 0;

    if (d["s"].get_string().get(sym) != simdjson::SUCCESS) return;
    if (d["p"].get_string().get(px_s) != simdjson::SUCCESS) return;
    if (d["q"].get_string().get(sz_s) != simdjson::SUCCESS) return;
    (void)d["T"].get_uint64().get(ts_event);
    (void)d["m"].get_bool().get(m);
    (void)d["a"].get_uint64().get(agg_id);

    double px = std::strtod(px_s.data(), nullptr);
    double sz = std::strtod(sz_s.data(), nullptr);
    if (px <= 0 || sz <= 0) return;

    std::string symbol(sym);
    // m=true → buyer is maker → taker was SELL → is_buyer_maker=true
    bool is_buyer_maker = m;

    // BBO for is_through
    auto it = last_bbo_.find(symbol);
    double bbo_bid = it != last_bbo_.end() ? it->second.first  : 0.0;
    double bbo_ask = it != last_bbo_.end() ? it->second.second : 0.0;
    bool is_through = false;
    if (bbo_bid > 0 && bbo_ask > 0) {
        if (!is_buyer_maker && px > bbo_ask) is_through = true;
        if ( is_buyer_maker && px < bbo_bid) is_through = true;
    }

    if (on_trade_) on_trade_(symbol, px, sz, is_buyer_maker);

    if (ch_writer_) {
        // Use agg_id as stable trade_id
        uint64_t tid_hash = ch::fnv1a("binance|" + std::to_string(agg_id));
        ch_writer_->push_trade(ch::VenueBinance, symbol,
                               ch::now_ms(), ts_event,
                               px, sz,
                               is_buyer_maker ? -1 : +1,
                               is_through ? 1 : 0,
                               tid_hash,
                               bbo_bid, bbo_ask);
    }
}