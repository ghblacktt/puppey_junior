#pragma once
#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
namespace ssl   = net::ssl;
using tcp = net::ip::tcp;

class BitunixApiClient {
public:
    BitunixApiClient(const std::string& api_key,
                     const std::string& api_secret);
    ~BitunixApiClient();

    // Разместить лимитный ордер. effect: "GTC" / "POST_ONLY" / "IOC" / "FOK"
    std::string place_limit_order(const std::string& symbol,
                                  const std::string& side,
                                  double price, double qty,
                                  const std::string& effect = "POST_ONLY");

    // Разместить рыночный ордер
    std::string place_market_order(const std::string& symbol,
                                   const std::string& side,
                                   double qty);

    // Отменить ордер
    bool cancel_order(const std::string& symbol, const std::string& order_id);

    // Отменить все ордера по символу
    bool cancel_all_orders(const std::string& symbol);

    // Получить позицию (для сверки)
    struct PositionInfo {
        std::string symbol;
        std::string side;
        double size = 0.0;
        double avg_price = 0.0;
        double unrealized_pnl = 0.0;
        bool is_valid = false;
    };
    PositionInfo get_position_info(const std::string& symbol);

    // Диагностика
    std::string last_ret_msg() const { return last_ret_msg_; }
    int         last_ret_code() const { return last_ret_code_; }
    std::string get_api_key() const { return api_key_; }
    std::vector<std::string> get_trading_pairs();
    // рядом с get_trading_pairs():
    std::vector<std::string> get_available_symbols(
            const std::vector<std::string>& candidates);

private:
    std::string api_key_;
    std::string api_secret_;
    std::string host_ = "fapi.bitunix.com";
    std::string port_ = "443";

    net::io_context ioc_;
    ssl::context    ctx_;
    std::unique_ptr<beast::ssl_stream<beast::tcp_stream>> http_stream_;
    std::vector<tcp::endpoint> cached_endpoints_;
    std::mutex io_mutex_;

    std::string last_ret_msg_;
    int         last_ret_code_ = 0;

    bool ensure_connection();
    void close_connection();

    std::string send_request(const std::string& method,
                             const std::string& endpoint,
                             const std::string& query_params,
                             const std::string& body,
                             bool authenticated);

    static std::string sha256_hex(const std::string& input);
    static std::string generate_nonce();
    static std::string now_ms_str();
    static std::string now_sec_str();

    std::string generate_signature(const std::string& nonce,
                                   const std::string& timestamp,
                                   const std::string& query_params,
                                   const std::string& body);
};