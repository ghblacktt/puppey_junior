#include "bybit_private_ws.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <simdjson.h>
#include <nlohmann/json.hpp>

BybitPrivateWSClient::BybitPrivateWSClient(
        net::io_context& ioc, ssl::context& ctx,
        const std::string& api_key, const std::string& api_secret,
        PositionCallback on_position,
        ExecutionCallback on_execution,
        OrderCallback on_order,
        SubscribeCallback on_subscribed)
        : ioc_(ioc), ctx_(ctx), resolver_(net::make_strand(ioc)),
          ping_timer_(net::make_strand(ioc)),
          reconnect_timer_(net::make_strand(ioc)),
          api_key_(api_key), api_secret_(api_secret),
          on_position_(std::move(on_position)),
          on_execution_(std::move(on_execution)),
          on_order_(std::move(on_order)),
          on_subscribed_(std::move(on_subscribed)) {
    json_buffer_.resize(65536 + simdjson::SIMDJSON_PADDING);
}

void BybitPrivateWSClient::subscribe_topics() {
    // all-in-one topics: order, execution, position
    nlohmann::json j = {{"op","subscribe"},{"args", {"order","execution","position"}}};
    std::string msg = j.dump();
    ws_->text(true);
    ws_->async_write(net::buffer(msg),
                     [self = shared_from_this()](beast::error_code ec, std::size_t) {
                         if (ec) { std::cerr << "[PrivWS] subscribe err\n"; return; }
                         std::cout << "[PrivWS] subscribed order/execution/position\n";
                         if (self->on_subscribed_) self->on_subscribed_();
                         self->send_ping();
                     });
}

void BybitPrivateWSClient::run(const std::string& host) {
    host_ = host;
    running_ = true;
    create_websocket();
    resolver_.async_resolve(host, "443",
                            beast::bind_front_handler(&BybitPrivateWSClient::on_resolve, shared_from_this()));
}

void BybitPrivateWSClient::stop() {
    running_ = false;
    ping_timer_.cancel();
    if (ws_) {
        beast::error_code ec;
        if (ws_->is_open()) ws_->close(websocket::close_code::normal, ec);
        ws_.reset();
    }
}

void BybitPrivateWSClient::create_websocket() {
    ws_ = std::make_unique<websocket::stream<beast::ssl_stream<beast::tcp_stream>>>(
            net::make_strand(ioc_), ctx_);
    ws_->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
    ws_->set_option(websocket::stream_base::decorator(
            [](websocket::request_type& req) {
                req.set(boost::beast::http::field::user_agent, "HFT-Cpp-Client");
            }));
}

void BybitPrivateWSClient::on_resolve(beast::error_code ec, const tcp::resolver::results_type& results) {
    if (ec) {
        std::cerr << "[PrivWS] Resolve error: " << ec.message() << "\n";
        reconnect();
        return;
    }
    auto& tcp_stream = ws_->next_layer().next_layer();
    net::async_connect(tcp_stream.socket(), results,
                       [self = shared_from_this()](beast::error_code ec, const tcp::resolver::results_type::endpoint_type) {
                           self->on_connect(ec);
                       });
}

void BybitPrivateWSClient::on_connect(beast::error_code ec) {
    if (ec) {
        std::cerr << "[PrivWS] Connect error: " << ec.message() << "\n";
        reconnect();
        return;
    }
    if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(), host_.c_str())) {
        std::cerr << "[PrivWS] SNI error\n";
        reconnect();
        return;
    }
    ws_->next_layer().async_handshake(ssl::stream_base::client,
                                      beast::bind_front_handler(&BybitPrivateWSClient::on_ssl_handshake, shared_from_this()));
}

void BybitPrivateWSClient::on_ssl_handshake(beast::error_code ec) {
    if (ec) {
        std::cerr << "[PrivWS] SSL handshake error: " << ec.message() << "\n";
        reconnect();
        return;
    }
    ws_->async_handshake(host_, "/v5/private",
                         beast::bind_front_handler(&BybitPrivateWSClient::on_handshake, shared_from_this()));
}

void BybitPrivateWSClient::on_handshake(beast::error_code ec) {
    if (ec) {
        std::cerr << "[PrivWS] WS handshake error: " << ec.message() << "\n";
        reconnect();
        return;
    }
    std::cout << "[PrivWS] Connected to " << host_ << "\n";
    reconnect_attempts_ = 0;
    send_auth();
    ws_->async_read(buffer_,
                    beast::bind_front_handler(&BybitPrivateWSClient::on_read, shared_from_this()));
}

uint64_t BybitPrivateWSClient::current_time_ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string BybitPrivateWSClient::hmac_sha256(const std::string& data) const {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(EVP_sha256(), api_secret_.c_str(), (int)api_secret_.length(),
         reinterpret_cast<const unsigned char*>(data.c_str()), data.length(),
         digest, &len);
    std::stringstream ss;
    for (unsigned int i = 0; i < len; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
    }
    return ss.str();
}

void BybitPrivateWSClient::send_auth() {
    uint64_t expires = current_time_ms() + 5000;
    std::string sign_str = "GET/realtime" + std::to_string(expires);
    std::string signature = hmac_sha256(sign_str);

    std::string auth_msg = R"({"op":"auth","args":[")" + api_key_ + R"(",)"
                           + std::to_string(expires) + R"(,")" + signature + R"("]})";

    ws_->text(true);
    ws_->async_write(net::buffer(auth_msg),
                     beast::bind_front_handler(&BybitPrivateWSClient::on_write, shared_from_this()));
}

