#include "bitunix_public_ws.h"
#include <simdjson.h>
#include <iostream>
#include <sstream>
#include <chrono>
#include <unordered_set>
#include "ch/ch_writer.h"

BitunixPublicWS::BitunixPublicWS(net::io_context& ioc, ssl::context& ctx,
                                 const std::vector<std::string>& symbols)
        : ioc_(ioc), ctx_(ctx),
          resolver_(net::make_strand(ioc)),
          ping_timer_(net::make_strand(ioc)),
          reconnect_timer_(net::make_strand(ioc)),
          subscribe_delay_timer_(net::make_strand(ioc)),
          symbols_(symbols) {
    json_buffer_.resize(65536 + simdjson::SIMDJSON_PADDING);
}

void BitunixPublicWS::run(const std::string& host) {
    host_ = host;
    running_ = true;
    create_ws();
    resolver_.async_resolve(host_, "443",
                            beast::bind_front_handler(&BitunixPublicWS::on_resolve, shared_from_this()));
}

void BitunixPublicWS::stop() {
    running_ = false;
    ping_timer_.cancel();
    reconnect_timer_.cancel();
    subscribe_delay_timer_.cancel();
    if (ws_) {
        beast::error_code ec;
        if (ws_->is_open()) ws_->close(websocket::close_code::normal, ec);
        ws_.reset();
    }
}

void BitunixPublicWS::create_ws() {
    ws_ = std::make_unique<websocket::stream<beast::ssl_stream<beast::tcp_stream>>>(
            net::make_strand(ioc_), ctx_);

    // WebSocket-level timeouts (явные, не suggested)
    websocket::stream_base::timeout t;
    t.handshake_timeout = std::chrono::seconds(30);
    t.idle_timeout      = std::chrono::seconds(600);   // 10 минут
    t.keep_alive_pings  = false;
    ws_->set_option(t);

    // Отключить TCP-level timeout полностью
    beast::get_lowest_layer(*ws_).expires_never();

    ws_->set_option(websocket::stream_base::decorator([](websocket::request_type& req) {
        req.set(boost::beast::http::field::user_agent, "HFT-Cpp-PublicWS");
    }));
}

void BitunixPublicWS::on_resolve(beast::error_code ec, const tcp::resolver::results_type& results) {
    if (ec) { reconnect(); return; }
    net::async_connect(ws_->next_layer().next_layer().socket(), results,
                       beast::bind_front_handler(&BitunixPublicWS::on_connect, shared_from_this()));
}

void BitunixPublicWS::on_connect(beast::error_code ec, const tcp::resolver::results_type::endpoint_type) {
    if (ec) { reconnect(); return; }
    if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(), host_.c_str())) {
        reconnect(); return;
    }
    ws_->next_layer().async_handshake(ssl::stream_base::client,
                                      beast::bind_front_handler(&BitunixPublicWS::on_ssl_handshake, shared_from_this()));
}

void BitunixPublicWS::on_ssl_handshake(beast::error_code ec) {
    if (ec) { reconnect(); return; }
    ws_->async_handshake(host_, "/public/",
                         beast::bind_front_handler(&BitunixPublicWS::on_handshake, shared_from_this()));
}

void BitunixPublicWS::on_handshake(beast::error_code ec) {
    if (!running_ || !ws_) return;
    if (ec) { reconnect(); return; }
    reconnect_attempts_ = 0;
    std::cout << "[BitunixPubWS] connected, subscribing to "
              << symbols_.size() << " symbols\n";
    ws_->async_read(buffer_,
                    beast::bind_front_handler(&BitunixPublicWS::on_read, shared_from_this()));
    subscribe_next(0);
}

void BitunixPublicWS::subscribe_next(size_t idx) {
    if (!running_ || !ws_) return;
    if (idx >= symbols_.size()) {
        std::cout << "[BitunixPubWS] all subscribed\n";
        send_ping();
        return;
    }

    // Батч по 5 символов → одно сообщение (лимит: 5 msg/sec)
    constexpr size_t kBatch = 2;
    size_t end = std::min(idx + kBatch, symbols_.size());

    std::ostringstream msg;
    msg << R"({"op":"subscribe","args":[)";
    bool first = true;
    for (size_t i = idx; i < end; ++i) {
        if (!first) msg << ",";
        first = false;
        msg << R"({"symbol":")" << symbols_[i] << R"(","ch":"depth_book1"})";
        msg << R"(,{"symbol":")" << symbols_[i] << R"(","ch":"trade"})";
    }
    msg << "]}";

    ws_->text(true);
    ws_->async_write(net::buffer(msg.str()),
                     [self = shared_from_this(), end](beast::error_code ec, std::size_t) {
                         if (ec) return;
                         if (end < self->symbols_.size()) {
                             self->subscribe_delay_timer_.expires_after(
                                     std::chrono::milliseconds(250));  // 4 msg/sec
                             self->subscribe_delay_timer_.async_wait(
                                     [self, end](beast::error_code ec2) {
                                         if (!ec2 && self->running_) self->subscribe_next(end);
                                     });
                         } else {
                             std::cout << "[BitunixPubWS] all subscribed\n";
                             self->send_ping();
                         }
                     });
}

