#pragma once
#include <string>
#include <memory>
#include <chrono>
#include <deque>
#include <utility>
#include <map>
#include <algorithm>
#include <cmath>
#include <pqxx/pqxx>
#include "order_book.h"
#include "cluster_engine.h"
#include "bybit/bybit_api_client.h"
#include "fill_outcome_tracker.h"
#include "adaptive_controller.h"
#include "regime_filter.h"
#include "iexchange.h"
#include "cross_venue_signal.h"
#include <array>
#include <unordered_map>
#include "features/hma_filter.h"
#include "features/kernel_filter.h"
#include "symbol_regime.h"

using arb::IExchange;
class CrossVenueSignalEngine;
class MarketMakingStrategy : public std::enable_shared_from_this<MarketMakingStrategy> {
public:
    void set_regime_snapshot(const mm::SymModeSnapshot& s) { regime_ = s; }
    const mm::SymModeSnapshot& regime() const { return regime_; }
    void set_executor(net::any_io_executor exec) {
        exec_ = std::move(exec);
        has_exec_ = true;
    }

    void cancel_ask_on_through(double trade_price, const std::string& reason);
    void cancel_bid_on_through(double trade_price, const std::string& reason);
    void cancel_ask_now(const std::string& reason);
    void cancel_bid_now(const std::string& reason);

    // ── Symbol regime (Sprint 3) ──
    struct FinalizedMarkout {
        std::string order_id;
        std::string side;
        double   qty             = 0.0;
        double   fill_price      = 0.0;
        uint64_t fill_time_ms    = 0;
        double   fair_price_at_fill = 0.0;
        double   mid_100ms       = 0.0;
        double   mid_500ms       = 0.0;
        double   mid_1s          = 0.0;
        double   mid_5s          = 0.0;
        double   mid_30s         = 0.0;
        double   mk_100ms_bps    = 0.0;
        double   mk_500ms_bps    = 0.0;
        double   mk_1s_bps       = 0.0;
        double   mk_5s_bps       = 0.0;
        double   mk_30s_bps      = 0.0;
        bool     finalized       = true;   // для совместимости с backtest main
    };
    // ============================================================
    // Rolling σ estimator
    // ============================================================
    class VolEstimator {
    public:
        static constexpr size_t MAX_SAMPLES = 300;
        static constexpr size_t MIN_SAMPLES = 20;

        void update(uint64_t ts_ms, double price) {
            if (!prices_.empty() && prices_.back().second > 0 && price > 0) {
                double ret = std::log(price / prices_.back().second);
                if (std::isfinite(ret)) returns_.push_back(ret);
            }
            prices_.push_back({ts_ms, price});
            while (prices_.size()  > MAX_SAMPLES) prices_.pop_front();
            while (returns_.size() > MAX_SAMPLES) returns_.pop_front();
        }

        bool get_sigma_per_sqrt_sec(double& out) const {
            if (returns_.size() < MIN_SAMPLES || prices_.size() < 2) return false;
            double mean = 0.0;
            for (double r : returns_) mean += r;
            mean /= static_cast<double>(returns_.size());
            double var = 0.0;
            for (double r : returns_) { double d = r - mean; var += d * d; }
            var /= static_cast<double>(returns_.size() - 1);
            uint64_t dt_ms = prices_.back().first - prices_.front().first;
            if (dt_ms == 0) return false;
            double avg_dt_sec = (dt_ms / 1000.0) / static_cast<double>(returns_.size());
            if (avg_dt_sec <= 0) return false;
            out = std::sqrt(var / avg_dt_sec);
            return std::isfinite(out) && out > 0;
        }

        void reset() { prices_.clear(); returns_.clear(); }

    private:
        std::deque<std::pair<uint64_t, double>> prices_;
        std::deque<double> returns_;
    };
    void set_signal_engine(CrossVenueSignalEngine* engine) { signal_engine_ = engine; }
    void set_is_bitunix_symbol(bool v)                   { is_bitunix_symbol_ = v; }
    void set_adaptive_config(AdaptiveController::Config cfg) {
        controller_ = AdaptiveController(cfg);
    }

    struct PlaceContext {
        double mid;
        double best_bid;
        double best_ask;
        double half_spread;
        double skew;
        double sigma;
        double q_ratio;
        double inv_usd;
        double cross_bps     = 0.0;
        double ret_500       = 0.0;
        double flow_b        = 0.0;
        double flow_bnx      = 0.0;
        double ofi           = 0.0;
        double delta         = 0.0;
        double hma_fast_val = 0.0;
        double hma_slow_val = 0.0;
        double kernel_val = 0.0;
        double hma_skew_bps = 0.0;
        double hma_fast     = 0.0;
        double hma_slow     = 0.0;
        double kernel       = 0.0;
    };

    struct PlacedOrderInfo {
        double      price;
        std::string side;
        uint64_t    placed_ms;
    };

