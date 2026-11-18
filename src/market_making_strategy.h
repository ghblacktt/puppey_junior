#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pqxx { class connection; }
namespace tp   { class TruePriceEngine; }
#include "order_book.h"

#include "cluster_engine.h"
#include "iexchange.h"
#include "fill_outcome_tracker.h"
#include "adaptive_controller.h"
#include "features/hma_filter.h"
#include "features/kernel_filter.h"
#include "symbol_regime.h"
namespace arb { class IExchange; }

class CrossVenueSignalEngine;
class RegimeFilter;

namespace mm_cfg {

// ─── Тайминги (ms) ──────────────────────────────────────────────
    constexpr uint64_t kBlockLogIntervalMs       = 30'000;
    constexpr uint64_t kTrendCutCooldownMs       = 60'000;
    constexpr uint64_t kToxicCloseBlockMs        = 300'000;
    constexpr uint64_t kBlacklistBlockMs         = 3'600'000;
    constexpr uint64_t kPositionRefreshMs        = 20'000;
    constexpr uint64_t kRefreshAfterFillCooldownMs = 3'000;
    constexpr uint64_t kBookStaleMs              = 10'000;
    constexpr uint64_t kPendingCloseTimeoutMs    = 90'000;
    constexpr uint64_t kPendingCloseRetryBlockMs = 60'000;
    constexpr uint64_t kMomentumMinAgeMs         = 500;
    constexpr uint64_t kMomentumNoFillResetMs    = 3'000;
    constexpr uint64_t kMomentumCloseTimeoutMs   = 4'000;
    constexpr uint64_t kMomentumTier2AgeMs       = 5'000;
    constexpr uint64_t kMomentumTier5AgeMs       = 10'000;
    constexpr uint64_t kMomentumTier15AgeMs      = 20'000;
    constexpr uint64_t kMinReplaceIntervalMs     = 5'000;
    constexpr uint64_t kVpinPauseDurationMs      = 15'000;
    constexpr uint64_t kBaseMaxHoldMs            = 300'000;
    constexpr uint64_t kToxicMaxHoldMs           = 5'000;
    constexpr uint64_t kMidHistoryWindowMs       = 60'000;
    constexpr uint64_t kSigmaEmaIntervalMs       = 1'000;
    constexpr uint64_t kMomentumDriftWindowMs    = 200;
    constexpr uint64_t kDriftWindowMs            = 60'000;
    constexpr uint64_t kStaleQuoteCooldownMs     = 1'000;
    constexpr uint64_t kObiEmaIntervalMs         = 500;
    constexpr uint64_t kFastAdverseMinAgeMs      = 5'000;
    constexpr uint64_t kFastAdverseMaxAgeMs      = 30'000;
    constexpr uint64_t kUrgencyAgeCapMs          = 30'000;
    constexpr uint64_t kDriftLogIntervalMs       = 30'000;

// ─── Пороги / эпсилоны ──────────────────────────────────────────
    constexpr double   kPosEpsilon              = 1e-9;
    constexpr double   kToxicThresholdBps       = -5.0;
    constexpr double   kDustNotionalUsd         = 5.5;
    constexpr double   kMinNotionalUsd          = 5.5;
    constexpr double   kMaxHoldTriggerUsd       = 5.0;
    constexpr double   kUrgencyMinUsd           = 1.0;
    constexpr double   kUrgencyMaxUsd           = 20.0;
    constexpr double   kSoftExitQtyRatio        = 0.15;
    constexpr double   kSoftExitPnlPct          = -0.005;
    constexpr double   kFastAdversePnlBps       = -15.0;

// ─── Спреад / bps ───────────────────────────────────────────────
    constexpr double   kMaxCrossBps             = 8.0;
    constexpr double   kMinBpsFromTop           = 0.5;
    constexpr int      kMinTicksFromTop         = 1;
    constexpr double   kMomentumSpreadGateBps   = 35.0;
    constexpr double   kMomentumHawkesGate      = 5.0;
    constexpr double   kMomentumMinSpreadTicks  = 2.0;
    constexpr double   kMinTrendThresholdPct    = 0.0020;
    constexpr double kAbsoluteMaxBps = 500.0;
// ── Классификация реального спреда (пороги, bps) ──
// ОДИН источник истины для порогов
    constexpr double kSpreadClassUltraWideBps = 50.0;
    constexpr double kSpreadClassWideBps      = 30.0;
    constexpr double kSpreadClassMidBps       = 15.0;
    constexpr double kSpreadClassTightBps     = 8.0;
    constexpr double kSpreadClassVeryTightBps = 3.0;

// ── min_spread для каждого класса ──
    constexpr double kMinSpreadUltraWideBps = 40.0;   // > 50 bps
    constexpr double kMinSpreadWideBps      = 30.0;   // 30-50
    constexpr double kMinSpreadMidBps       = 20.0;   // 15-30
    constexpr double kMinSpreadTightBps     = 12.0;   // 8-15
    constexpr double kMinSpreadVeryTightBps =  6.0;   // 3-8
    constexpr double kMinSpreadUltraTightBps = 2.0;   // < 3
    constexpr double kSpreadClassUltraUltraTightBps = 1.0;   // < 1 bps
    constexpr double kMinSpreadUltraUltraTightBps   = 1.0;   // котируем в 0.5 bps

// ── Fee floor ──
    constexpr double kMinSpreadFeeMultiplier = 2.0;
    constexpr double kMinSpreadFeePremiumBps = 1.0;

