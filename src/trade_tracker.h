#pragma once
#include <pqxx/pqxx>
#include <functional>
#include <string>
#include <map>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>
#include <iostream>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <set>
#include "common_types.h"
#include "order_executor.h"

class TradeTracker {
public:
    using PriceGetter = std::function<double(const std::string&)>;
    using LiquidityGetter = std::function<LiquidityInfo(const std::string&)>;
    using VWMAGetter = std::function<double(const std::string&)>;

    TradeTracker(const std::string& connection_string,
                 PriceGetter price_getter,
                 LiquidityGetter liquidity_getter,
                 VWMAGetter vwma_getter,
                 OrderExecutor* executor);
    ~TradeTracker();

    void track_signal(
            int signal_id,
            const std::string& symbol,
            const std::string& signal_type,
            double entry_price,
            double stop_loss,
            double take_profit_1,
            double take_profit_2,
            uint64_t timestamp,
            double confidence,
            double volatility,
            double absorption_volume);

    void print_performance_summary();

    void start();
    void stop();

    bool has_open_position(const std::string& symbol) const;
    size_t get_open_positions_count();

private:
    PriceGetter get_current_price_;
    LiquidityGetter get_liquidity_;
    VWMAGetter get_vwma_;

    std::unique_ptr<pqxx::connection> conn_;
    std::mutex mutex_;

    std::unordered_map<int, ActiveTrade> active_trades_;
    std::set<std::string> active_symbols_;
    std::map<std::string, uint64_t> last_close_time_;
    std::map<std::string, std::string> last_close_reason_;
    void collect_markout(const ActiveTrade& trade, double exit_price, uint64_t exit_time);
    static constexpr int MAX_OPEN_POSITIONS = 5;
    static constexpr uint64_t COOLDOWN_AFTER_TP_MS = 60 * 1000;
    static constexpr uint64_t COOLDOWN_AFTER_SL_MS = 5 * 60 * 1000;
    static constexpr uint64_t COOLDOWN_AFTER_TIMEOUT_MS = 5 * 60 * 1000;

    double initial_balance_ = 0.0;
    double max_daily_loss_pct_ = 0.03;
    double max_drawdown_pct_ = 0.05;
    bool kill_switch_active_ = false;

    bool check_risk_limits(double current_balance);
    void close_all_positions();
    void check_active_trades();   // <-- добавлено

    void log_signal_not_executed(int signal_id, const std::string& symbol,
                                 const std::string& signal_type, double confidence,
                                 double entry_price, double stop_loss,
                                 double take_profit, const std::string& reason);
    void log_signal_executed(int signal_id, int trade_id, const std::string& symbol);
    uint64_t get_cooldown_duration(const std::string& reason) const;
    bool is_in_cooldown(const std::string& symbol) const;
    uint64_t get_cooldown_remaining(const std::string& symbol) const;
    void close_trade(ActiveTrade& trade, double exit_price,
                     const std::string& reason, uint64_t timestamp);
    double calculate_position_size(const std::string& symbol, double entry_price);

    std::atomic<bool> running_{false};
    std::thread worker_thread_;
    OrderExecutor* order_executor_;
    int next_tracking_id_ = 1;
};