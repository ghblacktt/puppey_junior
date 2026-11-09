#include "bitunix_private_ws.h"
#include <simdjson.h>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <random>
#include <openssl/sha.h>

std::string BitunixPrivateWS::sha256_hex(const std::string& input) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.c_str()), input.length(), digest);
    std::stringstream ss;
    for (unsigned char c : digest) ss << std::hex << std::setw(2) << std::setfill('0') << (int)c;
    return ss.str();
}

std::string BitunixPrivateWS::generate_nonce() {
    static const char hex[] = "0123456789abcdef";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 15);
    std::string nonce(32, '0');
    for (auto& c : nonce) c = hex[dis(gen)];
    return nonce;
}

uint64_t BitunixPrivateWS::now_ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
}

uint64_t BitunixPrivateWS::now_sec() const {
    return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
}

BitunixPrivateWS::BitunixPrivateWS(
        net::io_context& ioc, ssl::context& ctx,
        const std::string& api_key,
        const std::string& api_secret,
        OrderCallback on_order,
        PositionCallback on_position,
        SubscribeCallback on_subscribed)
        : ioc_(ioc), ctx_(ctx),
          resolver_(net::make_strand(ioc)),
          ping_timer_(net::make_strand(ioc)),
          reconnect_timer_(net::make_strand(ioc)),
          subscribe_delay_timer_(net::make_strand(ioc)),
          api_key_(api_key), api_secret_(api_secret),
          on_order_(std::move(on_order)),
          on_position_(std::move(on_position)),
          on_subscribed_(std::move(on_subscribed)) {
    json_buffer_.resize(65536 + simdjson::SIMDJSON_PADDING);
}

void BitunixPrivateWS::run(const std::string& host) {
    host_ = host;
    running_ = true;
    create_ws();
    resolver_.async_resolve(host_, "443",
                            beast::bind_front_handler(&BitunixPrivateWS::on_resolve, shared_from_this()));
}

void BitunixPrivateWS::stop() {
    if (!running_.exchange(false)) return;   // уже остановлен
    boost::asio::post(ioc_, [self = shared_from_this()]() {
        self->ping_timer_.cancel();
        self->reconnect_timer_.cancel();
        self->subscribe_delay_timer_.cancel();
        if (self->ws_) {
            beast::error_code ec;
            if (self->ws_->is_open())
                self->ws_->close(websocket::close_code::normal, ec);
            self->ws_.reset();
        }
    });
}

void BitunixPrivateWS::create_ws() {
    ws_ = std::make_unique<websocket::stream<beast::ssl_stream<beast::tcp_stream>>>(
            net::make_strand(ioc_), ctx_);
    websocket::stream_base::timeout t;
    t.handshake_timeout = std::chrono::seconds(30);
    t.idle_timeout      = std::chrono::seconds(600);
    t.keep_alive_pings  = false;
    ws_->set_option(t);
    beast::get_lowest_layer(*ws_).expires_never();   // ← ДОБАВЬ
    ws_->set_option(websocket::stream_base::decorator([](websocket::request_type& req) {
        req.set(boost::beast::http::field::user_agent, "HFT-Cpp-PrivateWS");
    }));
}

void BitunixPrivateWS::on_resolve(beast::error_code ec, const tcp::resolver::results_type& results) {
    if (ec) { reconnect(); return; }
    net::async_connect(ws_->next_layer().next_layer().socket(), results,
                       beast::bind_front_handler(&BitunixPrivateWS::on_connect, shared_from_this()));
}

void BitunixPrivateWS::on_connect(beast::error_code ec, const tcp::resolver::results_type::endpoint_type) {
    if (ec) { reconnect(); return; }
    if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(), host_.c_str())) {
        reconnect(); return;
    }
    ws_->next_layer().async_handshake(ssl::stream_base::client,
                                      beast::bind_front_handler(&BitunixPrivateWS::on_ssl_handshake, shared_from_this()));
}

