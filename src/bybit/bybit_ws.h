#pragma once
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <set>
#include <unordered_map>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include "ticker_state.h"
#include "signal_logger.h"
#include "trade_tracker.h"
#include "mean_reversion_strategy.h"
#include "market_making_strategy.h"
#include "bybit_private_ws.h"
#include "iexchange.h"
#include "paper_exchange.h"
#include "bybit_trade_ws.h"
#include "bybit_private_ws.h"
#include "mm_symbol_registry.h"
#include "ch/ch_writer.h"
#include "true_price/true_price_engine.h"


namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = boost::asio::ip::tcp;
using arb::IExchange;
using arb::PaperExchange;
using arb::ExchangeExecution;
class BybitApiClient;

class BybitWSClient : public std::enable_shared_from_this<BybitWSClient> {
public:
    void set_ch_writer(std::shared_ptr<ch::Writer> w) { ch_writer_ = std::move(w); }
    void set_on_book_update(std::function<void(const std::string&)> cb) {
        on_book_update_cb_ = std::move(cb);
    }
    void set_mm_registry(std::shared_ptr<MmSymbolRegistry> reg) {
        mm_registry_ = std::move(reg);
    }
    int count_symbols_with_book() const;
    struct MeanReversionPosition {
        bool is_open = false;
        std::string side;
        double entry_price = 0.0;
        double qty = 0.0;
        uint64_t open_time = 0;
        std::string order_id;
        uint64_t order_sent_time = 0;
        double take_profit = 0.0;
        uint64_t last_trade_time = 0;
    };

    int max_mr_positions_ = 5;
    double risk_per_trade_pct_ = 0.005;
    double min_balance_for_trade_ = 100.0;

    std::unordered_map<std::string, MeanReversionPosition> mr_positions_;

    BybitWSClient(net::io_context& ioc, ssl::context& ctx,
                  const std::vector<std::string>& symbols,
                  OrderExecutor* executor,
                  BybitApiClient& api,
                  const std::string& ws_path,
                  bool auxiliary,
                  const std::string& pg_conn_str);
    ~BybitWSClient();

    void init_mm_strategies(IExchange& exchange);
    void on_paper_fill(const arb::ExchangeExecution& e);
    void set_paper_exchange(PaperExchange* p) { paper_exchange_ = p; }
    void stop_all_mm();
    auto get_current_price(const std::string& symbol) -> double;
    auto get_symbol_liquidity(const std::string& symbol) -> LiquidityInfo;
    void preload_history(const std::vector<std::string>& symbols, BybitApiClient& api);
    void enable_market_making(const std::string& symbol, double base_spread_pct, double order_qty, int positionIdx = 0);

    OrderBook& get_orderbook(const std::string& symbol);
    void close_all_mm_positions();
    net::steady_timer balance_timer_;
    void print_balance();
    std::shared_ptr<pqxx::connection> mm_db_conn_;
    std::map<std::string, uint64_t> processed_fills_;
    void run(const std::string& host, const std::string& port, const std::string& target);
    arb::PaperExchange* paper_exchange_;
    void init_trade_ws(const std::string& host);       // NEW
    void init_private_ws(const std::string& host);     // NEW
    void set_initial_balance(double b) { initial_balance_ = b; }
    void enable_symbol(const std::string& symbol);
    void disable_symbol(const std::string& symbol);
    std::vector<std::string> currently_trading_symbols() const;
    bool has_mm(const std::string& symbol) const;
    void set_signal_engine(std::shared_ptr<CrossVenueSignalEngine> engine) {
        signal_engine_ = std::move(engine);
    }
    // ── Bitunix symbol set (для cross-venue skew) ──
    void set_bitunix_symbols(std::set<std::string> syms) {
        bitunix_symbols_ = std::move(syms);
    }
    bool is_bitunix_symbol(const std::string& symbol) const {
        return bitunix_symbols_.count(symbol) > 0;
    }
    void start_mm_tick();
private:
    void scan_momentum_candidates();
    uint64_t last_momentum_scan_ms_ = 0;
    void start_regime_update();
    void do_regime_update();
    net::steady_timer regime_update_timer_;
    std::string pg_conn_str_;
    std::shared_ptr<ch::Writer> ch_writer_;
    std::set<std::string> bitunix_symbols_;   // пересечение Bybit × Bitunix
    std::shared_ptr<CrossVenueSignalEngine> signal_engine_;
    std::shared_ptr<BybitTradeWSClient>   trade_ws_;   // NEW
    std::shared_ptr<BybitPrivateWSClient> private_ws_;
    std::shared_ptr<MmSymbolRegistry> mm_registry_;
    void on_trade_ack(const BybitTradeWSClient::Ack& ack);
    std::atomic<bool> mm_initialized_{false};
    void on_private_execution(const std::string& exec_id,
                              const std::string& order_id,
                              const std::string& symbol,
                              const std::string& side,
                              double price, double qty, double fee,
                              const std::string& fee_currency,
                              uint64_t exec_time, bool is_maker);
    void on_private_order(const std::string& order_id,
                          const std::string& order_link_id,
                          const std::string& symbol,
                          const std::string& status,
                          const std::string& reject_reason);
    std::string ws_path_;       // ← NEW
    bool        auxiliary_ = false;
    std::function<void(const std::string&)> on_book_update_cb_;
    net::steady_timer reconnect_timer_;
    net::steady_timer ping_timer_;
    net::steady_timer subscribe_delay_timer_;
    net::steady_timer pos_manage_timer_;
    void start_pos_manage();
    net::steady_timer inv_refresh_timer_;
    void start_inv_refresh();
    BybitApiClient& api_;
    std::set<std::string> processed_execution_ids_;
    std::set<std::string> banned_symbols_;
    net::steady_timer mm_fill_poll_timer_;

