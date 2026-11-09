#include "bybit/bybit_trade_ws.h"
#include "bybit/bybit_api_client.h"
#include <iostream>
#include <simdjson.h>

BybitTradeWSClient::BybitTradeWSClient(net::io_context& ioc, ssl::context& ctx,
                                       BybitApiClient& api, AckCallback on_ack)
        : ioc_(ioc), ctx_(ctx), api_(api),
          resolver_(net::make_strand(ioc)),
          ping_timer_(net::make_strand(ioc)),
          reconnect_timer_(net::make_strand(ioc)),
          on_ack_(std::move(on_ack)) {
    json_buffer_.resize(65536 + simdjson::SIMDJSON_PADDING);
}

void BybitTradeWSClient::run(const std::string& host) {
    host_ = host;
    running_ = true;
    create_websocket();
    resolver_.async_resolve(host_, "443",
                            beast::bind_front_handler(&BybitTradeWSClient::on_resolve, shared_from_this()));
}

void BybitTradeWSClient::stop() {
    running_ = false;
    ping_timer_.cancel();
    reconnect_timer_.cancel();
    if (ws_) { beast::error_code ec; if (ws_->is_open()) ws_->close(websocket::close_code::normal, ec); ws_.reset(); }
}

void BybitTradeWSClient::create_websocket() {
    ws_ = std::make_unique<websocket::stream<beast::ssl_stream<beast::tcp_stream>>>(
            net::make_strand(ioc_), ctx_);
    ws_->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
    ws_->set_option(websocket::stream_base::decorator([](websocket::request_type& req) {
        req.set(boost::beast::http::field::user_agent, "HFT-Cpp-TradeWS");
    }));
}

void BybitTradeWSClient::on_resolve(beast::error_code ec, const tcp::resolver::results_type& results) {
    if (ec) { reconnect(); return; }
    net::async_connect(ws_->next_layer().next_layer().socket(), results,
                       beast::bind_front_handler(&BybitTradeWSClient::on_connect, shared_from_this()));
}

void BybitTradeWSClient::on_connect(beast::error_code ec, const tcp::resolver::results_type::endpoint_type) {
    if (ec) { reconnect(); return; }
    if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(), host_.c_str())) { reconnect(); return; }
    ws_->next_layer().async_handshake(ssl::stream_base::client,
                                      beast::bind_front_handler(&BybitTradeWSClient::on_ssl_handshake, shared_from_this()));
}

void BybitTradeWSClient::on_ssl_handshake(beast::error_code ec) {
    if (ec) { reconnect(); return; }
    ws_->async_handshake(host_, "/v5/trade",
                         beast::bind_front_handler(&BybitTradeWSClient::on_handshake, shared_from_this()));
}

void BybitTradeWSClient::on_handshake(beast::error_code ec) {
    if (ec) { reconnect(); return; }
    reconnect_attempts_ = 0;
    send_auth();
    ws_->async_read(buffer_, beast::bind_front_handler(&BybitTradeWSClient::on_read, shared_from_this()));
}

void BybitTradeWSClient::send_auth() {
    uint64_t expires = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() + 5000;
    std::string sign = api_.sign_auth(std::to_string(expires));
    nlohmann::json j = {
            {"op", "auth"},
            {"args", {api_.get_api_key(), expires, sign}}
    };
    std::string msg = j.dump();
    ws_->text(true);
    ws_->async_write(net::buffer(msg),
                     beast::bind_front_handler(&BybitTradeWSClient::on_write, shared_from_this()));
}

void BybitTradeWSClient::create_order(const nlohmann::json& args) {
    nlohmann::json j = {
            {"reqId", next_req_id()},
            {"header", {{"X-BAPI-TIMESTAMP", std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count())}}},
            {"op", "order.create"},
            {"args", nlohmann::json::array({args})}
    };
    {
        std::lock_guard<std::mutex> lk(queue_mtx_);
        out_queue_.push_back(j.dump());
    }
    if (!write_in_progress_) {
        write_in_progress_ = true;
        do_write();
    }
}

void BybitTradeWSClient::cancel_order(const nlohmann::json& args) {
    nlohmann::json j = {
            {"reqId", next_req_id()},
            {"header", {{"X-BAPI-TIMESTAMP", std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count())}}},
            {"op", "order.cancel"},
            {"args", nlohmann::json::array({args})}
    };
    {
        std::lock_guard<std::mutex> lk(queue_mtx_);
        out_queue_.push_back(j.dump());
    }
    if (!write_in_progress_) { write_in_progress_ = true; do_write(); }
}

void BybitTradeWSClient::amend_order(const nlohmann::json& args) {
    nlohmann::json j = {
            {"reqId", next_req_id()},
            {"header", {{"X-BAPI-TIMESTAMP", std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count())}}},
            {"op", "order.amend"},
            {"args", nlohmann::json::array({args})}
    };
    {
        std::lock_guard<std::mutex> lk(queue_mtx_);
        out_queue_.push_back(j.dump());
    }
    if (!write_in_progress_) { write_in_progress_ = true; do_write(); }
}

