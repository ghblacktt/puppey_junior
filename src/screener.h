#pragma once
#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <chrono>
#include <memory>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include "bybit/bybit_api_client.h"
#include "mm_symbol_registry.h"
#include "fill_model.h"
#include "time_decay_stats.h" // <-- Подключаем новый класс

struct SymbolScreenerStats {
    std::string symbol;
    double price = 0.0;

    // ═══ НОВОЕ: Event-based statistics с экспоненциальным затуханием ═══
    std::unique_ptr<mm::TimeDecayStats> spread_stats;
    std::unique_ptr<mm::TimeDecayStats> bbo_notional_stats;
    std::unique_ptr<mm::TimeDecayStats> adverse_stats;
    std::unique_ptr<mm::TimeDecayStats> through_stats;
    std::unique_ptr<mm::TimeDecayStats> update_rate_stats;
    std::unique_ptr<mm::TimeDecayStats> shadow_fills_stats;

    // Shadow order state
    bool     shadow_bid_pending = false;
    bool     shadow_ask_pending = false;
    uint64_t shadow_bid_fill_ms = 0;
    uint64_t shadow_ask_fill_ms = 0;
    double   shadow_bid_price = 0.0;
    double   shadow_ask_price = 0.0;
    uint64_t shadow_bid_placed_ms = 0;
    uint64_t shadow_ask_placed_ms = 0;
    std::string shadow_bid_oid_;   // С подчёркиванием
    std::string shadow_ask_oid_;   // С подчёркиванием

    // Rate tracking (для совместимости)
    uint64_t last_update_ms = 0;
    uint64_t update_count   = 0;
    uint64_t first_update_ms= 0;
    int      shadow_bid_fills = 0;
    int      shadow_ask_fills = 0;
    double   last_mid_for_through  = 0.0;

    // Snapshot metrics (для БД)
    double last_spread_median = 0.0;
    double last_spread_p25    = 0.0;
    double last_spread_p75    = 0.0;
    double last_sigma_bps     = 0.0;
    double last_bbo_notional  = 0.0;
    double last_update_rate   = 0.0;
    int    last_shadow_fills  = 0;
    double last_shadow_adv    = 0.0;
    double last_net_edge      = 0.0;
    uint64_t last_snapshot_ms = 0;
    double ema_edge = 0.0;
    uint64_t ema_last_update_ms = 0;

    // Hysteresis
    int consecutive_good_windows = 0;
    int consecutive_bad_windows = 0;

    // Constructor
    SymbolScreenerStats() {
        constexpr uint64_t TAU_MS = 900'000; // 15 минут
        spread_stats = std::make_unique<mm::TimeDecayStats>(TAU_MS);
        bbo_notional_stats = std::make_unique<mm::TimeDecayStats>(TAU_MS);
        adverse_stats = std::make_unique<mm::TimeDecayStats>(TAU_MS);
        through_stats = std::make_unique<mm::TimeDecayStats>(TAU_MS);
        update_rate_stats = std::make_unique<mm::TimeDecayStats>(TAU_MS);
        shadow_fills_stats = std::make_unique<mm::TimeDecayStats>(TAU_MS);
    }