    void start_mm_fill_polling();
    void poll_mm_fills();

    std::unordered_map<std::string, std::unique_ptr<OrderBook>> orderbooks_;
    std::unordered_map<std::string, std::unique_ptr<BarAggregator>> bar_aggregators_;
    std::unordered_map<std::string, std::unique_ptr<RegimeFilter>> regime_filters_;
    std::unordered_map<std::string, std::unique_ptr<TimeWindowVWMA>> vwmas_;
    std::unordered_map<std::string, std::unique_ptr<DynamicBBEngine>> bbs_;
    std::unordered_map<std::string, std::unique_ptr<ClusterEngine>> clusters_;
    std::unordered_map<std::string, std::shared_ptr<MarketMakingStrategy>> mm_strategies_;
    OrderExecutor* order_executor_ = nullptr;

    std::set<std::string> mm_active_symbols_;
    net::steady_timer execution_poll_timer_;
    net::steady_timer position_update_timer_;
    net::steady_timer stats_timer_;

    // Риск-менеджмент
    double initial_balance_ = 0.0;
    double max_daily_loss_pct_ = 0.03;
    double max_drawdown_pct_ = 0.05;
    bool kill_switch_active_ = false;
    bool can_open_new_position(double current_balance);

    std::chrono::steady_clock::time_point last_inv_refresh_{};
    void start_execution_polling();
    void poll_executions();
    void schedule_position_update();
    void schedule_stats_output();

    void on_resolve(beast::error_code ec, const tcp::resolver::results_type& results);
    void on_connect(beast::error_code ec, const tcp::resolver::results_type::endpoint_type& ep);
    void on_ssl_handshake(beast::error_code ec);
    void on_handshake(beast::error_code ec);
    void on_write(beast::error_code ec, std::size_t bytes_transferred);
    void on_read(beast::error_code ec, std::size_t bytes_transferred);
    void subscribe_topics();
    void send_subscribe_batch(size_t start_idx, size_t batch_size,
                              size_t batch_num, size_t total_batches);
    void send_ping();
    void process_message(const char* data, std::size_t size);
    auto extract_symbol(std::string_view topic) const -> std::string;
    void log_if_signal_generated(const std::string& symbol, TickerState& state);

    void reconnect();
    void on_reconnect_timer(beast::error_code ec);
    void create_websocket();

    net::io_context& ioc_;
    ssl::context& ctx_;
    tcp::resolver resolver_;

    std::unique_ptr<websocket::stream<beast::ssl_stream<beast::tcp_stream>>> ws_;

    beast::flat_buffer buffer_;

    std::string host_;
    std::vector<std::string> symbols_;
    std::map<std::string, std::unique_ptr<TickerState>> tickers_;

    std::unique_ptr<SignalLogger> signal_logger_;
    std::unique_ptr<TradeTracker> trade_tracker_;

    bool reconnecting_ = false;
    int reconnect_attempts_ = 0;
    static constexpr int MAX_RECONNECT_ATTEMPTS = 100;
    static constexpr int RECONNECT_DELAY_SEC = 5;

    std::vector<char> json_buffer_;
    void create_mm_for_symbol(const std::string& symbol);
    IExchange* exchange_ = nullptr;
    std::unique_ptr<tp::TruePriceEngine> tp_engine_;
};