void BybitTradeWSClient::do_write() {
    std::string msg;
    {
        std::lock_guard<std::mutex> lk(queue_mtx_);
        if (out_queue_.empty()) { write_in_progress_ = false; return; }
        msg = std::move(out_queue_.front());
        out_queue_.pop_front();
    }
    ws_->text(true);
    ws_->async_write(net::buffer(msg),
                     [self = shared_from_this()](beast::error_code ec, std::size_t) {
                         if (ec) { self->write_in_progress_ = false; return; }
                         self->do_write();
                     });
}

void BybitTradeWSClient::on_read(beast::error_code ec, std::size_t bytes) {
    if (ec) { reconnect(); return; }
    auto buffers = buffer_.data();
    if (buffers.size() > 0) process_message((const char*)buffers.data(), buffers.size());
    buffer_.consume(bytes);
    ws_->async_read(buffer_, beast::bind_front_handler(&BybitTradeWSClient::on_read, shared_from_this()));
}

void BybitTradeWSClient::process_message(const char* data, std::size_t size) {
    if (size + simdjson::SIMDJSON_PADDING > json_buffer_.capacity())
        json_buffer_.resize(size + simdjson::SIMDJSON_PADDING);
    std::memcpy(json_buffer_.data(), data, size);

    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    if (parser.parse(json_buffer_.data(), size, size).get(doc) != simdjson::SUCCESS) return;

    std::string_view op;
    if (doc["op"].get_string().get(op) == simdjson::SUCCESS) {
        if (op == "auth") {
            int64_t ret = -1;
            doc["retCode"].get_int64().get(ret);
            if (ret == 0) {
                authenticated_ = true;
                std::cout << "[TradeWS] Auth OK\n";
                send_ping();
            } else {
                std::cerr << "[TradeWS] Auth failed: " << ret << "\n";
            }
        }
        return;
    }

    // Ack for order.create / cancel / amend
    int64_t ret_code = -1;
    if (doc["retCode"].get_int64().get(ret_code) != simdjson::SUCCESS) return;

    Ack ack;
    std::string_view sv;
    if (doc["retMsg"].get_string().get(sv) == simdjson::SUCCESS) ack.ret_msg = sv;
    ack.ret_code = (int)ret_code;

    if (doc["reqId"].get_string().get(sv) == simdjson::SUCCESS) ack.req_id = sv;

    // data может быть object (orderId) или array (batch)
    simdjson::dom::element data_el;
    if (doc["data"].get(data_el) == simdjson::SUCCESS) {
        if (data_el.is_object()) {
            if (data_el["orderId"].get_string().get(sv) == simdjson::SUCCESS) ack.order_id = sv;
            if (data_el["orderLinkId"].get_string().get(sv) == simdjson::SUCCESS) ack.order_link_id = sv;
        } else if (data_el.is_array()) {
            simdjson::dom::array arr;
            if (data_el.get_array().get(arr) == simdjson::SUCCESS && arr.size() > 0) {
                auto first = arr.at(0);
                if (first["orderId"].get_string().get(sv) == simdjson::SUCCESS) ack.order_id = sv;
                if (first["orderLinkId"].get_string().get(sv) == simdjson::SUCCESS) ack.order_link_id = sv;
            }
        }
    }

    if (on_ack_) on_ack_(ack);
}

void BybitTradeWSClient::send_ping() {
    std::string ping = R"({"op":"ping"})";
    ws_->text(true);
    ws_->async_write(net::buffer(ping), [self = shared_from_this()](beast::error_code ec, std::size_t) {
        if (ec) return;
        self->ping_timer_.expires_after(std::chrono::seconds(20));
        self->ping_timer_.async_wait([self](beast::error_code ec) {
            if (!ec && self->running_) self->send_ping();
        });
    });
}

void BybitTradeWSClient::reconnect() {
    if (!running_) return;
    if (++reconnect_attempts_ > 10) { std::cerr << "[TradeWS] max reconnect\n"; return; }
    authenticated_ = false;
    if (ws_) { beast::error_code ec; if (ws_->is_open()) ws_->close(websocket::close_code::normal, ec); ws_.reset(); }
    buffer_.clear();
    reconnect_timer_.expires_after(std::chrono::seconds(5));
    reconnect_timer_.async_wait(beast::bind_front_handler(&BybitTradeWSClient::on_reconnect_timer, shared_from_this()));
}

void BybitTradeWSClient::on_reconnect_timer(beast::error_code ec) {
    if (ec) return;
    create_websocket();
    resolver_.async_resolve(host_, "443",
                            beast::bind_front_handler(&BybitTradeWSClient::on_resolve, shared_from_this()));
}

std::string BybitTradeWSClient::next_req_id() {
    static std::atomic<uint64_t> counter{0};
    return "mm_" + std::to_string(++counter) + "_" + std::to_string(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
}

void BybitTradeWSClient::on_write(beast::error_code ec, std::size_t /*bytes*/) {
    if (ec) {
        std::cerr << "[TradeWS] write error: " << ec.message() << "\n";
    }
}