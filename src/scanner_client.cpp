#include "scanner_client.h"
#include <simdjson.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <cstring>

namespace {
    inline uint64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
    }
}

ScannerClient::ScannerClient(net::io_context& ioc, ssl::context& ctx,
                             const std::vector<std::string>& symbols,
                             Screener& screener)
        : ioc_(ioc), ctx_(ctx),
          resolver_(net::make_strand(ioc)),
          ping_timer_(net::make_strand(ioc)),
          reconnect_timer_(net::make_strand(ioc)),
          subscribe_delay_timer_(net::make_strand(ioc)),
          symbols_(symbols),
          screener_(screener) {
    json_buffer_.resize(65536 + simdjson::SIMDJSON_PADDING);
    std::cout << "[Scanner] constructed with " << symbols_.size() << " symbols\n";
}

ScannerClient::~ScannerClient() {
    stop();

    book_writer_running_ = false;
    if (book_writer_thread_.joinable()) book_writer_thread_.join();

    trade_writer_running_ = false;
    if (trade_writer_thread_.joinable()) trade_writer_thread_.join();
}

void ScannerClient::run(const std::string& host) {
    host_ = host;
    running_ = true;

    // Стартуем writer threads (они работают всегда, пока running_ = true)
    book_writer_running_ = true;
    book_writer_thread_ = std::thread([this]() { book_writer_loop(); });

    trade_writer_running_ = true;
    trade_writer_thread_ = std::thread([this]() { trade_writer_loop(); });

    create_ws();
    resolver_.async_resolve(host, "443",
                            beast::bind_front_handler(&ScannerClient::on_resolve, shared_from_this()));
}