    // Backtest tuning
    void set_sigma_ref(double s) { sigma_ref_ = s; }
    void set_trend_thr_bps(double bps) { trend_thr_override_bps_ = bps; }
    void set_vol_ratio_max(double r) { vol_ratio_max_ = r; }

    MarketMakingStrategy(OrderBook& orderbook,
                         ClusterEngine& cluster,
                         IExchange& exchange,
                         std::string  symbol,
                         double base_spread_pct   = 0.002,
                         double order_qty_usd     = 15.0,
                         double max_position_usd  = 60.0);

    void tick();
    void set_db_connection(std::shared_ptr<pqxx::connection> conn) { db_conn_ = conn; }
    double get_inventory_usd() const { return inventory_usd_; }

    // Callback из poll_mm_fills. Если параметры переданы — используется точная цена/time
    void on_order_filled(const std::string& order_id,
                         uint64_t fill_ts_ms,
                         double fill_price,
                         double fill_qty,
                         const std::string& side,
                         bool is_full_fill = true);   // ← NEW

    // ============================================================
    // Setters
    // ============================================================
    void set_gamma(double g)                       { gamma_ = g; base_gamma_ = g; }
    void set_sigma_bounds(double lo, double hi)    { min_sigma_ = lo; max_sigma_ = hi; }
    void set_half_spread_bounds(double lo_pct, double hi_pct) {
        min_half_spread_pct_ = lo_pct;
        max_half_spread_pct_ = hi_pct;
    }
    void set_current_time_ms(uint64_t ts) { backtest_now_ms_ = ts; }
    void set_base_spread_pct(double pct)           { base_spread_pct_ = pct; }
    void set_hard_cap_ratio(double r)              { hard_cap_ratio_ = r; }
    void set_block_ratio(double r)                 { block_ratio_ = r; }
    void set_soft_exit_ratio(double r)             { soft_exit_ratio_ = r; }
    void set_soft_exit_pnl_pct(double p)           { soft_exit_pnl_pct_ = p; }
    void set_use_adaptive(bool v)                  { use_adaptive_ = v; }

    // Diagnostics
    double get_last_sigma() const       { return last_sigma_; }
    double get_last_half_spread() const { return last_half_spread_; }
    double get_last_skew() const        { return last_skew_; }
    const AdaptiveController::Params& get_adaptive_params() const { return adaptive_params_; }
    void set_run_tag(const std::string& tag) { run_tag_ = tag; }
    void set_momentum_weight(double w) { momentum_weight_ = std::max(0.0, w); }
    void reset_vol_estimator() {
        vol_estimator_.reset();
        last_sigma_ = sigma_ref_;
        mid_history_.clear();
        last_trend_log_ms_ = 0;
    }
    bool refresh_position();
    void on_trade_through_ask(double price, double size);
    void on_trade_through_bid(double price, double size);

    // Инжекция RegimeFilter (опционально, nullptr = не использовать)
    void set_regime_filter(RegimeFilter* rf) { regime_filter_ = rf; }

// Пороги для VPIN и multi-level spread
    void set_vpin_thresholds(double vpin_pause, double vpin_resume) {
        vpin_pause_ = vpin_pause;
        vpin_resume_ = vpin_resume;
    }

    double maker_fee_bps() const { return maker_fee_bps_; }
    void set_maker_fee_bps(double v) { maker_fee_bps_ = v; }

    // Graceful shutdown: отменить ордера, закрыть позиции (для hot-disable)
    void shutdown_gracefully(const std::string& reason = "GRACEFUL_SHUTDOWN");

    // Публичный доступ к inventory для disable-логики
    double inventory_usd() const { return inventory_usd_; }
    void set_max_hold_sec(int sec) { max_hold_override_sec_ = std::max(0, sec); }

    std::vector<FinalizedMarkout> drain_finalized_markouts();

private:
    HmaFilter    hma_fast_{8,  1000};   // 8 секунд, быстрый тренд
    HmaFilter    hma_slow_{32, 1000};   // 32 секунды, медленный тренд
    KernelFilter kernel_{120'000, 30'000};   // 2 мин окно, 30s bandwidth
    double compute_hma_skew_bps() const;
    double hma_skew_bps_ = 0.0;
    uint64_t pending_close_first_attempt_ms_ = 0;
    uint64_t now() const {
        if (backtest_now_ms_ > 0) return backtest_now_ms_;
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
    }
    CrossVenueSignalEngine* signal_engine_ = nullptr; // указатель на движок сигналов
    bool is_bitunix_symbol_ = false; // true, если символ торгуется на Bitunix
    double cross_skew_bps_ = 0.0;
    uint64_t backtest_now_ms_ = 0;
    double trend_thr_override_bps_ = 0.0;   // 0 = дефолт
    double vol_ratio_max_ = 2.0;            // default (live)
    int max_hold_override_sec_ = 0;
    uint64_t last_trend_cut_ms_ = 0;
    double maker_fee_bps_ = 2.0;
    RegimeFilter* regime_filter_ = nullptr;
    double vpin_pause_  = 0.75;    // >= VPIN: снимаем котировки
    double vpin_resume_ = 0.55;    // <= VPIN: возобновляем (гистерезис)
    uint64_t vpin_pause_until_   = 0;
    double   base_gamma_         = 4.0;   // исходное значение γ
    double   avg_sigma_1m_       = 0.0;   // EMA σ за минуту, для vol_ratio
    uint64_t last_sigma_ema_ms_  = 0;
    OrderBook&      orderbook_;
    ClusterEngine&  cluster_;
    IExchange& api_;
    std::string     symbol_;
    int consecutive_invalid_pos_ = 0;
    double base_spread_pct_;
    double order_qty_usd_;
    double max_position_usd_;
    std::atomic<bool> pending_cancel_ask_{false};
    std::atomic<bool> pending_cancel_bid_{false};
    // Инвентарь
    double      inventory_usd_ = 0.0;
    double      inventory_qty_ = 0.0;
    std::string inventory_side_;
    uint64_t    position_opened_time_ = 0;