    struct SpreadClass {
        double threshold_bps;
        double min_spread_bps;
    };

    constexpr std::array<SpreadClass, 7> kSpreadTable = {{
       { .threshold_bps=kSpreadClassUltraWideBps, .min_spread_bps=kMinSpreadUltraWideBps },
       { .threshold_bps=kSpreadClassWideBps,      .min_spread_bps=kMinSpreadWideBps      },
       { .threshold_bps=kSpreadClassMidBps,       .min_spread_bps=kMinSpreadMidBps       },
       { .threshold_bps=kSpreadClassTightBps,     .min_spread_bps=kMinSpreadTightBps     },
       { .threshold_bps=kSpreadClassVeryTightBps, .min_spread_bps=kMinSpreadVeryTightBps },
       { .threshold_bps=kSpreadClassUltraUltraTightBps, .min_spread_bps=kMinSpreadUltraTightBps   },
       { .threshold_bps=0.0,                            .min_spread_bps=kMinSpreadUltraUltraTightBps },
    }};

// ─── Skew / мультипликаторы ─────────────────────────────────────
    constexpr double   kObiSkewMax              = 0.30;
    constexpr double   kOfiSkewMax              = 0.15;
    constexpr double   kHmaSkewMaxFactor        = 0.05;
    constexpr double   kHmaSkewThresholdBps     = 0.5;
    constexpr double   kHmaSkewScaleBps         = 30.0;
    constexpr double   kMomentumMaxDriftPct     = 0.00005;
    constexpr double   kQtyInventoryPenalty     = 0.7;
    constexpr double   kToxicQtyMult            = 0.3;
    constexpr double   kIlliquidQtyMult         = 0.5;
    constexpr double   kToxicHalfSpreadMult     = 1.5;
    constexpr double   kIlliquidHalfSpreadMult  = 2.0;
    constexpr double   kToxicBlockRatio         = 0.30;
    constexpr double   kPositionRiskBlockRatio  = 0.5;
    constexpr double   kQtyRatioSoftExit        = 0.15;
    constexpr double   kSkewUrgencyMult         = 1.5;

// ─── OBI / OFI / VPIN ───────────────────────────────────────────
    constexpr double   kObiEmaAlpha             = 0.3;
    constexpr double   kObiEmaKeep              = 0.7;
    constexpr double   kObiBlockThreshold       = 0.4;
    constexpr double   kVpinMultThreshold       = 0.5;
    constexpr double   kVpinMultSlope           = 2.0;
    constexpr int      kVpinWindow              = 20;
    constexpr int      kObiLevels               = 5;
    constexpr int      kOfiSumLevels            = 100;

// ─── Sigma / волатильность ─────────────────────────────────────
    constexpr double   kSigmaEmaAlpha           = 0.05;
    constexpr double   kStressVolRatioThreshold = 1.3;
    constexpr double   kStressSpreadMultBase    = 1.5;
    constexpr double   kSigmaRatioClampLo       = 0.5;
    constexpr double   kSigmaRatioClampHi       = 3.0;

// ─── Drift / stale ──────────────────────────────────────────────
    constexpr double   kDriftThresholdMinBps    = 10.0;
    constexpr double   kDriftThresholdSigmaMult = 1.5;
    constexpr double   kStaleBidDistMinBps      = 10.0;
    constexpr double   kStaleBidDistMaxBps      = 300.0;
    constexpr double   kMoveThrTier1            = 2.0;
    constexpr double   kMoveThrDefault          = 0.5;

// ─── Дополнительные (для устранения остатков) ───────────────
    constexpr double   kPctToBps                = 10'000.0;
    constexpr double kTickRoundEpsilon          = 1e-9;

// Инвентарь / действия
    constexpr double   kMinInventoryUsdForAction   = 1.0;   // KILL, max-hold, urgency
    constexpr double   kActionableInventoryUsd     = 5.0;   // TREND-cut, FAST_ADVERSE

// Trend-cut
    constexpr double   kTrendThresholdSpreadFactor = 0.3;
    constexpr double   kMinTrendThresholdBps       = 20.0;

// Inventory penalty
    constexpr double   kInvPenaltyQuadFactor       = 0.5;

// Side-bias half-spread
    constexpr double   kSideBiasActivationRatio    = 0.05;
    constexpr double   kSideBiasHalfSpreadSlope    = 0.5;

// OFI clamp
    constexpr double   kOfiNormClampAbs            = 3.0;

// Tick-size discovery
    constexpr double   kTickProbeMult              = 1.001;
    constexpr double   kTickFallbackFraction       = 0.0001;

// Gap from BBO
    constexpr double   kMinGapBps                  = 1.0;

// Momentum close replace
    constexpr double   kMomentumCloseReplaceTicks  = 2.0;

// Drift threshold
    constexpr double   kDriftSigmaSeconds          = 60.0;

// Toxic-markout filter
    constexpr uint32_t kToxicMinMarkouts4h         = 20;
    constexpr double   kToxicMinHitPct1s           = 20.0;

// Momentum history
    constexpr size_t   kMinHistoryForMomentum      = 3;