void BitunixPrivateWS::on_ssl_handshake(beast::error_code ec) {
    if (ec) { reconnect(); return; }
    ws_->async_handshake(host_, "/private/",
                         beast::bind_front_handler(&BitunixPrivateWS::on_handshake, shared_from_this()));
}

void BitunixPrivateWS::on_handshake(beast::error_code ec) {
    if (ec) { reconnect(); return; }
    reconnect_attempts_ = 0;
    std::cout << "[BitunixPrivWS] connected\n";
    send_login();
    ws_->async_read(buffer_,
                    beast::bind_front_handler(&BitunixPrivateWS::on_read, shared_from_this()));
}

void BitunixPrivateWS::send_login() {
    if (!running_ || !ws_) return;
    uint64_t ts = now_sec();
    std::string nonce = generate_nonce();
    // WS login signature: SHA256(SHA256(nonce + timestamp + apiKey) + secretKey)
    std::string digest = sha256_hex(nonce + std::to_string(ts) + api_key_);
    std::string sign = sha256_hex(digest + api_secret_);

    std::ostringstream msg;
    msg << R"({"op":"login","args":[{"apiKey":")" << api_key_
        << R"(","timestamp":)" << ts
        << R"(,"nonce":")" << nonce
        << R"(","sign":")" << sign << R"("}]})";

    ws_->text(true);
    ws_->async_write(net::buffer(msg.str()),
                     beast::bind_front_handler(&BitunixPrivateWS::on_write, shared_from_this()));
}

void BitunixPrivateWS::subscribe() {
    if (!running_ || !ws_) return;
    std::string msg = R"({"op":"subscribe","args":[{"ch":"order"},{"ch":"position"}]})";
    ws_->text(true);
    ws_->async_write(net::buffer(msg),
                     [self = shared_from_this()](beast::error_code ec, std::size_t) {
                         if (ec) { std::cerr << "[BitunixPrivWS] subscribe error\n"; return; }
                         std::cout << "[BitunixPrivWS] subscribed order/position\n";
                         if (self->on_subscribed_) self->on_subscribed_();
                         self->send_ping();
                     });
}