    // Copy constructor (глубокое копирование unique_ptr)
    SymbolScreenerStats(const SymbolScreenerStats& other)
            : symbol(other.symbol), price(other.price),
              spread_stats(other.spread_stats ? std::make_unique<mm::TimeDecayStats>(*other.spread_stats) : nullptr),
              bbo_notional_stats(other.bbo_notional_stats ? std::make_unique<mm::TimeDecayStats>(*other.bbo_notional_stats) : nullptr),
              adverse_stats(other.adverse_stats ? std::make_unique<mm::TimeDecayStats>(*other.adverse_stats) : nullptr),
              through_stats(other.through_stats ? std::make_unique<mm::TimeDecayStats>(*other.through_stats) : nullptr),
              update_rate_stats(other.update_rate_stats ? std::make_unique<mm::TimeDecayStats>(*other.update_rate_stats) : nullptr),
              shadow_fills_stats(other.shadow_fills_stats ? std::make_unique<mm::TimeDecayStats>(*other.shadow_fills_stats) : nullptr),
              shadow_bid_pending(other.shadow_bid_pending), shadow_ask_pending(other.shadow_ask_pending),
              shadow_bid_fill_ms(other.shadow_bid_fill_ms), shadow_ask_fill_ms(other.shadow_ask_fill_ms),
              shadow_bid_price(other.shadow_bid_price), shadow_ask_price(other.shadow_ask_price),
              shadow_bid_placed_ms(other.shadow_bid_placed_ms), shadow_ask_placed_ms(other.shadow_ask_placed_ms),
              shadow_bid_oid_(other.shadow_bid_oid_), shadow_ask_oid_(other.shadow_ask_oid_),
              last_update_ms(other.last_update_ms), update_count(other.update_count), first_update_ms(other.first_update_ms),
              shadow_bid_fills(other.shadow_bid_fills), shadow_ask_fills(other.shadow_ask_fills),
              last_mid_for_through(other.last_mid_for_through),
              last_spread_median(other.last_spread_median), last_spread_p25(other.last_spread_p25),
              last_spread_p75(other.last_spread_p75), last_sigma_bps(other.last_sigma_bps),
              last_bbo_notional(other.last_bbo_notional), last_update_rate(other.last_update_rate),
              last_shadow_fills(other.last_shadow_fills), last_shadow_adv(other.last_shadow_adv),
              last_net_edge(other.last_net_edge), last_snapshot_ms(other.last_snapshot_ms),
              ema_edge(other.ema_edge), ema_last_update_ms(other.ema_last_update_ms),
              consecutive_good_windows(other.consecutive_good_windows), consecutive_bad_windows(other.consecutive_bad_windows) {}
};

struct ScreenerGlobalConfig {
    bool   enabled = true;
    int    scan_top_n = 200;
    double target_half_bps = 10.0;
    int    window_sec = 300;
    int    snapshot_interval_sec = 1; // <-- ИСПРАВЛЕНО: 15 секунд
    int    promote_interval_sec = 1;  // <-- ИСПРАВЛЕНО: 60 секунд
    int    min_shadow_fills = 10;      // <-- ИСПРАВЛЕНО: 10 филлов
    double min_net_edge_bps = 0.2;     // <-- ИСПРАВЛЕНО: 0.0
    double min_update_rate_hz = 0.5;
    double min_bbo_notional_usd = 500.0;
    double max_spread_p75_bps = 60.0;
    bool   auto_promote = true;
    bool   use_per_symbol_params  = true;
    int    default_max_hold_sec   = 120;
    double shadow_touch_volume_usd = 100.0;
};

class Screener {
public:
    Screener(boost::asio::io_context& ioc, const std::string& conn_str);
    void load(BybitApiClient& api);
    void on_book_update(const std::string& symbol, double best_bid, double best_ask, double bid_size, double ask_size);
    void on_trade(const std::string& symbol, double px, double qty, bool is_buyer_maker, uint64_t ts_ms);
    void start();
    size_t symbol_count() const { return stats_.size(); }
    ScreenerGlobalConfig config() const;
    void set_run_tag(const std::string& tag) { run_tag_ = tag; }
    void set_registry(std::shared_ptr<MmSymbolRegistry> reg) { registry_ = reg; }
    void debug_add_symbol(const std::string& sym);
    SymbolScreenerStats debug_stats(const std::string& sym) const;

private:
    std::shared_ptr<MmSymbolRegistry> registry_;
    void schedule_snapshot();
    void do_snapshot();
    void schedule_promote();
    void do_promote();
    void schedule_pg_revaluation();
    void do_pg_revaluation();

    static double median_of(std::deque<double> buf);
    void compute_snapshot_metrics(SymbolScreenerStats& s) const;
    void persist_snapshot(const std::vector<SymbolScreenerStats>& batch);

    boost::asio::io_context& ioc_;
    std::string conn_str_;
    boost::asio::steady_timer snapshot_timer_;
    boost::asio::steady_timer promote_timer_;
    boost::asio::steady_timer pg_reval_timer_;

    std::unordered_map<std::string, SymbolScreenerStats> stats_;
    mm::FillModel shadow_fm_;
    std::vector<std::string> symbols_;
    mutable std::mutex mtx_;
    ScreenerGlobalConfig cfg_;
    std::string run_tag_;
};