    constexpr double kHalfTickFactor = 0.5;
    constexpr double kOneTickFactor  = 1.0;
    constexpr double kTwoTicksFactor = 2.0;

    [[nodiscard]] static auto floorToTick(double p, double tick) noexcept -> double
    {
        return std::floor((p / tick) + kTickRoundEpsilon) * tick;
    }

    [[nodiscard]] static auto ceilToTick(double p, double tick) noexcept -> double
    {
        return std::ceil((p / tick) - kTickRoundEpsilon) * tick;
    }

    struct RegimeTuning { double half_spread_mult = 1.0; double qty_mult = 1.0; };

    [[nodiscard]] constexpr auto regimeTuning(mm::SymMode m) noexcept -> RegimeTuning
    {
        switch (m) {
            case mm::SymMode::Toxic:    return {.half_spread_mult=1.5, .qty_mult=0.3};
            case mm::SymMode::Illiquid: return {.half_spread_mult=2.0, .qty_mult=0.5};
            default:                    return {.half_spread_mult=1.0, .qty_mult=1.0};
        }
    }


    inline auto mm_now_ms() -> uint64_t {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
    }

    inline auto make_reject_id(const std::string& side) -> std::string {
        static std::atomic<uint64_t> seq{0};
        return "REJ_" + std::to_string(mm_now_ms()) + "_" + side
                   + "_" + std::to_string(seq.fetch_add(1, std::memory_order_relaxed));
    }

}


class MarketMakingStrategy : public std::enable_shared_from_this<MarketMakingStrategy> {
public:
    void set_regime_snapshot(const mm::SymModeSnapshot& s) { regime_ = s; }
    auto regime() const -> const mm::SymModeSnapshot& { return regime_; }
    void set_executor(net::any_io_executor exec) {
        exec_ = std::move(exec);
        has_exec_ = true;
    }

    void set_tp_engine(std::shared_ptr<tp::TruePriceEngine> e) { tp_engine_ = std::move(e); }
    auto get_tp_engine() const -> tp::TruePriceEngine* { return tp_engine_.get(); }
    void cancel_ask_on_through(double trade_price, const std::string& reason);
    void cancel_bid_on_through(double trade_price, const std::string& reason);