    std::string bid_order_id_;
    std::string ask_order_id_;

    std::shared_ptr<pqxx::connection> db_conn_;

    // AS/GLFT параметры
    double gamma_                = 4.0;
    double sigma_ref_            = 0.0002;
    double min_sigma_            = 0.00005;
    double max_sigma_            = 0.005;
    double min_half_spread_pct_  = 0.0003;
    double max_half_spread_pct_  = 0.02;
    double hard_cap_ratio_       = 0.99;
    double block_ratio_          = 0.10;

    double soft_exit_ratio_      = 0.30;
    double soft_exit_pnl_pct_    = 0.015;

    // Volatility state
    VolEstimator vol_estimator_;
    double last_sigma_ = 0.0002;
    uint64_t ask_cooldown_until_ = 0;
    uint64_t bid_cooldown_until_ = 0;
    // Tick-level state
    uint64_t last_pos_refresh_ms_   = 0;
    uint64_t last_bid_p_time_ms_    = 0;
    uint64_t last_ask_p_time_ms_    = 0;
    double   last_bid_p_            = 0.0;
    double   last_ask_p_            = 0.0;

    double   last_half_spread_      = 0.0;
    double   last_skew_             = 0.0;
    double   last_unrealized_pnl_pct_ = 0.0;

    // Close escalation
    int      close_fail_streak_     = 0;
    uint64_t close_blocked_until_   = 0;
    uint64_t last_close_attempt_ms_ = 0;
    std::string pending_close_oid_;
    uint64_t    pending_close_ms_ = 0;
    // Trend filter
    std::deque<std::pair<uint64_t, double>> mid_history_;
    uint64_t last_trend_log_ms_ = 0;
    std::string run_tag_;
    // Online adaptation
    FillOutcomeTracker        fill_tracker_;
    AdaptiveController        controller_;
    AdaptiveController::Params adaptive_params_;
    std::map<std::string, PlacedOrderInfo> placed_orders_;
    uint64_t last_adapt_ms_     = 0;
    uint64_t last_log_adapt_ms_ = 0;
    bool     use_adaptive_      = true;
    uint64_t last_adverse_close_ms_ = 0;
    double      momentum_weight_ = 1.0;
    // Внутренние методы
    void close_position_market(double best_bid, double best_ask,
                               const std::string& reason = "HARD_CAP");
    void run_adaptive_update(uint64_t now_ms);
    void prune_placed_orders(uint64_t now_ms);
    uint64_t last_bid_place_ms_ = 0;
    uint64_t last_ask_place_ms_ = 0;
    void log_order(const std::string& order_id, const std::string& side,
                   double price, double qty, const PlaceContext& ctx);
    void log_cancel(const std::string& order_id, const std::string& reason);
    void log_reject(const std::string& side, double price, double qty,
                    const PlaceContext& ctx, int ret_code, const std::string& ret_msg);

    // ── Pending markouts (rolling) ──
    struct PendingMarkout {
        std::string order_id;
        std::string side;
        double      qty = 0.0;
        double      fill_price = 0.0;
        double      fair_price_at_fill = 0.0;
        uint64_t    fill_time_ms = 0;
        std::array<double, 7> mids{0.0, 0.0, 0.0, 0.0, 0.0};
        std::array<bool,   7> recorded{false,false,false,false,false};
        bool finalized = false;
    };
    std::unordered_map<std::string, PendingMarkout> pending_markouts_;
    std::vector<FinalizedMarkout> finalized_markouts_;
// ── P&L attribution (per-symbol accumulators) ──
    double spread_income_total_ = 0.0;
    double fees_total_          = 0.0;
    net::any_io_executor exec_;
    bool                 has_exec_ = false;
    mm::SymModeSnapshot  regime_;
// ── Helpers ──
    void update_pending_markouts(uint64_t now);
    void persist_markout(const PendingMarkout& m);
    bool shutting_down_ = false;
};