void BitunixPrivateWS::send_ping() {
    if (!running_ || !ws_) return;
    uint64_t ts = now_sec();
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

void BitunixPrivateWS::on_read(beast::error_code ec, std::size_t bytes) {
    if (ec) { reconnect(); return; }
    if (!running_ || !ws_) return;
    auto buffers = buffer_.data();
    if (buffers.size() > 0)
        process_message((const char*)buffers.data(), buffers.size());
    buffer_.consume(bytes);
    ws_->async_read(buffer_,
                    beast::bind_front_handler(&BitunixPrivateWS::on_read, shared_from_this()));
}

void BitunixPrivateWS::on_write(beast::error_code ec, std::size_t) {
    if (ec) std::cerr << "[BitunixPrivWS] write error: " << ec.message() << "\n";
}

void BitunixPrivateWS::process_message(const char* data, std::size_t size) {
    if (size + simdjson::SIMDJSON_PADDING > json_buffer_.capacity())
        json_buffer_.resize(size + simdjson::SIMDJSON_PADDING);
    std::memcpy(json_buffer_.data(), data, size);

    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    if (parser.parse(json_buffer_.data(), size, size).get(doc) != simdjson::SUCCESS) return;

    // login response
    std::string_view op;
    if (doc["op"].get_string().get(op) == simdjson::SUCCESS) {
        if (op == "login") {
            std::string raw(json_buffer_.data(), size);
            std::cerr << "[BitunixPrivWS] LOGIN RESPONSE: " << raw << "\n";

            int64_t code = 0;
            bool ok = false;

            if (doc["code"].get_int64().get(code) == simdjson::SUCCESS) {
                ok = (code == 0);
                std::cerr << "[BitunixPrivWS] code=" << code << "\n";
            } else if (doc["retCode"].get_int64().get(code) == simdjson::SUCCESS) {
                ok = (code == 0);
                std::cerr << "[BitunixPrivWS] retCode=" << code << "\n";
            } else {
                // Проверяем наличие поля "data" без прямого сравнения результата
                simdjson::dom::object obj;
                if (doc["data"].get_object().get(obj) == simdjson::SUCCESS) {
                    ok = true;
                    std::cerr << "[BitunixPrivWS] data field present, treat as success\n";
                }
            }

            std::string_view msg;
            if (doc["msg"].get_string().get(msg) == simdjson::SUCCESS)
                std::cerr << "[BitunixPrivWS] msg=" << msg << "\n";
            else if (doc["retMsg"].get_string().get(msg) == simdjson::SUCCESS)
                std::cerr << "[BitunixPrivWS] retMsg=" << msg << "\n";

            if (ok) {
                authenticated_ = true;
                std::cout << "[BitunixPrivWS] authenticated\n";
                subscribe();
            } else {
                std::cerr << "[BitunixPrivWS] login failed (see LOGIN RESPONSE above)\n";
            }
            return;
        }
        return;
    }

    std::string_view ch;
    if (doc["ch"].get_string().get(ch) != simdjson::SUCCESS) return;

    simdjson::dom::element data_el;
    if (doc["data"].get(data_el) != simdjson::SUCCESS) return;

    if (ch == "order") {
        simdjson::dom::object obj;
        if (data_el.get_object().get(obj) == simdjson::SUCCESS) {
            std::string_view order_id, symbol, side, status, price_s, qty_s, deal_s;
            if (obj["orderId"].get_string().get(order_id) != simdjson::SUCCESS) return;
            (void)obj["symbol"].get_string().get(symbol);
            (void)obj["side"].get_string().get(side);
            (void)obj["orderStatus"].get_string().get(status);
            (void)obj["price"].get_string().get(price_s);
            (void)obj["qty"].get_string().get(qty_s);
            (void)obj["dealAmount"].get_string().get(deal_s);

            double price = price_s.empty() ? 0.0 : std::stod(std::string(price_s));
            double qty = qty_s.empty() ? 0.0 : std::stod(std::string(qty_s));
            double deal = deal_s.empty() ? 0.0 : std::stod(std::string(deal_s));

            if (on_order_)
                on_order_(std::string(order_id), std::string(symbol),
                          std::string(side), std::string(status),
                          price, qty, deal);
        }
    }
    else if (ch == "position") {
        simdjson::dom::object obj;
        if (data_el.get_object().get(obj) == simdjson::SUCCESS) {
            std::string_view symbol, side, size_s, avg_s, pnl_s;
            if (obj["symbol"].get_string().get(symbol) != simdjson::SUCCESS) return;
            (void)obj["side"].get_string().get(side);
            (void)obj["qty"].get_string().get(size_s);
            (void)obj["avgPrice"].get_string().get(avg_s);
            (void)obj["unrealizedPNL"].get_string().get(pnl_s);

            double size = size_s.empty() ? 0.0 : std::stod(std::string(size_s));
            double avg = avg_s.empty() ? 0.0 : std::stod(std::string(avg_s));
            double pnl = pnl_s.empty() ? 0.0 : std::stod(std::string(pnl_s));

            if (on_position_)
                on_position_(std::string(symbol), std::string(side), size, avg, pnl);
        }
    }
}

void BitunixPrivateWS::reconnect() {
    if (!running_) return;
    if (++reconnect_attempts_ > 10) {
        std::cerr << "[BitunixPrivWS] max reconnect attempts\n";
        return;
    }
    authenticated_ = false;
    if (ws_) {
        beast::error_code ec;
        if (ws_->is_open()) ws_->close(websocket::close_code::normal, ec);
        ws_.reset();
    }
    buffer_.clear();
    reconnect_timer_.expires_after(std::chrono::seconds(5));
    reconnect_timer_.async_wait(
            beast::bind_front_handler(&BitunixPrivateWS::on_reconnect_timer, shared_from_this()));
}

void BitunixPrivateWS::on_reconnect_timer(beast::error_code ec) {
    if (ec) return;
    create_ws();
    resolver_.async_resolve(host_, "443",
                            beast::bind_front_handler(&BitunixPrivateWS::on_resolve, shared_from_this()));
}