void BitunixPublicWS::send_ping() {
    if (!running_ || !ws_) return;
    uint64_t ts = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    std::ostringstream msg;
    msg << R"({"op":"ping","ping":)" << ts << "}";
    ws_->text(true);
    ws_->async_write(net::buffer(msg.str()),
                     [self = shared_from_this()](beast::error_code ec, std::size_t) {
                         if (ec) return;
                         self->ping_timer_.expires_after(std::chrono::seconds(10));
                         self->ping_timer_.async_wait([self](beast::error_code ec) {
                             if (!ec && self->running_) self->send_ping();
                         });
                     });
}

void BitunixPublicWS::on_read(beast::error_code ec, std::size_t bytes) {
    if (!running_ || !ws_) return;
    if (ec) {
        std::cerr << "[BitunixPubWS] on_read error: " << ec.message()
                  << " (" << ec.value() << ")\n";
        reconnect();
        return;
    }
    auto buffers = buffer_.data();
    if (buffers.size() > 0)
        process_message((const char*)buffers.data(), buffers.size());
    buffer_.consume(bytes);
    ws_->async_read(buffer_,
                    beast::bind_front_handler(&BitunixPublicWS::on_read, shared_from_this()));
}

void BitunixPublicWS::on_write(beast::error_code ec, std::size_t) {
    if (ec) std::cerr << "[BitunixPubWS] write error: " << ec.message() << "\n";
}

