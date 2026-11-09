#pragma once
#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio.hpp>
#include <simdjson.h>

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
namespace ssl   = net::ssl;
using tcp = net::ip::tcp;

// ============================================================
// Free structs (used by WS, TradeTracker, etc.)
// ============================================================
struct BybitOrder {
    std::string order_id;
    std::string symbol;
    std::string side;
    std::string order_type;
    double      price       = 0.0;
    double      qty         = 0.0;
    std::string order_status;
    uint64_t    created_time = 0;
    uint64_t    updated_time = 0;
};

struct InstrumentInfo {
    std::string symbol;
    double tick_size      = 0.0001;
    double step_size      = 0.1;
    double min_order_qty  = 0.0;
    double min_order_amt  = 0.0;
};

struct ExecutionInfo {
    std::string order_id;
    std::string symbol;
    std::string side;
    double      exec_price   = 0.0;
    double      exec_qty     = 0.0;
    double      exec_fee     = 0.0;
    std::string fee_currency;
    uint64_t    exec_time    = 0;
    bool        is_valid     = false;
    std::string exec_id;
    std::string exec_type;
};

// ============================================================
// BybitApiClient
// ============================================================
class BybitApiClient {
public:
    std::string get_api_key()    const { return api_key_; }
    std::string get_api_secret() const { return api_secret_; }   // NEW
    bool        is_testnet()     const {
        return host_.find("testnet") != std::string::npos
               || host_.find("demo")    != std::string::npos;
    }
    std::string last_ret_msg;
    int         last_ret_code = 0;
    static constexpr double POS_INFO_RATE_PER_SEC = 2.0;
    static constexpr double POS_INFO_RATE_BURST   = 3.0;
    // ============================================================
    // Nested structs — ВСЕ объявлены ДО методов, которые их возвращают
    // ============================================================
    struct Kline {
        uint64_t open_time = 0;
        double   open      = 0.0;
        double   high      = 0.0;
        double   low       = 0.0;
        double   close     = 0.0;
        double   volume    = 0.0;
    };

    struct Trade {
        uint64_t timestamp      = 0;
        double   price          = 0.0;
        double   volume         = 0.0;
        bool     is_buyer_maker = false;
    };

    struct OrderBookSnapshot {
        double timestamp = 0.0;
        std::vector<std::pair<double, double>> bids;   // price, qty
        std::vector<std::pair<double, double>> asks;
    };

    struct PositionInfo {
        std::string symbol;
        std::string side;
        double      size           = 0.0;
        double      avg_price      = 0.0;
        double      unrealized_pnl = 0.0;
        bool        is_valid       = false;
        bool        rate_limited = false;
    };

    struct Execution {
        std::string symbol;
        std::string side;
        std::string order_id;
        double      exec_price   = 0.0;
        double      exec_qty     = 0.0;
        double      exec_fee     = 0.0;
        std::string fee_currency;
        uint64_t    exec_time    = 0;
        std::string exec_id;
        std::string exec_type;
        bool        is_valid     = false;
    };

    // Для funding-arb: snapshot funding/turnover/last
    struct TickerData {
        double funding_rate = 0.0;    // доля за 8h (0.0001 = 0.01%)
        double turnover_24h = 0.0;    // USDT
        double last_price   = 0.0;
    };

    // ============================================================
    // Constructors
    // ============================================================
    BybitApiClient(const std::string& api_key,
                   const std::string& api_secret,
                   bool use_testnet = true);
    ~BybitApiClient();

    // ============================================================
    // Misc / connection
    // ============================================================
    bool        is_connected() const { return is_connected_; }
    std::string hmac_sha256(const std::string& sign_str) const;
    std::string sign_auth(const std::string& expires) const {
        return hmac_sha256("GET/realtime" + expires);
    }
    static double safe_stod(std::string_view sv);

    // ============================================================
    // Market data
    // ============================================================
    double                        get_last_price(const std::string& symbol);
    std::vector<Kline>            get_kline(const std::string& symbol,
                                            const std::string& interval,
                                            int limit = 200);
    std::vector<Trade>            get_public_trades(const std::string& symbol, int limit = 500);

    // Linear orderbook (по умолчанию)
    OrderBookSnapshot             get_orderbook_snapshot(const std::string& symbol, int depth = 50);
    // Spot orderbook (для funding-arb)
    OrderBookSnapshot             get_orderbook_snapshot_spot(const std::string& symbol, int depth = 1);

    // Bulk linear tickers (funding + turnover + last)
    std::unordered_map<std::string, TickerData> get_all_tickers_linear();

    // Funding rate одной пары
    double                        get_funding_rate(const std::string& symbol);