void BybitPrivateWSClient::subscribe_positions() {
    std::string sub_msg = R"({"op":"subscribe","args":["position"]})";
    ws_->text(true);
    ws_->async_write(net::buffer(sub_msg),
                     [self = shared_from_this()](beast::error_code ec, std::size_t) {
                         if (ec) {
                             std::cerr << "[PrivWS] Subscribe error: " << ec.message() << "\n";
                             return;
                         }
                         std::cout << "[PrivWS] Subscribed to position topic\n";
                         if (self->on_subscribed_) self->on_subscribed_();
                         self->send_ping();
                     });
}

void BybitPrivateWSClient::on_write(beast::error_code ec, std::size_t) {
    if (ec) {
        std::cerr << "[PrivWS] Write error: " << ec.message() << "\n";
    }
}

void BybitPrivateWSClient::send_ping() {
    std::string ping_msg = R"({"op":"ping"})";
    ws_->text(true);
    ws_->async_write(net::buffer(ping_msg),
                     [self = shared_from_this()](beast::error_code ec, std::size_t) {
                         if (ec) return;
                         self->ping_timer_.expires_after(std::chrono::seconds(20));
                         self->ping_timer_.async_wait([self](beast::error_code ec) {
                             if (!ec) self->send_ping();
                         });
                     });
}

void BybitPrivateWSClient::on_read(beast::error_code ec, std::size_t bytes) {
    if (ec) {
        std::cerr << "[PrivWS] Read error: " << ec.message() << "\n";
        reconnect();
        return;
    }

    auto buffers = buffer_.data();
    if (buffers.size() > 0) {
        const char* data = static_cast<const char*>(buffers.data());
        process_message(data, buffers.size());
    }

    buffer_.consume(bytes);
    ws_->async_read(buffer_,
                    beast::bind_front_handler(&BybitPrivateWSClient::on_read, shared_from_this()));
}

void BybitPrivateWSClient::process_message(const char* data, std::size_t size) {
    if (size + simdjson::SIMDJSON_PADDING > json_buffer_.capacity()) {
        json_buffer_.resize(size + simdjson::SIMDJSON_PADDING);
    }
    std::memcpy(json_buffer_.data(), data, size);

    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    if (parser.parse(json_buffer_.data(), size, size).get(doc) != simdjson::SUCCESS) return;

    // Control message
    std::string_view op;

    if (doc["op"].get_string().get(op) == simdjson::SUCCESS) {
        bool success = false;
        doc["success"].get_bool().get(success);
        std::string_view ret_msg;
        doc["ret_msg"].get_string().get(ret_msg);

        if (op == "auth") {
            if (success) {
                authenticated_ = true;
                std::cout << "[PrivWS] Authenticated, subscribing positions\n";
                subscribe_positions();
            } else {
                std::cerr << "[PrivWS] Auth failed: " << ret_msg << "\n";
            }
        } else if (op == "pong" || op == "subscribe") {
            // ignore
        }
        return;
    }


    // Data message
    std::string_view topic;
    if (doc["topic"].get_string().get(topic) != simdjson::SUCCESS) return;

    if (topic == "position") {
        simdjson::dom::element data_elem;
        if (doc["data"].get(data_elem) != simdjson::SUCCESS) return;
        simdjson::dom::array data_arr;
        if (data_elem.get_array().get(data_arr) != simdjson::SUCCESS) return;

        for (auto pos : data_arr) {
            std::string_view symbol, side, size_str, avg_price_str, pnl_str;
            if (pos["symbol"].get_string().get(symbol) != simdjson::SUCCESS) continue;
            if (pos["side"].get_string().get(side) != simdjson::SUCCESS) continue;
            if (pos["size"].get_string().get(size_str) != simdjson::SUCCESS) continue;

            double sz = std::strtod(size_str.data(), nullptr);

            double avg_price = 0.0;
            if (pos["avgPrice"].get_string().get(avg_price_str) == simdjson::SUCCESS) {
                avg_price = std::strtod(avg_price_str.data(), nullptr);
            }

            double pnl = 0.0;
            if (pos["unrealisedPnl"].get_string().get(pnl_str) == simdjson::SUCCESS) {
                pnl = std::strtod(pnl_str.data(), nullptr);
            }

            on_position_(std::string(symbol), std::string(side), sz, avg_price, pnl);
        }
    }

}

void BybitPrivateWSClient::reconnect() {
    if (!running_) return;
    if (++reconnect_attempts_ > 10) {
        std::cerr << "[PrivWS] Max reconnect attempts reached\n";
        return;
    }

    if (ws_) {
        beast::error_code ec;
        if (ws_->is_open()) ws_->close(websocket::close_code::normal, ec);
        ws_.reset();
    }
    buffer_.clear();
    authenticated_ = false;

    ping_timer_.expires_after(std::chrono::seconds(5));
    ping_timer_.async_wait(
            beast::bind_front_handler(&BybitPrivateWSClient::on_reconnect_timer, shared_from_this()));
}

void BybitPrivateWSClient::on_reconnect_timer(beast::error_code ec) {
    if (ec) return;
    create_websocket();
    resolver_.async_resolve(host_, "443",
                            beast::bind_front_handler(&BybitPrivateWSClient::on_resolve, shared_from_this()));
}