void BitunixPublicWS::process_message(const char* data, std::size_t size) {
    if (size + simdjson::SIMDJSON_PADDING > json_buffer_.capacity())
        json_buffer_.resize(size + simdjson::SIMDJSON_PADDING);
    std::memcpy(json_buffer_.data(), data, size);

    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    if (parser.parse(json_buffer_.data(), size, size).get(doc) != simdjson::SUCCESS) return;

    // ── RAW debug (ПОСЛЕ parse, один раз на каждый ch) ──
    {
        static std::unordered_set<std::string> seen_channels;
        std::string_view dbg_ch;
        if (doc["ch"].get_string().get(dbg_ch) == simdjson::SUCCESS) {
            std::string ch_str(dbg_ch);
            if (seen_channels.insert(ch_str).second) {
                std::string raw(json_buffer_.data(), size);
                if (raw.size() > 500) raw = raw.substr(0, 500) + "...";
                std::cout << "[BitunixPubWS] RAW ch=" << ch_str
                          << " sample: " << raw << "\n";
            }
        }
    }

    // ── op-сообщения (pong, подтверждение подписки) ──
    std::string_view op;
    if (doc["op"].get_string().get(op) == simdjson::SUCCESS) {
        return;
    }

    // ── ch-сообщения (data-push) ──
    std::string_view ch;
    if (doc["ch"].get_string().get(ch) != simdjson::SUCCESS) return;

    std::string_view symbol;
    if (doc["symbol"].get_string().get(symbol) != simdjson::SUCCESS) return;

    simdjson::dom::element data_el;
    if (doc["data"].get(data_el) != simdjson::SUCCESS) return;

    // ═══════════════════════════════════════════════════════════
    // depth_book1 (BBO) — существующая логика, без изменений
    // ═══════════════════════════════════════════════════════════
    if (ch == "depth_book1") {
        simdjson::dom::array bids, asks;
        if (data_el["b"].get_array().get(bids) != simdjson::SUCCESS) return;
        if (data_el["a"].get_array().get(asks) != simdjson::SUCCESS) return;

        auto parse_top = [](simdjson::dom::array arr,
                            double& price, double& size) -> bool {
            for (auto el : arr) {
                simdjson::dom::array pair;
                if (el.get_array().get(pair) != simdjson::SUCCESS) continue;
                if (pair.size() < 2) continue;
                std::string_view p, q;
                if (pair.at(0).get_string().get(p) == simdjson::SUCCESS &&
                    pair.at(1).get_string().get(q) == simdjson::SUCCESS) {
                    price = std::strtod(p.data(), nullptr);
                    size  = std::strtod(q.data(), nullptr);
                    return price > 0.0;
                }
            }
            return false;
        };

        double bid = 0, ask = 0, bs = 0, as = 0;
        bool ok_b = parse_top(bids, bid, bs);
        bool ok_a = parse_top(asks, ask, as);
        if (!ok_b || !ok_a) return;

        if (on_book_) on_book_(std::string(symbol), bid, ask, bs, as);
        last_bbo_[std::string(symbol)] = {bid, ask};

        if (ch_writer_) {
            uint64_t ts_ev = 0;
            (void)doc["ts"].get_uint64().get(ts_ev);
            ch_writer_->push_book(ch::VenueBitunix, std::string(symbol),
                                  ch::now_ms(), ts_ev, bid, ask, bs, as);
        }

        return;
    }

    // ═══════════════════════════════════════════════════════════
    // trade — НОВАЯ ветка
    // ═══════════════════════════════════════════════════════════
    if (ch == "trade") {
        auto parse_one = [&](simdjson::dom::element trade_el) {
            std::string_view p_str, v_str, side_str;

            // price: "p" | "price"
            if (trade_el["p"].get_string().get(p_str) != simdjson::SUCCESS &&
                trade_el["price"].get_string().get(p_str) != simdjson::SUCCESS) return;

            // size: "v" | "qty" | "volume" | "size"
            if (trade_el["v"].get_string().get(v_str) != simdjson::SUCCESS &&
                trade_el["qty"].get_string().get(v_str) != simdjson::SUCCESS &&
                trade_el["volume"].get_string().get(v_str) != simdjson::SUCCESS &&
                trade_el["size"].get_string().get(v_str) != simdjson::SUCCESS) return;

            double px = std::strtod(p_str.data(), nullptr);
            double sz = std::strtod(v_str.data(), nullptr);
            if (px <= 0 || sz <= 0) return;

            // side = сторона ТАКЕРА. "SELL" → агрессор продаёт → maker=buy → is_buyer_maker=true
            bool is_buyer_maker = false;
            if (trade_el["s"].get_string().get(side_str) == simdjson::SUCCESS) {
                // Bitunix: s="sell"|"buy" — это taker side
                // taker=sell → maker=buy → is_buyer_maker=true
                is_buyer_maker = (side_str == "sell" || side_str == "SELL" || side_str == "Sell");
            } else if (trade_el["side"].get_string().get(side_str) == simdjson::SUCCESS) {
                is_buyer_maker = (side_str == "SELL" || side_str == "Sell" || side_str == "sell");
            } else if (trade_el["S"].get_string().get(side_str) == simdjson::SUCCESS) {
                is_buyer_maker = (side_str == "Sell" || side_str == "SELL");
            }

            // BBO на момент получения
            auto bbo_it = last_bbo_.find(std::string(symbol));
            double bbo_bid = bbo_it != last_bbo_.end() ? bbo_it->second.first  : 0.0;
            double bbo_ask = bbo_it != last_bbo_.end() ? bbo_it->second.second : 0.0;

            bool is_through = false;
            if (bbo_bid > 0 && bbo_ask > 0) {
                if (!is_buyer_maker && px > bbo_ask) is_through = true;
                if ( is_buyer_maker && px < bbo_bid) is_through = true;
            }

            if (ch_writer_) {
                uint64_t ts_event = 0;
                (void)doc["ts"].get_uint64().get(ts_event);   // top-level, точнее чем per-trade t
                uint64_t tid_hash = ch::fnv1a(
                        "bnx|" + std::string(symbol) + "|" + std::to_string(ts_event)
                        + "|" + std::to_string(px) + "|" + std::to_string(sz)
                        + "|" + (is_buyer_maker ? "S" : "B"));

                ch_writer_->push_trade(ch::VenueBitunix, std::string(symbol),
                                       ch::now_ms(), ts_event,
                                       px, sz,
                                       is_buyer_maker ? -1 : +1,
                                       is_through ? 1 : 0,
                                       tid_hash,
                                       bbo_bid, bbo_ask);
            }

            if (on_trade_) on_trade_(std::string(symbol), px, sz, is_buyer_maker);
        };

        // data может быть массивом или единичным объектом
        simdjson::dom::array arr;
        if (data_el.get_array().get(arr) == simdjson::SUCCESS) {
            for (auto el : arr) parse_one(el);
        } else {
            simdjson::dom::object obj;
            if (data_el.get_object().get(obj) == simdjson::SUCCESS) {
                parse_one(data_el);
            }
        }
        return;
    }
}

void BitunixPublicWS::reconnect() {
    if (!running_) return;
    std::cerr << "[BitunixPubWS] RECONNECT attempt=" << (reconnect_attempts_+1) << "\n";
    if (++reconnect_attempts_ > 10) {
        std::cerr << "[BitunixPubWS] max reconnect attempts\n";
        return;
    }
    if (ws_) {
        beast::error_code ec;
        if (ws_->is_open()) ws_->close(websocket::close_code::normal, ec);
        ws_.reset();
    }
    buffer_.clear();
    reconnect_timer_.expires_after(std::chrono::seconds(5));
    reconnect_timer_.async_wait(
            beast::bind_front_handler(&BitunixPublicWS::on_reconnect_timer, shared_from_this()));
}

void BitunixPublicWS::on_reconnect_timer(beast::error_code ec) {
    if (ec) return;
    create_ws();
    resolver_.async_resolve(host_, "443",
                            beast::bind_front_handler(&BitunixPublicWS::on_resolve, shared_from_this()));
}