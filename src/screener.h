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
};

struct ScreenerGlobalConfig {
    bool   enabled = true;
    int    scan_top_n = 200;
    double target_half_bps = 10.0;
    int    window_sec = 300;
    int    snapshot_interval_sec = 30;
    int    promote_interval_sec = 300;
    int    min_shadow_fills = 15;
    double min_net_edge_bps = 2.0;
    double min_update_rate_hz = 0.5;
    double min_bbo_notional_usd = 500.0;
    double max_spread_p75_bps = 60.0;
    bool   auto_promote = true;
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

    // Запуск периодических таймеров (snapshot + promote)
    void start();

    size_t symbol_count() const { return stats_.size(); }
    ScreenerGlobalConfig config() const;
    void set_run_tag(const std::string& tag) { run_tag_ = tag; }
private:
    void schedule_snapshot();
    void do_snapshot();
    void schedule_promote();
    void do_promote();
    std::string run_tag_;
    static double median_of(std::deque<double> buf);
    static double pctl_of(std::deque<double> buf, double p);
    static double sigma_of(const std::deque<double>& mids);

    void compute_snapshot_metrics(SymbolScreenerStats& s);
    void persist_snapshot(const std::vector<SymbolScreenerStats>& batch);
    void persist_config();

    boost::asio::io_context& ioc_;
    std::string conn_str_;
    boost::asio::steady_timer snapshot_timer_;
    boost::asio::steady_timer promote_timer_;

    std::unordered_map<std::string, SymbolScreenerStats> stats_;
    std::vector<std::string> symbols_;
    mutable std::mutex mtx_;
    ScreenerGlobalConfig cfg_;
    std::atomic<bool> running_{false};
};