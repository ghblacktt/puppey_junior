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

// ── Per-symbol статистика (rolling window) ──
struct SymbolScreenerStats {
    std::string symbol;
    double price = 0.0;

    // Rolling buffers (последние N сэмплов)
    std::deque<double> spread_bps_buf;
    std::deque<double> mid_buf;
    std::deque<double> bbo_notional_buf;

    // Rate tracking
    uint64_t last_update_ms = 0;
    uint64_t update_count   = 0;
    uint64_t first_update_ms= 0;

    // Shadow fills: simulates what would happen if we placed MM at mid ± H
    // Состояние: bid_active / ask_active (не заполнены), либо "waiting after fill"
    bool     shadow_bid_pending = false;   // ждём fill по bid
    bool     shadow_ask_pending = false;
    uint64_t shadow_bid_fill_ms = 0;       // когда fill случился (0 = нет)
    uint64_t shadow_ask_fill_ms = 0;
    double   shadow_bid_price = 0.0;
    double   shadow_ask_price = 0.0;
    double   shadow_bid_adverse_bps = 0.0; // накапливаем adverse после fill
    double   shadow_ask_adverse_bps = 0.0;
    int      shadow_bid_fills = 0;
    int      shadow_ask_fills = 0;
    int      shadow_bid_adverse_n = 0;
    int      shadow_ask_adverse_n = 0;
    uint64_t shadow_bid_placed_ms = 0;
    uint64_t shadow_ask_placed_ms = 0;
    // OID зарегистрированных в shadow_fm_ ордеров (пусто = нет активного)
    std::string shadow_bid_oid_;
    std::string shadow_ask_oid_;

    // ═══ Rolling 10-min event windows (Sprint 4) ═══
    // Счётчики ниже остаются как lifetime-тоталы (для info / логов).
    // Решения promote/demote принимаются по окну, не по тоталам.
    std::deque<uint64_t>                    fills_events_ts_;     // ts fill
    std::deque<std::pair<uint64_t,double>>  adverse_events_;      // (ts, bps)
    std::deque<uint64_t>                    through_events_ts_;   // ts through
    std::deque<uint64_t>                    update_events_ts_;    // ts book-update

    // Последний snapshot (для БД)
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

    double   last_gross_edge       = 0.0;   // target_half + adv - 2
    double   last_toxic_penalty    = 0.0;   // through_pct * 15
    double   last_volume_bonus     = 0.0;   // log2(notional_1k) * 0.3
    double   last_through_pct      = 0.0;   // 0-100

    uint64_t through_count         = 0;
    double   last_mid_for_through  = 0.0;

    // ═══ NEW: per-symbol параметры ═══
    double   effective_min_hs_bps = 0.0;
    int      effective_max_hold_sec = 0;
    double   shadow_fill_volume_usd = 100.0;
    double shadow_bid_cum_volume_usd = 0.0;
    double shadow_ask_cum_volume_usd = 0.0;

    int consecutive_good_windows = 0;  // NEW: сколько раз подряд прошел порог
    int consecutive_bad_windows = 0;   // NEW: сколько раз подряд провалил порог
    double shadow_bid_fills_window = 0; // NEW: для расчета баланса
    double shadow_ask_fills_window = 0; //

    double ema_edge = 0.0;
    uint64_t ema_last_update_ms = 0;
};

struct ScreenerGlobalConfig {
    bool   enabled = true;
    int    scan_top_n = 200;
    double target_half_bps = 10.0;
    int    window_sec = 300;
    int    snapshot_interval_sec = 1;
    int    promote_interval_sec = 1;
    int    min_shadow_fills = 10;
    double min_net_edge_bps = -0.5;
    double min_update_rate_hz = 0.5;
    double min_bbo_notional_usd = 500.0;
    double max_spread_p75_bps = 60.0;
    bool   auto_promote = true;
    bool   use_per_symbol_params  = true;    // брать min_hs/max_hold из mm_symbols
    int    default_max_hold_sec   = 120;     // fallback если per-symbol не задан
    double shadow_touch_volume_usd = 100.0;  // объём для fill (как в paper)
};

class Screener {
public:
    Screener(boost::asio::io_context& ioc, const std::string& conn_str);

    // Загрузить конфиг + список символов из API
    void load(BybitApiClient& api);

    // Изменения с WS
    void on_book_update(const std::string& symbol,
                        double best_bid, double best_ask,
                        double bid_size, double ask_size);
    void on_mid_tick(const std::string& symbol, double mid);
    // Trades из ScannerClient — в единый FillModel
    void on_trade(const std::string& symbol,
                  double px, double qty,
                  bool is_buyer_maker,
                  uint64_t ts_ms);

    // ── Debug accessors (used by tests/test_sprint1.cpp) ──
    void debug_add_symbol(const std::string& sym);
    SymbolScreenerStats debug_stats(const std::string& sym) const;

    // Запуск периодических таймеров (snapshot + promote)
    void start();

    size_t symbol_count() const { return stats_.size(); }
    ScreenerGlobalConfig config() const;
    void set_run_tag(const std::string& tag) { run_tag_ = tag; }
    void set_registry(std::shared_ptr<MmSymbolRegistry> reg) { registry_ = reg; }
private:
    std::shared_ptr<MmSymbolRegistry> registry_;
    void schedule_snapshot();
    void do_snapshot();
    void schedule_promote();
    void do_promote();
    std::string run_tag_;
    static double median_of(std::deque<double> buf);
    static double pctl_of(std::deque<double> buf, double p);
    static double sigma_of(const std::deque<double>& mids);

    void compute_snapshot_metrics(SymbolScreenerStats& s) const;
    void persist_snapshot(const std::vector<SymbolScreenerStats>& batch);
    void persist_config();

    boost::asio::io_context& ioc_;
    std::string conn_str_;
    boost::asio::steady_timer snapshot_timer_;
    boost::asio::steady_timer promote_timer_;

    std::unordered_map<std::string, SymbolScreenerStats> stats_;
    mm::FillModel shadow_fm_;
    std::vector<std::string> symbols_;
    mutable std::mutex mtx_;
    ScreenerGlobalConfig cfg_;
    std::atomic<bool> running_{false};
    double shadow_bid_cum_volume_usd = 0.0;
    double shadow_ask_cum_volume_usd = 0.0;

    net::steady_timer pg_reval_timer_;
    void do_pg_revaluation();
    void schedule_pg_revaluation();
};