    // Debug accessors (tests only)
    std::map<std::string,bool> debug_pending_markouts_is_close() const {
        std::map<std::string,bool> out;
        for (const auto& [k, v] : pending_markouts_) out[k] = v.is_close;
        return out;
    }

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
        bool     is_close        = false;
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
            prices_.emplace_back(ts_ms, price);
            while (prices_.size()  > MAX_SAMPLES) {
                prices_.pop_front();
            }
            while (returns_.size() > MAX_SAMPLES) {
                returns_.pop_front();
            }
        }

        auto get_sigma_per_sqrt_sec(double& out) const -> bool {
            if (returns_.size() < MIN_SAMPLES || prices_.size() < 2) {
                return false;
            }

            double mean = 0.0;
            for (double r : returns_) {
                mean += r;
            }

            mean /= static_cast<double>(returns_.size());
            double var = 0.0;
            for (double r : returns_) {
                double d = r - mean; var += d * d;
            }
            var /= static_cast<double>(returns_.size() - 1);
            uint64_t dt_ms = prices_.back().first - prices_.front().first;
            if (dt_ms == 0) {
                return false;
            }
            double avg_dt_sec = (dt_ms / 1000.0) / static_cast<double>(returns_.size());
            if (avg_dt_sec <= 0) {
                return false;
            }

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
        double mid {};
        double best_bid {};
        double best_ask {};
        double half_spread {};
        double skew {};
        double sigma {};
        double q_ratio {};
        double inv_usd {};
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

// ═══ Ranking: expected value для momentum ═══
    struct MomentumSignal {
        std::string symbol;
        std::string venue;         // "bybit", "bitunix"
        int         side;          // +1 buy, -1 sell
        double      hawkes_bps;
        double      spread_ticks;
        double      fill_prob;
        double      duration_factor;
        double      adverse_bps;
        double      score_bps;     // итоговый EV
        bool        is_valid;
    };
    // ── Ranking momentum ──
    auto compute_momentum_signal(uint64_t now) const -> MomentumSignal;
    void set_momentum_selected(bool sel) { momentum_selected_ = sel; }
    auto is_momentum_enabled() const -> bool { return momentum_enabled_; }
    auto get_symbol() const -> const std::string& { return symbol_; }
    auto get_venue() const -> std::string { return is_bitunix_symbol_ ? "bitunix" : "bybit"; }

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
                         arb::IExchange& exchange,
                         std::string  symbol,
                         double base_spread_pct   = 0.002,
                         double order_qty_usd     = 15.0,
                         double max_position_usd  = 60.0);

    void tick();
    void set_db_connection(std::shared_ptr<pqxx::connection> conn) { db_conn_ = conn; }
    auto get_inventory_usd() const -> double { return inventory_usd_; }

    // Callback из poll_mm_fills. Если параметры переданы — используется точная цена/time
    void on_order_filled(const std::string& order_id,
                         uint64_t fill_ts_ms,
                         double fill_price,
                         double fill_qty,
                         const std::string& side,
                         bool is_full_fill = true);   // ← NEW

    auto get_inventory_qty() const -> double { return inventory_qty_; }

    // ─────────────────────────────────────────────────────────
    // Debug accessors (used by tests/test_sprint1.cpp).
    // Unconditional — 6 inline one-liners, никакого влияния на prod.
    // ─────────────────────────────────────────────────────────
    void debug_set_bid(const std::string& oid, double px) {
        bid_order_id_ = oid;
        last_bid_p_   = px;
    }
    void debug_set_ask(const std::string& oid, double px) {
        ask_order_id_ = oid;
        last_ask_p_   = px;
    }
    const std::string& debug_bid_oid() const { return bid_order_id_; }
    const std::string& debug_ask_oid() const { return ask_order_id_; }

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

    auto get_last_half_spread() const -> double { return last_half_spread_; }
    auto get_adaptive_params() const -> const AdaptiveController::Params& { return adaptive_params_; }
    void set_run_tag(const std::string& tag) { run_tag_ = tag; }
    void set_momentum_weight(double w) { momentum_weight_ = std::max(0.0, w); }
    void reset_vol_estimator() {
        vol_estimator_.reset();
        last_sigma_ = sigma_ref_;
        mid_history_.clear();
        last_trend_log_ms_ = 0;
    }
    auto refresh_position() -> bool;
    void on_trade_through_ask(double price, double size);
    void on_trade_through_bid(double price, double size);

    // Инжекция RegimeFilter (опционально, nullptr = не использовать)
    void set_regime_filter(RegimeFilter* rf) { regime_filter_ = rf; }