    // ============================================================
    // Position / balance
    // ============================================================
    PositionInfo get_position_info(const std::string& symbol, int positionIdx = 0);
    double       get_balance(const std::string& coin = "USDT");

    // ============================================================
    // Instruments / normalization
    // ============================================================
    InstrumentInfo get_instrument_info(const std::string& symbol,
                                       const std::string& category = "linear");

    // Linear normalize/format
    double      normalize_price(const std::string& symbol, double price);
    double      normalize_qty(const std::string& symbol, double qty);
    std::string format_price(const std::string& symbol, double price);
    std::string format_qty(const std::string& symbol, double qty);

    // Spot normalize/format
    double      normalize_price_spot(const std::string& symbol, double price);
    double      normalize_qty_spot(const std::string& symbol, double qty);
    std::string format_price_spot(const std::string& symbol, double price);
    std::string format_qty_spot(const std::string& symbol, double qty);

    // ============================================================
    // Order placement — Linear
    // ============================================================
    std::string place_limit_order(const std::string& symbol,
                                  const std::string& side,
                                  double price, double qty,
                                  double take_profit = 0.0,
                                  double stop_loss   = 0.0,
                                  int    positionIdx = 0);

    std::string place_market_order(const std::string& symbol,
                                   const std::string& side,
                                   double qty);              // reduceOnly=true

    std::string place_market_order_open(const std::string& symbol,
                                        const std::string& side,
                                        double qty);         // без reduceOnly — открытие

    std::string place_ioc_limit_order(const std::string& symbol,
                                      const std::string& side,
                                      double price, double qty);

    std::string place_close_order(const std::string& symbol,
                                  const std::string& side,
                                  double price, double qty);

    // ============================================================
    // Order placement — Spot (для funding-arb)
    // ============================================================
    std::string place_ioc_limit_order_spot(const std::string& symbol,
                                           const std::string& side,
                                           double price, double qty);

    std::string place_market_order_spot(const std::string& symbol,
                                        const std::string& side,
                                        double qty);

    // ============================================================
    // Order cancel / query
    // ============================================================
    bool                     cancel_order(const std::string& symbol,
                                          const std::string& order_id);
    bool                     cancel_all_orders(const std::string& symbol);
    BybitOrder               get_order(const std::string& symbol,
                                       const std::string& order_id);
    std::vector<BybitOrder>  get_open_orders(const std::string& symbol);

    // ============================================================
    // Executions
    // ============================================================
    ExecutionInfo               get_last_execution(const std::string& symbol,
                                                   const std::string& order_id);
    std::vector<Execution>      get_executions(const std::string& symbol,
                                               uint64_t start_time_ms = 0,
                                               int limit = 10);

    // ============================================================
    // Symbols
    // ============================================================
    std::vector<std::string> get_all_linear_symbols();
    std::vector<std::string> get_all_spot_symbols();
    std::vector<std::string> get_top_liquid_symbols(int top_n = 50);       // linear
    std::vector<std::string> get_top_liquid_spot_symbols(int top_n = 30);  // spot

    // ============================================================
    // Misc
    // ============================================================
    bool request_demo_funds(const std::string& coin);
    bool set_trading_stop(const std::string& symbol,
                          const std::string& side,
                          double take_profit, double stop_loss,
                          double position_size = 0.0);

private:
    // ---- Fields ----
    std::string api_key_;
    std::string api_secret_;
    std::string host_;
    std::string port_;

    net::io_context ioc_;
    ssl::context    ctx_;
    mutable std::mutex rate_mtx_;
    double rate_tokens_ = 1.0;
    std::chrono::steady_clock::time_point rate_last_refill_ =
            std::chrono::steady_clock::now();
    uint64_t last_rate_warn_ms_ = 0;

    bool try_acquire_pos_info_token();
    std::unique_ptr<beast::ssl_stream<beast::tcp_stream>> http_stream_;
    std::vector<tcp::endpoint> cached_endpoints_;
    std::mutex io_mutex_;

    bool is_connected_ = false;

    std::map<std::string, InstrumentInfo> instruments_cache_;   // key = "category:symbol"
    std::mutex instruments_mutex_;

    static std::mutex request_mutex_;

    // ---- Connection ----
    bool ensure_connection();
    void close_connection();

    // ---- HTTP ----
    std::string send_request(const std::string& method,
                             const std::string& endpoint,
                             const std::map<std::string, std::string>& params = {},
                             bool authenticated = true);

    std::string get_timestamp();
    std::string generate_signature_post(const std::string& timestamp,
                                        const std::string& body);
    std::string generate_signature_get(const std::string& timestamp,
                                       const std::string& query);

    // ---- Parsing ----
    void parse_order(const simdjson::dom::element& order_elem, BybitOrder& order);
};