void ScannerClient::stop() {
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

// ═══════════════════════════════════════════════════════════════
// ClickHouse writers
// ═══════════════════════════════════════════════════════════════
void ScannerClient::add_book_update(const OrderbookUpdate& u) {
    std::lock_guard<std::mutex> lock(book_queue_mutex_);
    book_queue_.push_back(u);
}

void ScannerClient::add_trade_tick(const TradeTick& t) {
    std::lock_guard<std::mutex> lock(trade_queue_mutex_);
    trade_queue_.push_back(t);
}

void ScannerClient::book_writer_loop() {
    std::vector<OrderbookUpdate> batch;
    batch.reserve(5000);
    std::cout << "[CH-Book] writer thread started\n";

    while (book_writer_running_) {
        {
            std::lock_guard<std::mutex> lock(book_queue_mutex_);
            while (!book_queue_.empty() && batch.size() < 5000) {
                batch.push_back(std::move(book_queue_.front()));
                book_queue_.pop_front();
            }
        }
        if (!batch.empty()) {
            send_batch_books_to_clickhouse(batch);
            batch.clear();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::cout << "[CH-Book] writer thread stopped\n";
}

void ScannerClient::trade_writer_loop() {
    std::vector<TradeTick> batch;
    batch.reserve(5000);
    std::cout << "[CH-Trade] writer thread started\n";

    while (trade_writer_running_) {
        {
            std::lock_guard<std::mutex> lock(trade_queue_mutex_);
            while (!trade_queue_.empty() && batch.size() < 5000) {
                batch.push_back(std::move(trade_queue_.front()));
                trade_queue_.pop_front();
            }
        }
        if (!batch.empty()) {
            send_batch_trades_to_clickhouse(batch);
            batch.clear();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::cout << "[CH-Trade] writer thread stopped\n";
}

void ScannerClient::send_batch_books_to_clickhouse(
        const std::vector<OrderbookUpdate>& batch) {
    try {
        std::stringstream body;
        for (const auto& u : batch) {
            body << "{\"symbol\":\"" << u.symbol << "\","
                 << "\"timestamp\":" << u.timestamp_ms << ","
                 << "\"update_id\":" << u.update_id << ","
                 << "\"update_type\":\"" << u.update_type << "\","
                 << "\"bid_prices\":[";
            for (size_t i = 0; i < u.bid_prices.size(); ++i) {
                if (i > 0) body << ",";
                body << std::fixed << std::setprecision(8) << u.bid_prices[i];
            }
            body << "],\"bid_sizes\":[";
            for (size_t i = 0; i < u.bid_sizes.size(); ++i) {
                if (i > 0) body << ",";
                body << std::fixed << std::setprecision(8) << u.bid_sizes[i];
            }
            body << "],\"ask_prices\":[";
            for (size_t i = 0; i < u.ask_prices.size(); ++i) {
                if (i > 0) body << ",";
                body << std::fixed << std::setprecision(8) << u.ask_prices[i];
            }
            body << "],\"ask_sizes\":[";
            for (size_t i = 0; i < u.ask_sizes.size(); ++i) {
                if (i > 0) body << ",";
                body << std::fixed << std::setprecision(8) << u.ask_sizes[i];
            }
            body << "]}\n";
        }

        net::io_context ioc;
        tcp::resolver resolver(ioc);
        auto const results = resolver.resolve("localhost", "8123");
        beast::tcp_stream stream(ioc);
        stream.connect(results);
        http::request<http::string_body> req{
                http::verb::post,
                "/?query=INSERT%20INTO%20market_data.orderbook_updates%20FORMAT%20JSONEachRow",
                11};
        req.set(http::field::host, "localhost:8123");
        req.set(http::field::authorization, "Basic cm9ib3Q6cm9ib3RfcGFzcw==");
        req.set(http::field::content_type, "text/plain");
        req.body() = body.str();
        req.prepare_payload();
        http::write(stream, req);
        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(stream, buffer, res);
        beast::error_code ec;
        stream.socket().shutdown(tcp::socket::shutdown_both, ec);
    } catch (const std::exception& e) {
        std::cerr << "[CH-Book] error: " << e.what() << "\n";
    }
}

void ScannerClient::send_batch_trades_to_clickhouse(
        const std::vector<TradeTick>& batch) {
    try {
        std::stringstream body;
        for (const auto& t : batch) {
            body << "{\"symbol\":\"" << t.symbol << "\","
                 << "\"timestamp\":" << t.timestamp_ms << ","
                 << "\"price\":" << std::fixed << std::setprecision(8) << t.price << ","
                 << "\"volume\":" << std::fixed << std::setprecision(8) << t.volume << ","
                 << "\"is_buyer_maker\":" << (t.is_buyer_maker ? 1 : 0) << "}\n";
        }

        net::io_context ioc;
        tcp::resolver resolver(ioc);
        auto const results = resolver.resolve("localhost", "8123");
        beast::tcp_stream stream(ioc);
        stream.connect(results);
        http::request<http::string_body> req{
                http::verb::post,
                "/?query=INSERT%20INTO%20market_data.market_ticks%20FORMAT%20JSONEachRow",
                11};
        req.set(http::field::host, "localhost:8123");
        req.set(http::field::authorization, "Basic cm9ib3Q6cm9ib3RfcGFzcw==");
        req.set(http::field::content_type, "text/plain");
        req.body() = body.str();
        req.prepare_payload();
        http::write(stream, req);
        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(stream, buffer, res);
        beast::error_code ec;
        stream.socket().shutdown(tcp::socket::shutdown_both, ec);
    } catch (const std::exception& e) {
        std::cerr << "[CH-Trade] error: " << e.what() << "\n";
    }
}

// ═══════════════════════════════════════════════════════════════
// WS chain (без изменений от прошлой версии)
// ═══════════════════════════════════════════════════════════════
void ScannerClient::create_ws() {
    ws_ = std::make_unique<websocket::stream<beast::ssl_stream<beast::tcp_stream>>>(
            net::make_strand(ioc_), ctx_);
    ws_->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
}

void ScannerClient::on_resolve(beast::error_code ec, const tcp::resolver::results_type& results) {
    if (ec) { reconnect(); return; }
    net::async_connect(ws_->next_layer().next_layer().socket(), results,
                       beast::bind_front_handler(&ScannerClient::on_connect, shared_from_this()));
}

void ScannerClient::on_connect(beast::error_code ec,
                               const tcp::resolver::results_type::endpoint_type) {
    if (ec) { reconnect(); return; }
    if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(), host_.c_str())) {
        reconnect(); return;
    }
    ws_->next_layer().async_handshake(ssl::stream_base::client,
                                      beast::bind_front_handler(&ScannerClient::on_ssl_handshake, shared_from_this()));
}

void ScannerClient::on_ssl_handshake(beast::error_code ec) {
    if (ec) { reconnect(); return; }
    ws_->async_handshake(host_, "/v5/public/linear",
                         beast::bind_front_handler(&ScannerClient::on_handshake, shared_from_this()));
}

void ScannerClient::on_handshake(beast::error_code ec) {
    if (ec) { reconnect(); return; }
    reconnect_attempts_ = 0;
    reconnecting_ = false;
    std::cout << "[Scanner] WS handshake OK\n";
    ws_->async_read(buffer_,
                    beast::bind_front_handler(&ScannerClient::on_read, shared_from_this()));
    subscribe_next_batch(0);
}

void ScannerClient::subscribe_next_batch(size_t start_idx) {
    if (start_idx >= symbols_.size()) {
        std::cout << "[Scanner] all batches sent\n";
        send_ping();
        return;
    }

    constexpr size_t batch_size = 10;
    size_t end = std::min(start_idx + batch_size, symbols_.size());

    std::ostringstream args;
    args << "[";
    for (size_t i = start_idx; i < end; ++i) {
        if (i > start_idx) args << ",";
        args << "\"orderbook.50." << symbols_[i] << "\"";
        args << ",\"publicTrade." << symbols_[i] << "\"";
    }
    args << "]";
    std::string msg = R"({"op":"subscribe","args":)" + args.str() + "}";

    ws_->text(true);
    ws_->async_write(net::buffer(msg),
                     [self = shared_from_this(), end](beast::error_code ec, std::size_t) {
                         if (ec) { std::cerr << "[Scanner] write error\n"; return; }
                         if (end >= self->symbols_.size()) {
                             std::cout << "[Scanner] all batches sent\n";
                             self->send_ping();
                             return;
                         }
                         self->subscribe_delay_timer_.expires_after(std::chrono::milliseconds(1000));
                         self->subscribe_delay_timer_.async_wait([self, end](beast::error_code ec) {
                             if (!ec) self->subscribe_next_batch(end);
                         });
                     });
}

void ScannerClient::send_ping() {
    std::string msg = R"({"op":"ping"})";
    ws_->text(true);
    ws_->async_write(net::buffer(msg),
                     [self = shared_from_this()](beast::error_code ec, std::size_t) {
                         if (ec) return;
                         self->ping_timer_.expires_after(std::chrono::seconds(20));
                         self->ping_timer_.async_wait([self](beast::error_code ec) {
                             if (!ec && self->running_) self->send_ping();
                         });
                     });
}

void ScannerClient::on_read(beast::error_code ec, std::size_t bytes) {
    if (ec) { reconnect(); return; }
    auto buffers = buffer_.data();
    if (buffers.size() > 0)
        process_message((const char*)buffers.data(), buffers.size());
    buffer_.consume(bytes);
    ws_->async_read(buffer_,
                    beast::bind_front_handler(&ScannerClient::on_read, shared_from_this()));
}

void ScannerClient::on_write(beast::error_code ec, std::size_t) {
    if (ec) std::cerr << "[Scanner] write error: " << ec.message() << "\n";
}

// ═══════════════════════════════════════════════════════════════
// process_message — ключевая часть
// ═══════════════════════════════════════════════════════════════
void ScannerClient::process_message(const char* data, std::size_t size) {
    if (size + simdjson::SIMDJSON_PADDING > json_buffer_.capacity())
        json_buffer_.resize(size + simdjson::SIMDJSON_PADDING);
    std::memcpy(json_buffer_.data(), data, size);

    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    if (parser.parse(json_buffer_.data(), size, size).get(doc) != simdjson::SUCCESS) return;

    std::string_view topic;
    if (doc["topic"].get_string().get(topic) != simdjson::SUCCESS) return;

    auto last_dot = topic.find_last_of('.');
    if (last_dot == std::string_view::npos) return;
    std::string symbol(topic.substr(last_dot + 1));

    simdjson::dom::element data_field;
    if (doc["data"].get(data_field) != simdjson::SUCCESS) return;

    // ═══════════ ORDERBOOK ═══════════
    if (topic.find("orderbook") != std::string_view::npos) {
        // Биржевой timestamp
        uint64_t exchange_ts = 0;
        if (doc["ts"].get_uint64().get(exchange_ts) != simdjson::SUCCESS)
            exchange_ts = now_ms();

        // update_type
        std::string update_type = "delta";
        std::string_view type_sv;
        if (doc["type"].get_string().get(type_sv) == simdjson::SUCCESS)
            update_type = std::string(type_sv);

        // update_id
        uint64_t update_id = 0;
        if (data_field["u"].get_uint64().get(update_id) != simdjson::SUCCESS) {
            data_field["seq"].get_uint64().get(update_id);
        }

        // Парсим bids/asks
        std::vector<double> bp, bs, ap, as;
        simdjson::dom::array bids, asks;
        if (data_field["b"].get_array().get(bids) == simdjson::SUCCESS) {
            for (auto b : bids) {
                auto arr_res = b.get_array();
                if (arr_res.error()) continue;
                auto arr = arr_res.value();
                if (arr.size() < 2) continue;
                std::string_view p, q;
                if (arr.at(0).get_string().get(p) != simdjson::SUCCESS) continue;
                if (arr.at(1).get_string().get(q) != simdjson::SUCCESS) continue;
                bp.push_back(std::strtod(p.data(), nullptr));
                bs.push_back(std::strtod(q.data(), nullptr));
            }
        }
        if (data_field["a"].get_array().get(asks) == simdjson::SUCCESS) {
            for (auto a : asks) {
                auto arr_res = a.get_array();
                if (arr_res.error()) continue;
                auto arr = arr_res.value();
                if (arr.size() < 2) continue;
                std::string_view p, q;
                if (arr.at(0).get_string().get(p) != simdjson::SUCCESS) continue;
                if (arr.at(1).get_string().get(q) != simdjson::SUCCESS) continue;
                ap.push_back(std::strtod(p.data(), nullptr));
                as.push_back(std::strtod(q.data(), nullptr));
            }
        }

        // Сохраняем в ClickHouse
        if (book_writer_running_) {
            OrderbookUpdate u;
            u.symbol = symbol;
            u.timestamp_ms = exchange_ts;
            u.update_id = update_id;
            u.update_type = update_type;
            u.bid_prices = std::move(bp);
            u.bid_sizes  = std::move(bs);
            u.ask_prices = std::move(ap);
            u.ask_sizes  = std::move(as);
            add_book_update(u);
        }

        // Screener
        if (!bp.empty() && !ap.empty() && bp.size() >= 1 && ap.size() >= 1) {
            screener_.on_book_update(symbol, bp[0], ap[0], bs[0], as[0]);
        }
    }
        // ═══════════ TRADES ═══════════
    else if (topic.find("publicTrade") != std::string_view::npos) {
        simdjson::dom::array trades_arr;
        if (data_field.get_array().get(trades_arr) == simdjson::SUCCESS) {
            for (auto t : trades_arr) {
                std::string_view p, v, side;
                uint64_t ts = 0;
                if (t["p"].get_string().get(p) != simdjson::SUCCESS) continue;
                if (t["v"].get_string().get(v) != simdjson::SUCCESS) continue;
                if (t["T"].get_uint64().get(ts) != simdjson::SUCCESS) continue;

                TradeTick tick;
                tick.symbol = symbol;
                tick.timestamp_ms = ts;
                tick.price = std::strtod(p.data(), nullptr);
                tick.volume = std::strtod(v.data(), nullptr);
                tick.is_buyer_maker = false;
                if (t["S"].get_string().get(side) == simdjson::SUCCESS)
                    tick.is_buyer_maker = (side == "Sell");

                if (tick.price > 0 && tick.volume > 0 && trade_writer_running_) {
                    add_trade_tick(tick);
                }
            }
        }
    }
}

void ScannerClient::reconnect() {
    if (reconnecting_ || !running_) return;
    reconnecting_ = true;
    if (++reconnect_attempts_ > 20) {
        std::cerr << "[Scanner] max reconnect attempts\n";
        return;
    }
    ping_timer_.cancel();
    subscribe_delay_timer_.cancel();
    if (ws_) {
        beast::error_code ec;
        if (ws_->is_open()) ws_->close(websocket::close_code::normal, ec);
        ws_.reset();
    }
    buffer_.clear();
    reconnect_timer_.expires_after(std::chrono::seconds(5));
    reconnect_timer_.async_wait([self = shared_from_this()](beast::error_code ec) {
        if (ec) return;
        self->reconnecting_ = false;
        self->create_ws();
        self->resolver_.async_resolve(self->host_, "443",
                                      beast::bind_front_handler(&ScannerClient::on_resolve, self));
    });
}