// Пороги для VPIN и multi-level spread
    void set_vpin_thresholds(double vpin_pause, double vpin_resume) {
        vpin_pause_ = vpin_pause;
        vpin_resume_ = vpin_resume;
    }

    auto maker_fee_bps() const -> double { return maker_fee_bps_; }
    void set_maker_fee_bps(double v) { maker_fee_bps_ = v; }

    // Graceful shutdown: отменить ордера, закрыть позиции (для hot-disable)
    void shutdown_gracefully(const std::string& reason = "GRACEFUL_SHUTDOWN");
    // true когда cleanup после shutdown_gracefully() полностью выполнен.
    // Внешний loop должен поллить это перед io_context.stop().
    bool is_shutdown_complete() const {
        return shutdown_complete_.load(std::memory_order_acquire);
    }

    // Публичный доступ к inventory для disable-логики
    auto inventory_usd() const -> double { return inventory_usd_; }
    void set_max_hold_sec(int sec) { max_hold_override_sec_ = std::max(0, sec); }

    auto drain_finalized_markouts() -> std::vector<FinalizedMarkout>;

    void set_momentum_mode(bool enabled, double qty_usd = 100.0) {
        momentum_enabled_ = enabled;
        momentum_qty_usd_ = qty_usd;
    }
    void set_momentum_threshold(double thr_bps) { momentum_thr_ = thr_bps; }
    void close_position_market(double best_bid, double best_ask,
                               const std::string& reason = "HARD_CAP");
private:


    std::shared_ptr<tp::TruePriceEngine> tp_engine_;

    enum class QuoteSide { Bid, Ask };

    struct QuotePrices { double bid; double ask; };

    [[nodiscard]] auto computeQuotePrices(
            double micro, double bid_half, double ask_half,
            double skew, double momentum_skew, double obi_skew,
            double ofi_skew, double cross_skew,
            double best_bid, double best_ask, double tick_size) const -> QuotePrices;

    void maintainQuoteSide(QuoteSide side,
                           bool allow,
                           bool needs_replace,
                           double px,
                           double qty,
                           const PlaceContext& ctx,
                           uint64_t now);
    void cancelStaleIfNeeded(QuoteSide side, double mid, uint64_t now);

    struct GateInputs {
        double   q_ratio         = 0.0;
        double   bid_price       = 0.0;
        double   ask_price       = 0.0;
        double   best_bid        = 0.0;
        double   best_ask        = 0.0;
        double   bid_block_ratio = 0.0;
        double   ask_block_ratio = 0.0;
        double   drift_bps       = 0.0;
        double   drift_thr_bps   = 0.0;
        uint64_t now             = 0;
    };

    struct SideGates {
        bool        allow_bid        = true;
        bool        allow_ask        = true;
        const char* bid_block_reason = "";
        const char* ask_block_reason = "";
    };

    [[nodiscard]] auto evaluateGates(const GateInputs& in) -> SideGates;


    bool     momentum_selected_ = false;
    HmaFilter    hma_fast_{8,  1000};   // 8 секунд, быстрый тренд
    HmaFilter    hma_slow_{32, 1000};   // 32 секунды, медленный тренд
    KernelFilter kernel_{120'000, 30'000};   // 2 мин окно, 30s bandwidth
    auto compute_hma_skew_bps() const -> double;
    double hma_skew_bps_ = 0.0;
    uint64_t pending_close_first_attempt_ms_ = 0;
    auto now() const -> uint64_t {
        if (backtest_now_ms_ > 0) {
            return backtest_now_ms_;
        }
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
    arb::IExchange& api_;
    std::string     symbol_;
    int consecutive_invalid_pos_ = 0;
    double base_spread_pct_;
    double order_qty_usd_;
    double max_position_usd_;
    std::atomic<bool> pending_cancel_ask_{false};
    std::atomic<bool> pending_cancel_bid_{false};
    // ─────────────────────────────────────────────────────────────
    // Trigger-цены из WS thread. Хранятся как min (bid) / max (ask),
    // чтобы tick() получил «худший» трейд за интервал.
    // Sentinel — очень далеко от реальных цен.
    // ─────────────────────────────────────────────────────────────
    static constexpr double kNoPriceSentinel = 1e100;
    std::atomic<double> pending_cancel_ask_trigger_{ -kNoPriceSentinel };
    std::atomic<double> pending_cancel_bid_trigger_{  kNoPriceSentinel };

    // Только tick() вызывает. Внутри — сетевой cancel + log + clear state.
    void cancel_ask_now(const std::string& reason);
    void cancel_bid_now(const std::string& reason);

    double      inventory_usd_ = 0.0;
    double      inventory_qty_ = 0.0;
    std::string inventory_side_;
    uint64_t    position_opened_time_ = 0;

    std::string bid_order_id_;
    std::string ask_order_id_;

    std::shared_ptr<pqxx::connection> db_conn_;

    // ─────────────────────────────────────────────────────────────
    // Deferred fills — единственный канал из ASIO fill-thread в tick-thread
    // ─────────────────────────────────────────────────────────────
    struct DeferredFill {
        std::string order_id;
        uint64_t    fill_ts_ms;
        double      fill_price;
        double      fill_qty;
        std::string side;
        bool        is_full_fill;
    };
    mutable std::mutex        deferred_fill_mtx_;
    std::vector<DeferredFill> deferred_fills_;

    // Timestamp последнего применённого fill (на tick-thread).
    // refresh_position() пропускается если now - last_fill_processed_ms_
    // < kRefreshAfterFillCooldownMs — API может быть ещё stale.
    uint64_t last_fill_processed_ms_ = 0;

    void process_deferred_fills(uint64_t now);
    void apply_fill_internal(const std::string& order_id,
                             uint64_t fill_ts_ms,
                             double   fill_price,
                             double   fill_qty,
                             const std::string& side,
                             bool     is_full_fill);

    // ─────────────────────────────────────────────────────────────
    // shutting_down_ — читается из handleEarlyExits (tick),
    // пишется из shutdown_gracefully (может быть любая нить)
    // ─────────────────────────────────────────────────────────────
    std::atomic<bool> shutting_down_{false};
    std::atomic<bool> shutdown_complete_{false};
    void shutdown_do_work(const std::string& reason);

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
        bool        is_close = false;
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
    bool     momentum_enabled_              = false;
    bool     momentum_active_               = false;
    double   momentum_skew_                 = 0.0;
    uint64_t momentum_entry_ms_             = 0;
    int      momentum_retry_                = 0;
    uint64_t momentum_last_entry_attempt_ms_ = 0;   // ← cooldown
    double   momentum_entry_px_       = 0.0;   // ← ДОБАВИТЬ
    double   momentum_entry_qty_      = 0.0;   // ← ДОБАВИТЬ
    int8_t   momentum_entry_sign_     = 0;
    std::string momentum_close_oid_;              // текущий close order
    double      momentum_close_px_  = 0.0;        // его цена
    uint64_t    momentum_close_ts_  = 0;          // когда поставили
    uint64_t    momentum_max_ms_    = 30'000;     // было 5000 — maker close нужен запас
    double   momentum_qty_usd_              = 100.0;
    double   momentum_thr_                  = 10.0;
    double   momentum_tp_bps_               = 2.0;
    uint64_t momentum_cooldown_ms_          = 3'000;   // 60s между entries
    // ═══ Toxic fill detection (dynamic exit) ═══
    bool        toxic_check_pending_ = false;
    std::string toxic_check_side_;
    double      toxic_mid_at_fill_ = 0.0;
    int         adverse_fill_count_ = 0;
    double entry_price_ = 0.0;
    double   obi_ema_ = 0.0;
    uint64_t obi_ema_last_ms_ = 0;

    // ─── Tick pipeline ───────────────────────────────────────
    void   logBlockingState(uint64_t now);

    void   cancelAllOrders(const char* reason);     // nullptr = без log_cancel
    void   cancelBidIfAny(const char* reason);
    void   cancelAskIfAny(const char* reason);

    void   processPendingClose(uint64_t now, double best_bid, double best_ask);

    // Возвращает true, если tick «съеден» (нужно выйти из tick()).
    auto   handleMomentumMode(uint64_t now, double mid,
                              double best_bid, double best_ask,
                              double tick_size) -> bool;
    auto   handleMaxHold(uint64_t now, double mid,
                         double best_bid, double best_ask) -> bool;
    bool handleTrendCut(uint64_t now, double mid, double best_bid, double best_ask,
                        double sigma);

    // ─── Вычисления ──────────────────────────────────────────
    auto computeMomentumSkew(uint64_t now, double mid) const -> double;
    auto computeTimeUrgency(uint64_t now) const -> double;
    auto computeDrift60sBps(uint64_t now, double mid) const -> double;

    // Возвращает mid из истории, ближайший к now-window_ms;
    // если не нашли — самый старый; если история пуста — fallback.
    auto findMidAgo(uint64_t now, uint64_t window_ms, double fallback) const -> double;

    void   updateSigmaEma(uint64_t now, double sigma);
    void   pushMidHistory(uint64_t now, double mid);

    // ─── Логи ────────────────────────────────────────────────
    void   logDriftState(uint64_t now, double drift_bps,
                         double thr_bps, double sigma);
    void   logTickEnd(uint64_t now, double mid,
                      double bid_px, double ask_px,
                      bool allow_bid, bool allow_ask,
                      double q_ratio, double half_spread,
                      double sigma, double hma_skew_bps);

    // ─── Struct-ы результатов ────────────────────────────────
    struct VolState {
        double   sigma              = 0.0;
        double   vol_ratio          = 1.0;
        double   stress_mult_spread = 1.0;
        double   stress_mult_qty    = 1.0;
        bool     cancel_all         = false;
    };

    struct SpreadState {
        double half_spread = 0.0;
        double sigma_ratio = 1.0;
        double eff_qty_usd = 0.0;
    };

    struct Signals {
        double q_ratio       = 0.0;
        double obi           = 0.0;
        double ofi_norm      = 0.0;
        double momentum_skew = 0.0;
    };

    struct SideHalves {
        double bid = 0.0;
        double ask = 0.0;
    };

    struct CrossSkew {
        double skew   = 0.0;   // в долях mid
        double hawkes = 0.0;   // bps
        double shadow = 0.0;   // bps
    };

    struct Qtys {
        double bid       = 0.0;
        double ask       = 0.0;
        bool   allow_bid = true;
        bool   allow_ask = true;
    };

    struct ReplaceFlags {
        bool   bid       = false;
        bool   ask       = false;
        double bid_price = 0.0;   // может быть «снапнута» на last
        double ask_price = 0.0;
    };

    using RegimeTuning = mm_cfg::RegimeTuning;
    // ─── Методы ──────────────────────────────────────────────
    auto handleToxicFillCheck(uint64_t now, double mid,
                              double best_bid, double best_ask) -> bool;
    auto handleVpinPause(uint64_t now) -> bool;

    auto computeSignals(uint64_t now, double mid) -> Signals;
    auto computeVolatility(uint64_t now) -> VolState;
    auto computeSpreadParams(double mid, double best_bid, double best_ask,
                                    double sigma, const VolState& vol) -> SpreadState;
    auto computeSideHalves(double half_spread, double q_ratio,
                                 double hma_skew_bps,
                                 const RegimeTuning& bid_tuning,
                                 const RegimeTuning& ask_tuning) const -> SideHalves;
    auto computeCrossSkew(uint64_t now, double mid) -> CrossSkew;

    auto computeQuantities(double mid, double q_ratio, double eff_qty_usd,
                           const RegimeTuning& bid_tuning,
                           const RegimeTuning& ask_tuning) -> Qtys;
    auto clampQtyToMinNotional(double q, double mid, mm::SymMode mode) const -> double;

    auto computeReplaceFlags(uint64_t now,
                                     double bid_price, double ask_price,
                                     double bid_half, double ask_half,
                                     double tick_size) const -> ReplaceFlags;

    auto buildPlaceContext(double mid, double best_bid, double best_ask,
                                   double half_spread, double skew, double sigma,
                                   double q_ratio, double hma_skew_bps,
                                   double tp_shadow_bps, uint64_t now) const -> PlaceContext;

    struct MarketSnapshot {
        double mid;
        double best_bid;
        double best_ask;
        double micro;
    };

    auto                            handleEarlyExits(uint64_t now) const -> bool;
    auto   readMarketSnapshot(uint64_t now) const -> std::optional<MarketSnapshot>;
    auto                            refreshPositionIfDue(uint64_t now) -> bool;
    auto                            getTickSize(double mid) const -> double;
    auto                            handleRegimeFilter() -> bool;
    auto                            handleKillSwitch(double best_bid, double best_ask) -> bool;
    auto                            handleHardCap(double best_bid, double best_ask) const -> bool;
    auto                            handleRiskExits(uint64_t now, double q_ratio,
                                                    double best_bid, double best_ask) -> bool;
    auto                          computeInventorySkew(uint64_t now, double half_spread,
                                                         double sigma_ratio, double q_ratio) const -> double;
    void                            updateObiEma(uint64_t now, double obi);

    struct BlockRatios { double bid = 0.0; double ask = 0.0; };
    [[nodiscard]] auto computeBlockRatios() const noexcept -> BlockRatios;

};
