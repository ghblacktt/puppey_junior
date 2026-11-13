#include "market_making_strategy.h"

#include <cmath>
#include "util/console.h"
#include <iostream>
#include <utility>
#include "iexchange.h"

using arb::IExchange;
// ============================================================
// Local helpers
// ============================================================
namespace {
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

namespace mm_cfg {

// ─── Тайминги (ms) ──────────────────────────────────────────────
    constexpr uint64_t kBlockLogIntervalMs       = 30'000;
    constexpr uint64_t kTrendCutCooldownMs       = 60'000;
    constexpr uint64_t kToxicCloseBlockMs        = 300'000;
    constexpr uint64_t kBlacklistBlockMs         = 3'600'000;
    constexpr uint64_t kPositionRefreshMs        = 20'000;
    constexpr uint64_t kBookStaleMs              = 10'000;
    constexpr uint64_t kPendingCloseTimeoutMs    = 45'000;
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
    constexpr double kMinSpreadUltraTightBps = 4.0;   // < 3

// ── Fee floor ──
    constexpr double kMinSpreadFeeMultiplier = 2.0;
    constexpr double kMinSpreadFeePremiumBps = 1.0;

    struct SpreadClass {
        double threshold_bps;
        double min_spread_bps;
    };

    constexpr std::array<SpreadClass, 6> kSpreadTable = {{
                                                                 { .threshold_bps=kSpreadClassUltraWideBps, .min_spread_bps=kMinSpreadUltraWideBps },
                                                                 { .threshold_bps=kSpreadClassWideBps,      .min_spread_bps=kMinSpreadWideBps      },
                                                                 { .threshold_bps=kSpreadClassMidBps,       .min_spread_bps=kMinSpreadMidBps       },
                                                                 { .threshold_bps=kSpreadClassTightBps,     .min_spread_bps=kMinSpreadTightBps     },
                                                                 { .threshold_bps=kSpreadClassVeryTightBps, .min_spread_bps=kMinSpreadVeryTightBps },
                                                                 { .threshold_bps=0.0,                      .min_spread_bps=kMinSpreadUltraTightBps },
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

} // namespace mm_cfg

// ============================================================
// Конструктор
// ============================================================
MarketMakingStrategy::MarketMakingStrategy(
        OrderBook& orderbook,
        ClusterEngine& cluster,
        IExchange& exchange,
        std::string  symbol,
        double base_spread_pct,
        double order_qty_usd,
        double max_position_usd)
        : orderbook_(orderbook),
          cluster_(cluster),
          api_(exchange),
          symbol_(std::move(symbol)),
          base_spread_pct_(base_spread_pct),
          order_qty_usd_(order_qty_usd),
          max_position_usd_(max_position_usd)
{
    adaptive_params_.base_spread_pct = base_spread_pct_;
    adaptive_params_.order_qty_usd   = order_qty_usd_;
    adaptive_params_.max_hold_ms     = mm_cfg::kBaseMaxHoldMs;
    adaptive_params_.enabled         = true;
    base_gamma_ = gamma_;
}

// ============================================================
// refresh_position
// ============================================================
auto MarketMakingStrategy::refresh_position() -> bool {
    auto pos = api_.get_position_info(symbol_);

    if (pos.rate_limited) {
        return false;
    }

    if (!pos.is_valid) {
        consecutive_invalid_pos_++;
        if (consecutive_invalid_pos_ <= 3) {
            std::cerr << "[MM] " << symbol_
                      << " position fetch failed (streak="
                      << consecutive_invalid_pos_
                      << ") code=" << api_.last_ret_code() << "\n";
        }
        if (consecutive_invalid_pos_ < 3) {
            return false;
        }
    }
    if (pos.size <= 0.0) {

        consecutive_invalid_pos_ = 0;
        inventory_qty_ = 0.0;
        inventory_usd_ = 0.0;
        inventory_side_.clear();
        position_opened_time_ = 0;
        last_unrealized_pnl_pct_ = 0.0;
        last_bid_p_ = 0.0;
        last_ask_p_ = 0.0;
        return true;
    }
    consecutive_invalid_pos_ = 0;

    double mid      = orderbook_.get_mid_price();
    double prev_inv = inventory_usd_;
    std::string prev_side = inventory_side_;

    if (mid <= 0) {
        mid = pos.avg_price;
    }

    if (!prev_side.empty() && prev_side != pos.side) {
        position_opened_time_ = 0;
    }

    inventory_qty_  = pos.size;
    inventory_side_ = pos.side;

    double sign    = (pos.side == "Buy") ? 1.0 : -1.0;
    inventory_qty_ = sign * pos.size;              // ← signed (не abs)
    inventory_usd_ = inventory_qty_ * mid;

    if (std::abs(inventory_usd_) > 1.0) {
        last_unrealized_pnl_pct_ = pos.unrealized_pnl / std::abs(inventory_usd_);
    } else {
        last_unrealized_pnl_pct_ = 0.0;
    }

    if (position_opened_time_ == 0) {
        position_opened_time_ = this->now();
    }

    if (std::abs(inventory_usd_ - prev_inv) > 1.0) {
        last_bid_p_ = 0.0;
        last_ask_p_ = 0.0;
    }

    return true;
}

// ============================================================
// close_position_market
// ============================================================
void MarketMakingStrategy::close_position_market(double best_bid, double best_ask,
                                                 const std::string& reason) {
    if (std::abs(inventory_usd_) < 1e-9 && std::abs(inventory_qty_) < 1e-9) {
        return;
    }

    uint64_t now = this->now();
    if (now < close_blocked_until_) {
        return;
    }

    // ── Если уже есть pending close — проверить статус, не блокируя ──
    if (!pending_close_oid_.empty()) {
        auto ord = api_.get_order(symbol_, pending_close_oid_);
        bool filled = (ord.order_status == "Filled");
        bool gone   = ord.order_status.empty();   // не найден — считаем исполненным

        if (filled || gone) {
            pending_close_oid_.clear();
        } else if (ord.order_status == "New" || ord.order_status == "Open"  || ord.order_status == "Pending"  || ord.order_status == "PartiallyFilled") {
            if (now - pending_close_ms_ < 5'000) {
                return;   // ждём, не блокируем
            }
            // таймаут — отменяем и пробуем заново
            api_.cancel_order(symbol_, pending_close_oid_);
            pending_close_oid_.clear();
        } else {
            if (!pending_close_oid_.empty()) {
                api_.cancel_order(symbol_, pending_close_oid_);
            }
            pending_close_oid_.clear();
        }
    }

    if (now - last_close_attempt_ms_ < 1000) {
        return;   // было 3000
    }
    last_close_attempt_ms_ = now;

    if (pending_close_first_attempt_ms_ == 0) {
        pending_close_first_attempt_ms_ = now;
    }

    // ── Отменить существующие MM bid/ask — только при создании ПЕРВОГО close ──
    if (pending_close_oid_.empty()) {   // ← ДОБАВИТЬ
        if (!bid_order_id_.empty()) {
            api_.cancel_order(symbol_, bid_order_id_);
            log_cancel(bid_order_id_, reason);
            bid_order_id_.clear();
            last_bid_p_ = 0.0;
        }
        if (!ask_order_id_.empty()) {
            api_.cancel_order(symbol_, ask_order_id_);
            log_cancel(ask_order_id_, reason);
            ask_order_id_.clear();
            last_ask_p_ = 0.0;
        }
    }

    auto pos_before = api_.get_position_info(symbol_);
    if (!pos_before.is_valid || pos_before.size < 1e-9) {
        if (pos_before.is_valid) {
            inventory_usd_ = 0.0;
            inventory_qty_ = 0.0;
            inventory_side_.clear();
            last_unrealized_pnl_pct_ = 0.0;
            position_opened_time_    = 0;
            pending_close_first_attempt_ms_ = 0;
            pending_close_oid_.clear();
        }
        return;
    }

    std::string close_side = (pos_before.side == "Buy") ? "Sell" : "Buy";
    double qty = api_.normalize_qty(symbol_, pos_before.size);
    if (qty <= 0) {
        // Не можем нормализовать — сбросить прогресс close, попробовать позже
        pending_close_first_attempt_ms_ = 0;
        close_blocked_until_ = now + 5'000;
        close_fail_streak_++;
        if (close_fail_streak_ >= 3) {
            close_blocked_until_ = now + 60'000;
            close_fail_streak_ = 0;
            std::cerr << "[MM] " << symbol_ << " CLOSE FAIL: qty=0 ×3, blocked 60s\n";
        }
        return;
    }

    // ── пассивная цена: используем best_bid/ask напрямую ──
    // ═══ AGGRESSIVE close: пересекаем BBO, гарантированный taker fill ═══
    // Exit-ордер — закрыться любой ценой. Taker fee 5.5 bps дешевле чем drift 30s.
    // ═══ PASSIVE close at opposite BBO ═══
    // Sell close (мы long) → place at best_ask. Ждёт buyer, fills naturally.
    // Buy close (мы short) → place at best_bid. Ждёт seller.
    // Cost: 2 bps maker, не 8.5 bps taker.
    // В paper: fills когда natural flow crosses (30-50% времени).
    // Timeout: 3s → retry (same price). 30s → force aggressive.
    // Если retry только что произошёл — сразу aggressive, не ждём 8s
    bool is_toxic = (regime_.bid.mode == mm::SymMode::Toxic
                     || regime_.ask.mode == mm::SymMode::Toxic);
    uint64_t aggressive_after_ms = is_toxic ? 3'000ULL : 3'000ULL;
    bool force_aggressive = (now - pending_close_first_attempt_ms_ > aggressive_after_ms);
    double tick_size = 0.0;
    {
        auto info = api_.get_instrument_info(symbol_);
        tick_size = info.tick_size;
        if (tick_size <= 0) {
            double probe = api_.normalize_price(symbol_, best_bid * 1.001);
            tick_size = probe - best_bid;
            if (tick_size <= 0) {
                tick_size = best_bid * 0.0001;
            }
        }
    }

    double close_px = NAN;
    if (force_aggressive) {
        close_px = (close_side == "Sell") ? best_bid : best_ask;
    } else {
        // Passive, но подрезаем best — front of queue
        if (close_side == "Sell") {
            close_px = best_ask;   // join ask, maker ✓ — правильно
        } else {
            close_px = best_bid;   // join bid, maker ✓ — правильно
        }
    }
    close_px = api_.normalize_price(symbol_, close_px);

    std::string close_oid = api_.place_limit_order(symbol_, close_side, close_px, qty, 0, 0, 0);

    if (close_oid.empty()) {
        close_fail_streak_++;
        std::cerr << "[MM] " << symbol_ << " " << reason << " place failed"
                  << " code=" << api_.last_ret_code()
                  << " msg=" << api_.last_ret_msg() << "\n";
        if (close_fail_streak_ >= 3) {
            close_blocked_until_ = now + 60'000;
            close_fail_streak_ = 0;
        }
        return;
    }

    // ── НЕБЛОКИРУЮЩАЯ проверка: если агрессивный — филл уже случился ──
    auto p_now = api_.get_position_info(symbol_);
    double mid_now = orderbook_.get_mid_price();
    if (mid_now <= 0) mid_now = close_px;
    bool immediate_fill = (p_now.is_valid && p_now.size * mid_now < 1.0);

    if (immediate_fill) {
        std::cout << "[MM] " << symbol_ << " CLOSE OK [" << reason << "]"
                  << " " << close_side << " qty=" << qty
                  << " order=" << close_oid << "\n";
        inventory_usd_ = 0.0;
        inventory_qty_ = 0.0;
        inventory_side_.clear();
        last_unrealized_pnl_pct_ = 0.0;
        position_opened_time_    = 0;
        close_fail_streak_       = 0;
        pending_close_oid_.clear();
        pending_close_first_attempt_ms_ = 0;
        return;
    }

    // ── Пассивный лимит в книге — запоминаем, ждём через next tick ──
    pending_close_oid_ = close_oid;
    pending_close_ms_  = now;

    std::cout << "[MM] " << symbol_ << " CLOSE PENDING [" << reason << "]"
              << " " << close_side << " qty=" << qty
              << " order=" << close_oid << "\n";
}

// ============================================================
// main tick
// ============================================================
void MarketMakingStrategy::tick()
{
    using namespace mm_cfg;

    const uint64_t now = this->now();

    logBlockingState(now);

    if (handleEarlyExits(now)) {
        return;
    }

    // ═════════════════════════════════════════════════════════════
    // 1. Обработка pending cancel от fill-tracker
    // ═════════════════════════════════════════════════════════════
    if (pending_cancel_ask_.exchange(false, std::memory_order_relaxed)) {
        cancel_ask_now("TOXIC_THROUGH");
    }
    if (pending_cancel_bid_.exchange(false, std::memory_order_relaxed)) {
        cancel_bid_now("TOXIC_THROUGH");
    }

    // ═════════════════════════════════════════════════════════════
    // 2. Периодический refresh позиции
    // ═════════════════════════════════════════════════════════════
    if (!refreshPositionIfDue(now)) {
        return;
    }

    // ═════════════════════════════════════════════════════════════
    // 3. Рыночные данные
    // ═════════════════════════════════════════════════════════════
    const auto mkt_opt = readMarketSnapshot(now);
    if (!mkt_opt) {
        return;
    }
    const double mid      = mkt_opt->mid;
    const double best_bid = mkt_opt->best_bid;
    const double best_ask = mkt_opt->best_ask;
    const double micro    = mkt_opt->micro;

    if (std::abs(inventory_qty_) > kPosEpsilon) {
        inventory_usd_ = inventory_qty_ * mid;
    }

    // ═════════════════════════════════════════════════════════════
    // 4. Toxic fill check (через ~100ms после fill)
    // ═════════════════════════════════════════════════════════════
    if (handleToxicFillCheck(now, mid, best_bid, best_ask)) {
        return;
    }

    // ═════════════════════════════════════════════════════════════
    // 5. Tick size
    // ═════════════════════════════════════════════════════════════
    const double tick_size = getTickSize(mid);

    // ═════════════════════════════════════════════════════════════
    // 6. Pending maker-close: ждём fill / timeout
    // ═════════════════════════════════════════════════════════════
    if (!pending_close_oid_.empty()) {
        processPendingClose(now, best_bid, best_ask);   // вынесено — см. ниже
        return;
    }

    // ═════════════════════════════════════════════════════════════
    // 7. Momentum mode (maker-only) — полностью вынесено
    // ═════════════════════════════════════════════════════════════
    if (tp_engine_ && momentum_enabled_ &&
        handleMomentumMode(now, mid, best_bid, best_ask, tick_size)) {
        return;
    }

    // ═════════════════════════════════════════════════════════════
    // 8. VPIN pause
    // ═════════════════════════════════════════════════════════════
    if (handleVpinPause(now)) {
        return;
    }

    // ═════════════════════════════════════════════════════════════
    // 9. Regime filter
    // ═════════════════════════════════════════════════════════════
    if (handleRegimeFilter()) {
        return;
    }

    // ═════════════════════════════════════════════════════════════
    // 10. Периодические апдейты движков
    // ═════════════════════════════════════════════════════════════
    update_pending_markouts(now);
    vol_estimator_.update(now, mid);
    fill_tracker_.update_mid(mid, now);
    run_adaptive_update(now);
    prune_placed_orders(now);

    if (handleKillSwitch(best_bid, best_ask)) {
        return;
    }

    // ═════════════════════════════════════════════════════════════
    // 11. Max hold
    // ═════════════════════════════════════════════════════════════
    if (handleMaxHold(now, mid, best_bid, best_ask)) {
        return;
    }

    // ═════════════════════════════════════════════════════════════
    // 12. Hard cap
    // ═════════════════════════════════════════════════════════════
    if (handleHardCap(best_bid, best_ask)) {
        return;
    }

    // ═════════════════════════════════════════════════════════════
    // 13. Сигналы / скосы
    // ═════════════════════════════════════════════════════════════
    const auto sig = computeSignals(now, mid);
    const double q_ratio       = sig.q_ratio;
    const double obi           = sig.obi;
    const double ofi_norm      = sig.ofi_norm;
    const double momentum_skew = sig.momentum_skew;

    // ═════════════════════════════════════════════════════════════
    // 14. Risk exits
    // ═════════════════════════════════════════════════════════════
    if (handleRiskExits(now, q_ratio, best_bid, best_ask)) {
        return;
    }

    // ═════════════════════════════════════════════════════════════
    // 15. История и trend-cut
    // ═════════════════════════════════════════════════════════════
    pushMidHistory(now, mid);
    hma_fast_.on_mid(now, mid);
    hma_slow_.on_mid(now, mid);
    kernel_.on_mid(now, mid);

    const double hma_skew_bps = compute_hma_skew_bps();
    hma_skew_bps_ = hma_skew_bps;

    if (handleTrendCut(now, mid, best_bid, best_ask)) {
        return;
    }

    // ═════════════════════════════════════════════════════════════
    // 16. Volatility / stress
    // ═════════════════════════════════════════════════════════════
    const auto vol = computeVolatility(now);
    if (vol.cancel_all) {
        cancelAllOrders("");
        return;
    }
    const double sigma = vol.sigma;

    // ═════════════════════════════════════════════════════════════
    // 17. Расчёт спреда
    // ═════════════════════════════════════════════════════════════
    const auto spread     = computeSpreadParams(mid, best_bid, best_ask, sigma, vol);
    const double sigma_ratio = spread.sigma_ratio;
    const double half_spread = spread.half_spread;

    // ═════════════════════════════════════════════════════════════
    // 18. Скашивание: инвентарь + время
    // ═════════════════════════════════════════════════════════════
    const double skew = computeInventorySkew(now, half_spread, sigma_ratio, q_ratio);

    // ═════════════════════════════════════════════════════════════
    // 19. Regime-adjust спреда по сторонам
    // ═════════════════════════════════════════════════════════════
    const auto bid_tuning = mm_cfg::regimeTuning(regime_.bid.mode);
    const auto ask_tuning = mm_cfg::regimeTuning(regime_.ask.mode);

    const auto halves = computeSideHalves(half_spread, q_ratio, hma_skew_bps,
                                          bid_tuning, ask_tuning);
    const double bid_half = halves.bid;
    const double ask_half = halves.ask;

    const double obi_skew = obi * half_spread * kObiSkewMax;
    const double ofi_skew = ofi_norm * half_spread * kOfiSkewMax;

    // ═════════════════════════════════════════════════════════════
    // 20. Cross-skew (signal engine)
    // ═════════════════════════════════════════════════════════════
    const auto cross_state         = computeCrossSkew(now, mid);
    const double cross_skew    = cross_state.skew;
    const double tp_shadow_bps = cross_state.shadow;

    // ═════════════════════════════════════════════════════════════
    // 21. Цены
    // ═════════════════════════════════════════════════════════════
    const auto quote_px = computeQuotePrices(micro, bid_half, ask_half,
                                       skew, momentum_skew, obi_skew,
                                       ofi_skew, cross_skew,
                                       best_bid, best_ask, tick_size);
    double bid_price = quote_px.bid;
    double ask_price = quote_px.ask;

    if (bid_price >= ask_price) {
        return;
    }

    // ═════════════════════════════════════════════════════════════
    // 22. Gates + side-effects
    // ═════════════════════════════════════════════════════════════

    // block_ratio — вход для gates
    const auto block = computeBlockRatios();

    // OBI-EMA — это обновление state, не гейт, оставляем в tick()
    updateObiEma(now, obi);

    // Drift — переносим сюда из блока 27, чтобы он был входом gates
    const double drift_60s_filter = computeDrift60sBps(now, mid);
    const double drift_thr_bps    = std::max(
            mm_cfg::kDriftThresholdMinBps,
            mm_cfg::kDriftThresholdSigmaMult * sigma
            * std::sqrt(mm_cfg::kDriftSigmaSeconds) * mm_cfg::kPctToBps);

    logDriftState(now, drift_60s_filter, drift_thr_bps, sigma);

    // Единый вызов всех гейтов
    const GateInputs gin{
            .q_ratio         = q_ratio,
            .bid_price       = bid_price,
            .ask_price       = ask_price,
            .best_bid        = best_bid,
            .best_ask        = best_ask,
            .bid_block_ratio = block.bid,
            .ask_block_ratio = block.ask,
            .drift_bps       = drift_60s_filter,
            .drift_thr_bps   = drift_thr_bps,
            .now             = now,
    };
    const SideGates gates = evaluateGates(gin);

    // allow_* остаются изменяемыми: ниже блок 23 (qty) ещё может их выключить
    bool allow_bid = gates.allow_bid;
    bool allow_ask = gates.allow_ask;

    // Cancel-ы — side effect, остаются здесь
    if (!allow_bid && !bid_order_id_.empty()) {
        api_.cancel_order(symbol_, bid_order_id_);
        log_cancel(bid_order_id_, "BLOCKED");
        bid_order_id_.clear();
        last_bid_p_ = 0.0;
    }
    if (!allow_ask && !ask_order_id_.empty()) {
        api_.cancel_order(symbol_, ask_order_id_);
        log_cancel(ask_order_id_, "BLOCKED");
        ask_order_id_.clear();
        last_ask_p_ = 0.0;
    }

    // ═════════════════════════════════════════════════════════════
    // 23. Количества
    // ═════════════════════════════════════════════════════════════
    auto quantities = computeQuantities(mid, q_ratio, spread.eff_qty_usd,
                                        bid_tuning, ask_tuning);
    if (!quantities.allow_bid) { allow_bid = false; cancelBidIfAny("ZERO_QTY"); }
    if (!quantities.allow_ask) { allow_ask = false; cancelAskIfAny("ZERO_QTY"); }
    const double qty_bid = quantities.bid;
    const double qty_ask = quantities.ask;

    // ═════════════════════════════════════════════════════════════
    // 24. Replace-логика (не чаще раза в 5s)
    // ═════════════════════════════════════════════════════════════
    const auto r_flags = computeReplaceFlags(now, bid_price, ask_price,
                                            bid_half, ask_half, tick_size);
    const bool bid_needs_replace = r_flags.bid;
    const bool ask_needs_replace = r_flags.ask;
    bid_price = r_flags.bid_price;
    ask_price = r_flags.ask_price;

    // ═════════════════════════════════════════════════════════════
    // 25. Контекст для логов
    // ═════════════════════════════════════════════════════════════
    const PlaceContext ctx = buildPlaceContext(
            mid, best_bid, best_ask, half_spread, skew, sigma,
            q_ratio, hma_skew_bps, tp_shadow_bps, now);

    // ═════════════════════════════════════════════════════════════
    // 26. Cancel stale ордеров
    // ═════════════════════════════════════════════════════════════
    cancelStaleIfNeeded(QuoteSide::Bid, mid, now);
    cancelStaleIfNeeded(QuoteSide::Ask, mid, now);

    // ═════════════════════════════════════════════════════════════
    // 28. Размещение
    // ═════════════════════════════════════════════════════════════
    maintainQuoteSide(QuoteSide::Bid, allow_bid, bid_needs_replace,
                      bid_price, qty_bid, ctx, now);
    maintainQuoteSide(QuoteSide::Ask, allow_ask, ask_needs_replace,
                      ask_price, qty_ask, ctx, now);

    // ═════════════════════════════════════════════════════════════
    // 29. Логи
    // ═════════════════════════════════════════════════════════════
    logTickEnd(now, mid, bid_price, ask_price,
               allow_bid, allow_ask, q_ratio,
               half_spread, sigma, hma_skew_bps);
}


auto MarketMakingStrategy::computeQuotePrices(
        double micro, double bid_half, double ask_half,
        double skew, double momentum_skew, double obi_skew,
        double ofi_skew, double cross_skew,
        double best_bid, double best_ask, double tick_size) const -> QuotePrices
{
    using namespace mm_cfg;

    const double shift = skew + momentum_skew + obi_skew + ofi_skew + cross_skew;

    double bid_price = micro - bid_half + shift;
    double ask_price = micro + ask_half + shift;

    // Round к tick
    bid_price = floorToTick(api_.normalize_price(symbol_, bid_price), tick_size);
    ask_price = ceilToTick (api_.normalize_price(symbol_, ask_price), tick_size);

    // Гарантированно не пересекаем BBO (min 1 тик)
    const double min_gap = std::max(tick_size * kMinTicksFromTop, best_bid * 1e-4);
    if (bid_price >= best_bid - tick_size * kHalfTickFactor) {
        bid_price = best_bid - min_gap;
    }
    if (ask_price <= best_ask + tick_size * kHalfTickFactor) {
        ask_price = best_ask + min_gap;
    }

    // Min-abs отступ от BBO
    const double min_abs_bid = std::max(tick_size * kMinTicksFromTop,
                                        best_bid * kMinBpsFromTop / kPctToBps);
    const double min_abs_ask = std::max(tick_size * kMinTicksFromTop,
                                        best_ask * kMinBpsFromTop / kPctToBps);

    bid_price = std::min(bid_price, floorToTick(best_bid - min_abs_bid, tick_size));
    ask_price = std::max(ask_price, ceilToTick (best_ask + min_abs_ask, tick_size));

    return {bid_price, ask_price};
}

void MarketMakingStrategy::cancelStaleIfNeeded(QuoteSide side, double mid, uint64_t now) {
    std::string& oid        = (side == QuoteSide::Bid) ? bid_order_id_ : ask_order_id_;
    double&      last_px    = (side == QuoteSide::Bid) ? last_bid_p_    : last_ask_p_;
    uint64_t&    cooldown   = (side == QuoteSide::Bid) ? bid_cooldown_until_ : ask_cooldown_until_;
    const bool   is_bid     = (side == QuoteSide::Bid);

    if (oid.empty() || last_px <= 0.0) return;

    const double dist_bps = is_bid
                            ? (mid - last_px) / last_px * mm_cfg::kPctToBps
                            : (last_px - mid) / last_px * mm_cfg::kPctToBps;

    // Cancel ТОЛЬКО если mid близко к нашему ордеру (adverse imminent)
    // Верхний порог УБРАН — на волатильных символах mid ходит на ±40 bps,
    // и dist > 50 срабатывал каждые 100 мс → 0 fills
    if (dist_bps >= mm_cfg::kStaleBidDistMinBps) {
        return;   // далеко = ok, не отменяем
    }

    api_.cancel_order(symbol_, oid);
    oid.clear();
    last_px  = 0.0;
    cooldown = now + mm_cfg::kStaleQuoteCooldownMs;
}


void MarketMakingStrategy::maintainQuoteSide(QuoteSide side,
                                             bool allow,
                                             bool needs_replace,
                                             double px,
                                             double qty,
                                             const PlaceContext& ctx,
                                             uint64_t now)
{
    if (!allow || !needs_replace) {
        return;
    }

    std::string& oid       = (side == QuoteSide::Bid) ? bid_order_id_ : ask_order_id_;
    double&      last_px   = (side == QuoteSide::Bid) ? last_bid_p_ : last_ask_p_;
    uint64_t&    last_t    = (side == QuoteSide::Bid) ? last_bid_p_time_ms_ : last_ask_p_time_ms_;
    uint64_t&    last_plc  = (side == QuoteSide::Bid) ? last_bid_place_ms_  : last_ask_place_ms_;
    const char*  side_str  = (side == QuoteSide::Bid) ? "Buy" : "Sell";

    if (!oid.empty()) {
        api_.cancel_order(symbol_, oid);
        log_cancel(oid, "REPLACED");
        oid.clear();
        last_px = 0.0;
    }

    oid = api_.place_limit_order(symbol_, side_str, px, qty, 0, 0, 0);
    if (oid.empty()) {
        const int rc = api_.last_ret_code();
        if (rc == 110126) {
            adaptive_params_.enabled = false;
            return;
        }
        if (rc == -1) {
            return;
        }
        log_reject(side_str, px, qty, ctx, rc, api_.last_ret_msg());
        return;
    }

    log_order(oid, side_str, px, qty, ctx);
    last_px   = px;
    last_t    = last_plc = now;
    placed_orders_[oid] = {.price = px, .side = side_str, .placed_ms = now};
}
// ============================================================
// Adaptive update — FIX: НЕ выключаем enabled, используем vpin_pause_until_
// ============================================================
void MarketMakingStrategy::run_adaptive_update(uint64_t now) {
    auto b = fill_tracker_.get_stats_2s(+1);
    auto s = fill_tracker_.get_stats_2s(-1);

    bool urgent = (b.n >= 10 && b.p25 < -30.0)
                  || (s.n >= 10 && s.p25 < -30.0);

    uint64_t interval = urgent ? 60'000 : 300'000;
    if (now - last_adapt_ms_ < interval) return;
    last_adapt_ms_ = now;

    if (urgent) {
        if (!bid_order_id_.empty()) {
            api_.cancel_order(symbol_, bid_order_id_);
            log_cancel(bid_order_id_, "TOXIC_KILL");
            bid_order_id_.clear();
            last_bid_p_ = 0.0;
        }
        if (!ask_order_id_.empty()) {
            api_.cancel_order(symbol_, ask_order_id_);
            log_cancel(ask_order_id_, "TOXIC_KILL");
            ask_order_id_.clear();
            last_ask_p_ = 0.0;
        }
        // ✅ НЕ трогаем adaptive_params_.enabled — используем отдельный таймер
        vpin_pause_until_ = now + 30'000;
        std::cerr << "[MM] " << symbol_
                  << " TOXIC KILL: adverse p25=" << b.p25
                  << ", pause 30s\n";
        return;
    }

    auto old = adaptive_params_;
    adaptive_params_ = controller_.recompute(fill_tracker_, adaptive_params_, maker_fee_bps_);
    adaptive_params_.max_hold_ms = std::clamp<uint64_t>(adaptive_params_.max_hold_ms, 30'000, 60'000);
    bool changed =
            std::abs(old.base_spread_pct - adaptive_params_.base_spread_pct) > 1e-5 ||
            std::abs(old.order_qty_usd   - adaptive_params_.order_qty_usd)   > 0.5  ||
            old.max_hold_ms != adaptive_params_.max_hold_ms ||
            old.enabled     != adaptive_params_.enabled;

    if (changed && (now - last_log_adapt_ms_ > 60'000)) {
        std::cout << "[ADAPT] " << symbol_
                  << " | n_buy=" << b.n << " n_sell=" << s.n
                  << " | adv_p25_buy="  << b.p25
                  << " adv_p25_sell="   << s.p25
                  << " | spread=" << adaptive_params_.base_spread_pct
                  << " qty=$"     << adaptive_params_.order_qty_usd
                  << " hold="     << adaptive_params_.max_hold_ms / 1000 << "s"
                  << " enabled="  << (adaptive_params_.enabled ? 1 : 0)
                  << " | urgent=" << (urgent ? 1 : 0)
                  << "\n";
        last_log_adapt_ms_ = now;
    }
}

// ============================================================
// prune_placed_orders
// ============================================================
void MarketMakingStrategy::prune_placed_orders(uint64_t now) {
    for (auto it = placed_orders_.begin(); it != placed_orders_.end(); ) {
        if (now - it->second.placed_ms > 600'000) it = placed_orders_.erase(it);
        else                                       ++it;
    }
}

// ============================================================
// on_order_filled
// ============================================================
void MarketMakingStrategy::on_order_filled(const std::string& order_id,
                                           uint64_t fill_ts_ms,
                                           double fill_price,
                                           double fill_qty,
                                           const std::string& side,
                                           bool is_full_fill) {
    double      p = fill_price;
    std::string s = side;

    auto it = placed_orders_.find(order_id);
    if (it != placed_orders_.end()) {
        if (p <= 0)    p = it->second.price;
        if (s.empty()) s = it->second.side;
        placed_orders_.erase(it);
    }

    if (fill_qty <= 0) fill_qty = 0.0;

    // ═══ FIX №1: определяем is_close_fill ДО обновления inventory ═══
    bool is_close_fill = false;
    double entry_px_for_pnl = entry_price_;   // ← нужно поле в классе
    if (p > 0 && !s.empty() && std::abs(inventory_qty_) > 1e-9) {
        bool was_long = inventory_qty_ > 0;
        bool is_sell  = (s == "Sell");
        if ((was_long && is_sell) || (!was_long && !is_sell)) {
            is_close_fill = true;
        }
    }

    if (p > 0 && !s.empty()) {
        uint64_t ts = fill_ts_ms > 0 ? fill_ts_ms : this->now();
        int side_sign = (s == "Buy") ? +1 : -1;

        // ═══ FIX №3: обновляем entry_price_ ═══
        double old_qty = inventory_qty_;
        double new_qty = old_qty + side_sign * fill_qty;
        if (!is_close_fill) {
            // Открытие или увеличение — обновляем среднюю entry
            if (std::abs(old_qty) < 1e-9) {
                entry_price_ = p;   // новая позиция
            } else {
                entry_price_ = (entry_price_ * std::abs(old_qty) + p * fill_qty)
                               / std::abs(new_qty);
            }
        } else {
            // Закрытие — не трогаем entry (или сбрасываем при полном)
            if (std::abs(new_qty) < 1e-9) {
                // полное закрытие — можно сбросить, но нужно для PnL ниже
            }
        }

        inventory_qty_ = new_qty;
        inventory_usd_  = inventory_qty_ * p;

        if (std::abs(inventory_qty_) < 1e-9) {
            inventory_side_.clear();
            position_opened_time_ = 0;
        } else {
            inventory_side_ = (inventory_qty_ > 0) ? "Buy" : "Sell";
            if (position_opened_time_ == 0) position_opened_time_ = ts;
        }

        fill_tracker_.record_fill(p, side_sign, ts);

        // ── Markout tracker (open fills) ──
        if (fill_qty > 1e-9 && !is_close_fill) {
            PendingMarkout m;
            m.order_id = order_id;
            m.side = s;
            m.qty = fill_qty;
            m.fill_price = p;
            m.fair_price_at_fill = orderbook_.get_microprice();
            if (m.fair_price_at_fill <= 0.0)
                m.fair_price_at_fill = orderbook_.get_mid_price();
            if (m.fair_price_at_fill <= 0.0)
                m.fair_price_at_fill = p;
            m.fill_time_ms = ts;
            static std::atomic<uint64_t> _fill_seq{0};
            std::string markout_key = order_id + "_" + std::to_string(_fill_seq.fetch_add(1));
            pending_markouts_[markout_key] = m;

            double fair = m.fair_price_at_fill;
            double spread_income = side_sign * (fair - p) * fill_qty;
            double fee_usd = p * fill_qty * maker_fee_bps_ / 10000.0;
            spread_income_total_ += spread_income;
            fees_total_ += fee_usd;

            if (db_conn_ && db_conn_->is_open()) {
                try {
                    pqxx::work txn(*db_conn_);
                    txn.exec(
                            "INSERT INTO mm_pnl_attribution "
                            "(order_id, symbol, side, qty, fill_price, fair_price, run_tag, "
                            " spread_income, fee_usd, fill_time_ms) "
                            "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10)",
                            pqxx::params{order_id, symbol_, s, fill_qty, p, fair, run_tag_,
                                         spread_income, fee_usd, ts});
                    txn.commit();
                } catch (const std::exception& e) {
                    std::cerr << "[MM-DB] attribution error: " << e.what() << "\n";
                }
            }
        } else if (fill_qty > 1e-9 && is_close_fill) {
            PendingMarkout m;
            m.order_id = order_id;
            m.side = s;
            m.qty = fill_qty;
            m.fill_price = p;
            m.fair_price_at_fill = orderbook_.get_mid_price();
            m.fill_time_ms = ts;
            pending_markouts_[order_id + "_close_" + std::to_string(ts)] = m;
        }

        // ═══ FIX №2: post-loss blacklist с PnL от ENTRY ═══
        if (is_close_fill && entry_px_for_pnl > 0) {
            double pnl_bps = 0.0;
            if (s == "Sell") {   // закрытие long
                pnl_bps = (p - entry_px_for_pnl) / entry_px_for_pnl * 10000.0;
            } else {             // закрытие short
                pnl_bps = (entry_px_for_pnl - p) / entry_px_for_pnl * 10000.0;
            }

            if (pnl_bps < -20.0) {
                adverse_fill_count_++;
                close_blocked_until_ = this->now() + 300'000;   // 5 мин
                std::cerr << "[MM-POST-LOSS] " << symbol_
                          << " close_side=" << s
                          << " entry=" << entry_px_for_pnl
                          << " exit=" << p
                          << " pnl_bps=" << std::fixed << std::setprecision(1) << pnl_bps
                          << " count=" << adverse_fill_count_
                          << " → block 5min\n" << std::defaultfloat;

                if (adverse_fill_count_ >= 2) {
                    close_blocked_until_ = this->now() + 3'600'000;   // 1 час
                    std::cerr << "[MM-BLACKLIST] " << symbol_
                              << " disabled 1h (2+ adverse closes)\n";
                }
            }

            // сброс entry при полном закрытии
            if (std::abs(inventory_qty_) < 1e-9) {
                entry_price_ = 0.0;
            }
        }
    }

    if (order_id == bid_order_id_) {
        if (is_full_fill) {
            bid_order_id_.clear();
            last_bid_p_ = 0.0;
        }
    }
    if (order_id == ask_order_id_) {
        if (is_full_fill) {
            ask_order_id_.clear();
            last_ask_p_ = 0.0;
        }
    }

    if (order_id == pending_close_oid_) {
        pending_close_oid_.clear();
        pending_close_ms_ = 0;
        pending_close_first_attempt_ms_ = 0;
    }

    // cooldown 10s
    uint64_t _now = this->now();
    if (s == "Buy")  bid_cooldown_until_ = _now + 10'000;
    if (s == "Sell") ask_cooldown_until_ = _now + 10'000;
}

// ============================================================
// TOXIC DETECTION
// ============================================================
void MarketMakingStrategy::on_trade_through_bid(double trade_price, double qty) {
    if (bid_order_id_.empty()) return;
    if (last_bid_p_ <= 0.0) return;
    if (trade_price > last_bid_p_ + 1e-9) return;   // трейд не через наш bid

    // ═══ СИНХРОННЫЙ cancel, не pending ═══
    api_.cancel_order(symbol_, bid_order_id_);
    log_cancel(bid_order_id_, "TRADE_THROUGH");
    bid_order_id_.clear();
    last_bid_p_ = 0.0;
    bid_cooldown_until_ = this->now() + 1'000;
}

void MarketMakingStrategy::on_trade_through_ask(double trade_price, double qty) {
    if (ask_order_id_.empty()) return;
    if (last_ask_p_ <= 0.0) return;
    if (trade_price < last_ask_p_ - 1e-9) return;

    api_.cancel_order(symbol_, ask_order_id_);
    log_cancel(ask_order_id_, "TRADE_THROUGH");
    ask_order_id_.clear();
    last_ask_p_ = 0.0;
    ask_cooldown_until_ = this->now() + 1'000;
}

// ============================================================
// Логирование
// ============================================================
void MarketMakingStrategy::log_order(const std::string& order_id,
                                     const std::string& side,
                                     double price, double qty,
                                     const PlaceContext& ctx) {
    if (!db_conn_ || !db_conn_->is_open()) return;
    try {
        pqxx::work txn(*db_conn_);
        auto now = this->now();
        txn.exec(
                "INSERT INTO mm_orders "
                "(order_id, symbol, side, price, qty, status, placed_time, run_tag, "
                " mid_at_place, best_bid_at_place, best_ask_at_place, "
                " half_spread_at_place, skew_at_place, sigma_at_place, "
                " q_ratio_at_place, inv_usd_at_place, "
                " cross_bps_at_place, ret_500_at_place, flow_b_at_place, "
                " flow_bnx_at_place, ofi_at_place, delta_at_place, "
                " hma_skew_bps_at_place, hma_fast_at_place, "
                " hma_slow_at_place, kernel_at_place) "
                "VALUES ($1,$2,$3,$4,$5,'OPEN',$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,"
                "$16,$17,$18,$19,$20,$21,$22,$23,$24,$25)",
                pqxx::params{
                        order_id, symbol_, side, price, qty, now, run_tag_,
                        ctx.mid, ctx.best_bid, ctx.best_ask,
                        ctx.half_spread, ctx.skew, ctx.sigma,
                        ctx.q_ratio, ctx.inv_usd,
                        ctx.cross_bps, ctx.ret_500, ctx.flow_b,
                        ctx.flow_bnx, ctx.ofi, ctx.delta,
                        ctx.hma_skew_bps, ctx.hma_fast,
                        ctx.hma_slow, ctx.kernel});
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[MM-DB] log_order error: " << e.what() << "\n";
    }
}

void MarketMakingStrategy::log_cancel(const std::string& order_id,
                                      const std::string& reason) {
    placed_orders_.erase(order_id);

    if (!db_conn_ || !db_conn_->is_open()) return;
    try {
        pqxx::work txn(*db_conn_);
        auto now = this->now();
        const std::string sql =
                "UPDATE mm_orders SET status='CANCELLED', cancel_time=$1, "
                "cancel_reason=$2, order_age_ms=$1 - placed_time "
                "WHERE order_id=$3 AND status IN ('OPEN','PARTIALLY_FILLED')";
        txn.exec(sql, pqxx::params{now, reason, order_id});
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[MM-DB] log_cancel error: " << e.what() << "\n";
    }
}

void MarketMakingStrategy::log_reject(const std::string& side, double price,
                                      double qty, const PlaceContext& ctx,
                                      int ret_code, const std::string& ret_msg) {
    if (!db_conn_ || !db_conn_->is_open()) return;
    try {
        pqxx::work txn(*db_conn_);
        auto now = this->now();

        std::string fake_id = make_reject_id(side);

        txn.exec(
                "INSERT INTO mm_orders "
                "(order_id, symbol, side, price, qty, status, placed_time, run_tag, "
                " ret_code, ret_msg, mid_at_place, best_bid_at_place, best_ask_at_place, "
                " half_spread_at_place, skew_at_place, sigma_at_place, "
                " q_ratio_at_place, inv_usd_at_place) "
                "VALUES ($1,$2,$3,$4,$5,'REJECTED',$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17)",
                pqxx::params{
                        fake_id, symbol_, side, price, qty, now, run_tag_,
                        ret_code, ret_msg,
                        ctx.mid, ctx.best_bid, ctx.best_ask,
                        ctx.half_spread, ctx.skew, ctx.sigma,
                        ctx.q_ratio, ctx.inv_usd});
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[MM-DB] log_reject error: " << e.what() << "\n";
    }
}

void MarketMakingStrategy::logBlockingState(uint64_t now)
{
    static std::unordered_map<std::string, uint64_t> _blk_log;

    if (close_blocked_until_ <= now) {
        return;
    }

    const uint64_t& last = _blk_log[symbol_];
    if (last != 0 && now - last <= mm_cfg::kBlockLogIntervalMs) {
        return;
    }
    _blk_log[symbol_] = now;

    std::cerr << "[MM-BLOCKED] " << symbol_
              << " until=" << (close_blocked_until_ - now) / 1000 << "s"
              << " adverse_count=" << adverse_fill_count_ << "\n";
}

void MarketMakingStrategy::logDriftState(uint64_t now,
                                         double drift_bps,
                                         double thr_bps,
                                         double sigma)
{
    static std::unordered_map<std::string, uint64_t> _drift_log;

    const uint64_t& last = _drift_log[symbol_];
    if (last != 0 && now - last <= mm_cfg::kDriftLogIntervalMs) {
        return;
    }
    _drift_log[symbol_] = now;

    std::cerr << "[MM-DRIFT] " << symbol_
              << " drift_60s=" << std::fixed << std::setprecision(1)
              << drift_bps << "bps"
              << " thr=" << thr_bps << "bps"
              << " sigma=" << (sigma * mm_cfg::kPctToBps) << "bps/sqrt(s)"
              << " blocked_bid=" << (drift_bps < -thr_bps ? "1" : "0")
              << " blocked_ask=" << (drift_bps >  thr_bps ? "1" : "0")
              << std::defaultfloat << "\n";
}

void MarketMakingStrategy::logTickEnd(uint64_t now, double mid,
                                      double bid_px, double ask_px,
                                      bool allow_bid, bool allow_ask,
                                      double q_ratio, double half_spread,
                                      double sigma, double hma_skew_bps)
{
    static std::unordered_map<std::string, RateLimiter> _rl;
    auto& rl = _rl[symbol_];
    if (!rl.ok()) {
        return;
    }

    std::cout << "[TICK-END] " << symbol_
              << " bid=" << bid_px << " ask=" << ask_px
              << " allow_bid=" << (allow_bid ? "1" : "0")
              << " allow_ask=" << (allow_ask ? "1" : "0")
              << " bid_oid="   << (bid_order_id_.empty() ? "-" : bid_order_id_)
              << " ask_oid="   << (ask_order_id_.empty() ? "-" : ask_order_id_)
              << " q_ratio="   << q_ratio
              << "\n";

    const double spread_bps = half_spread * 2.0 / mid * mm_cfg::kPctToBps;
    const double pnl_pct    = last_unrealized_pnl_pct_ * 100.0;
    const char*  pnl_color  = (pnl_pct >= 0.0) ? clr::green : clr::red;
    const char*  inv_color  = (std::abs(inventory_usd_) > max_position_usd_ * 0.5)
                              ? clr::yellow : clr::reset;

    std::cout << clr::gray << fmt_ts() << clr::reset << "  "
              << clr::bold << "MM" << clr::reset << "  "
              << pad_r(symbol_, 14)
              << pad_l(num(bid_px, 4), 10)
              << pad_l(num(ask_px, 4), 10)
              << pad_l(num(spread_bps, 1), 5) << "bps"
              << " " << (hma_skew_bps >= 0 ? clr::green : clr::red)
              << pad_l(num(hma_skew_bps, 1), 5) << clr::reset
              << pad_l(num(std::abs(sigma * mm_cfg::kPctToBps), 1), 5)
              << inv_color << pad_l(num(inventory_usd_, 0), 6) << clr::reset
              << pnl_color << pad_l(num(pnl_pct, 2), 7) << "%" << clr::reset
              << "  " << (adaptive_params_.enabled ? clr::green : clr::red)
              << (adaptive_params_.enabled ? "ok" : "off") << clr::reset
              << "  " << clr::cyan
              << "b:" << mm::to_str(regime_.bid.mode)
              << "/a:" << mm::to_str(regime_.ask.mode)
              << clr::reset << "\n";
}

void MarketMakingStrategy::cancelBidIfAny(const char* reason)
{
    if (bid_order_id_.empty()) {
        return;
    }
    api_.cancel_order(symbol_, bid_order_id_);
    if (reason != nullptr && *reason != '\0') {
        log_cancel(bid_order_id_, reason);
    }
    bid_order_id_.clear();
    last_bid_p_ = 0.0;
}

void MarketMakingStrategy::cancelAskIfAny(const char* reason)
{
    if (ask_order_id_.empty()) {
        return;
    }
    api_.cancel_order(symbol_, ask_order_id_);
    if (reason != nullptr && *reason != '\0') {
        log_cancel(ask_order_id_, reason);
    }
    ask_order_id_.clear();
    last_ask_p_ = 0.0;
}

void MarketMakingStrategy::cancelAllOrders(const char* reason)
{
    cancelBidIfAny(reason);
    cancelAskIfAny(reason);
}
// ============================================================
// Pending maker-close
// ============================================================
void MarketMakingStrategy::processPendingClose(uint64_t now,
                                               double best_bid,
                                               double best_ask)
{
    const auto ord = api_.get_order(symbol_, pending_close_oid_);

    if (ord.order_status == "Filled") {
        pending_close_oid_.clear();
        close_blocked_until_ = 0;
        last_pos_refresh_ms_ = 0;
        return;
    }

    bool need_retry   = false;
    const char* reason = nullptr;

    if (ord.order_status == "Cancelled" || ord.order_status.empty()) {
        pending_close_oid_.clear();
        pending_close_ms_ = 0;
        need_retry = true;
        reason     = "MAX_HOLD_CANCELLED";
    } else if (now - pending_close_ms_ > mm_cfg::kPendingCloseTimeoutMs) {
        api_.cancel_order(symbol_, pending_close_oid_);
        pending_close_oid_.clear();
        pending_close_ms_ = 0;
        need_retry = true;
        reason     = "MAX_HOLD_TIMEOUT";
    }

    if (!need_retry) {
        return;
    }

    bool retried = false;
    if (std::abs(inventory_qty_) > mm_cfg::kPosEpsilon) {
        close_position_market(best_bid, best_ask, reason);
        retried = !pending_close_oid_.empty();
    }
    if (!retried) {
        close_blocked_until_ = now + mm_cfg::kPendingCloseRetryBlockMs;
    }
}

// ============================================================
// Momentum mode
// ============================================================
bool MarketMakingStrategy::handleMomentumMode(uint64_t now, double mid,
                                              double best_bid, double best_ask,
                                              double tick_size)
{
    const double hawkes = tp_engine_->target_skew_bps(now);

    // ═══════════ EXIT ═══════════
    if (momentum_active_) {
        const uint64_t age = (now > momentum_entry_ms_) ? (now - momentum_entry_ms_) : 0;
        const bool has_pos = std::abs(inventory_qty_) > mm_cfg::kPosEpsilon;

        if (age < mm_cfg::kMomentumMinAgeMs) {
            return true;
        }

        double pnl_bps = 0.0;
        if (momentum_entry_px_ > 0.0) {
            pnl_bps = momentum_entry_sign_
                      * (mid - momentum_entry_px_) / momentum_entry_px_
                      * mm_cfg::kPctToBps;
        }

        // Проверяем fill закрывающего ордера ДО проверки has_pos.
        if (!momentum_close_oid_.empty()) {
            const auto ord = api_.get_order(symbol_, momentum_close_oid_);
            if (ord.order_status == "Filled") {
                std::cout << "[MOMENTUM-CLOSE] " << symbol_
                          << " entry_px=" << momentum_entry_px_
                          << " mid_pnl_bps=" << pnl_bps << "\n";
                momentum_close_oid_.clear();
                momentum_active_    = false;
                momentum_retry_     = 0;
                momentum_entry_px_  = 0.0;
                momentum_entry_sign_ = 0;
                momentum_last_entry_attempt_ms_ = now;
                return true;
            }
        }

        // Позиция не открылась — сброс через kMomentumNoFillResetMs.
        if (!has_pos && age > mm_cfg::kMomentumNoFillResetMs) {
            cancelBidIfAny(nullptr);
            cancelAskIfAny(nullptr);

            if (!momentum_close_oid_.empty()) {
                api_.cancel_order(symbol_, momentum_close_oid_);
                momentum_close_oid_.clear();
            }

            momentum_active_    = false;
            momentum_retry_     = 0;
            momentum_entry_px_  = 0.0;
            momentum_entry_sign_ = 0;
            momentum_last_entry_attempt_ms_ = now;
            return true;
        }

        if (!has_pos) {
            return true;
        }

        // Прогрессивный offset (только для лога; цена закрытия — на BBO).
        int offset_ticks = 1;
        if (age > mm_cfg::kMomentumTier15AgeMs)      { offset_ticks = 15; }
        else if (age > mm_cfg::kMomentumTier5AgeMs)  { offset_ticks = 5;  }
        else if (age > mm_cfg::kMomentumTier2AgeMs)  { offset_ticks = 2;  }

        const std::string close_side = (inventory_qty_ > 0) ? "Sell" : "Buy";
        const double      close_qty  = std::abs(inventory_qty_);
        double            close_px   = (close_side == "Sell") ? best_ask : best_bid;
        close_px = api_.normalize_price(symbol_, close_px);

        bool need_replace = false;
        if (momentum_close_oid_.empty()) {
            need_replace = true;
        } else {
            const auto ord = api_.get_order(symbol_, momentum_close_oid_);
            if (ord.order_status == "Cancelled" || ord.order_status.empty()) {
                momentum_close_oid_.clear();
                need_replace = true;
            } else if (std::abs(close_px - momentum_close_px_)
                       > tick_size * mm_cfg::kMomentumCloseReplaceTicks) {
                api_.cancel_order(symbol_, momentum_close_oid_);
                momentum_close_oid_.clear();
                need_replace = true;
            } else if (now - momentum_close_ts_ > mm_cfg::kMomentumCloseTimeoutMs) {
                api_.cancel_order(symbol_, momentum_close_oid_);
                momentum_close_oid_.clear();
                need_replace = true;
            }
        }

        if (need_replace) {
            momentum_close_oid_ = api_.place_limit_order(
                    symbol_, close_side, close_px, close_qty, 0, 0, 0);
            momentum_close_px_ = close_px;
            momentum_close_ts_ = now;

            std::cout << "[MOMENTUM-CLOSE-ATTEMPT] " << symbol_ << " " << close_side
                      << " px="     << close_px
                      << " bb="     << best_bid
                      << " ba="     << best_ask
                      << " off="    << offset_ticks << "t"
                      << " pnl_bps=" << std::fixed << std::setprecision(2)
                      << pnl_bps
                      << " age=" << age
                      << std::defaultfloat << "\n";
        }

        // Hard timeout — сдаёмся без market close.
        if (age > momentum_max_ms_ && has_pos) {
            momentum_active_ = false;
            momentum_last_entry_attempt_ms_ = now;
        }
        return true;
    }

    // ═══════════ ENTRY ═══════════
    const double vpin_now = cluster_.get_vpin(mm_cfg::kVpinWindow);
    const bool   cooldown_ok = (momentum_last_entry_attempt_ms_ == 0)
                               || (now - momentum_last_entry_attempt_ms_ > momentum_cooldown_ms_);
    const bool   vpin_ok     = (vpin_now < vpin_pause_);
    const bool   no_pos      = std::abs(inventory_qty_) < mm_cfg::kPosEpsilon;

    const double spread_ticks_cur = (best_ask - best_bid) / tick_size;
    const double spread_bps_cur   = spread_ticks_cur * (tick_size / mid) * mm_cfg::kPctToBps;

    const bool signal_ok         = std::abs(hawkes) > mm_cfg::kMomentumHawkesGate;
    const bool spread_positive   = (best_ask > best_bid);
    const bool spread_wide_ok    = spread_ticks_cur > mm_cfg::kMomentumMinSpreadTicks;
    const bool spread_bps_ok     = spread_bps_cur >= mm_cfg::kMomentumSpreadGateBps;
    const bool regime_ok         = (regime_.bid.mode != mm::SymMode::Toxic)
                                   && (regime_.ask.mode != mm::SymMode::Toxic);

    const bool should_enter = momentum_selected_
                              && cooldown_ok && vpin_ok && no_pos
                              && signal_ok && spread_positive
                              && spread_wide_ok && spread_bps_ok
                              && regime_ok;

    if (!should_enter) {
        return false;   // обычный MM-flow продолжается
    }

    const double qty = api_.normalize_qty(symbol_, momentum_qty_usd_ / mid);
    if (qty <= 0.0) {
        return true;
    }

    cancelBidIfAny(nullptr);
    cancelAskIfAny(nullptr);

    std::string oid;
    double      px              = NAN;
    int8_t      sign            = 0;
    double      fill_px_actual  = 0.0;

    if (hawkes > 0.0) {
        px             = api_.normalize_price(symbol_, best_bid);
        fill_px_actual = px;
        sign           = +1;
        oid            = api_.place_limit_order(symbol_, "Buy", px, qty, 0, 0, 0);
    } else {
        px             = api_.normalize_price(symbol_, best_ask);
        fill_px_actual = px;
        sign           = -1;
        oid            = api_.place_limit_order(symbol_, "Sell", px, qty, 0, 0, 0);
    }

    if (!oid.empty()) {
        momentum_active_    = true;
        momentum_skew_      = hawkes;
        momentum_entry_ms_  = now;
        momentum_retry_     = 0;
        momentum_entry_px_  = fill_px_actual;
        momentum_entry_qty_ = qty;
        momentum_entry_sign_ = sign;                // ← BUG-FIX: было 0
        momentum_last_entry_attempt_ms_ = now;

        std::cout << "[MOMENTUM] " << symbol_
                  << (hawkes > 0.0 ? " BUY " : " SELL ")
                  << "skew=" << std::fixed << std::setprecision(2) << hawkes
                  << " qty=$" << std::setprecision(0) << momentum_qty_usd_
                  << " px="   << std::setprecision(4) << px
                  << std::defaultfloat << "\n";
    }
    return true;
}

// ============================================================
// Max-hold
// ============================================================
bool MarketMakingStrategy::handleMaxHold(uint64_t now, double mid,
                                         double best_bid, double best_ask)
{
    const double abs_inv = std::abs(inventory_usd_);
    if (abs_inv <= mm_cfg::kMinInventoryUsdForAction || position_opened_time_ == 0) {
        return false;
    }

    const uint64_t age_ms = (now > position_opened_time_)
                            ? (now - position_opened_time_) : 0;

    uint64_t dynamic_max_hold = (max_hold_override_sec_ > 0)
                                ? (static_cast<uint64_t>(max_hold_override_sec_) * 1000ULL)
                                : std::min(mm_cfg::kBaseMaxHoldMs, adaptive_params_.max_hold_ms);

    if (regime_.bid.mode == mm::SymMode::Toxic
        || regime_.ask.mode == mm::SymMode::Toxic) {
        dynamic_max_hold = std::min<uint64_t>(dynamic_max_hold, mm_cfg::kToxicMaxHoldMs);
    }

    if (age_ms <= dynamic_max_hold) {
        return false;
    }

    // dust
    const double pos_notional = std::abs(inventory_qty_) * mid;
    if (pos_notional < mm_cfg::kDustNotionalUsd) {
        inventory_qty_        = 0.0;
        inventory_usd_        = 0.0;
        position_opened_time_ = 0;
        return true;
    }

    if (std::abs(inventory_qty_) <= mm_cfg::kPosEpsilon) {
        return true;
    }

    const std::string close_side = (inventory_qty_ > 0) ? "Sell" : "Buy";
    const double      close_px   = (close_side == "Sell") ? best_ask : best_bid;
    const double      close_qty  = std::abs(inventory_qty_);

    const std::string oid = api_.place_limit_order(
            symbol_, close_side, close_px, close_qty, 0, 0, 0);

    if (!oid.empty()) {
        pending_close_oid_              = oid;
        pending_close_ms_               = now;
        pending_close_first_attempt_ms_ = now;
        close_blocked_until_            = now + mm_cfg::kPendingCloseTimeoutMs;
        std::cout << "[MM] " << symbol_ << " CLOSE MAKER [MAX_HOLD] ...\n";
    } else {
        close_position_market(best_bid, best_ask, "MAX_HOLD_FALLBACK");
        close_blocked_until_ = now + mm_cfg::kPendingCloseRetryBlockMs;
    }
    return true;
}

MarketMakingStrategy::SideGates
MarketMakingStrategy::evaluateGates(const GateInputs& in)
{
    using namespace mm_cfg;
    SideGates g;

    auto block_bid = [&](const char* why) noexcept {
        g.allow_bid = false;
        g.bid_block_reason = why;
    };
    auto block_ask = [&](const char* why) noexcept {
        g.allow_ask = false;
        g.ask_block_reason = why;
    };

    // 1. Цена пересекает противоположный BBO / за пределом block_ratio
    if (in.bid_price >= in.best_ask)        { block_bid("CROSS_ASK");    }
    if (in.ask_price <= in.best_bid)        { block_ask("CROSS_BID");    }
    if (in.q_ratio  >= in.bid_block_ratio)  { block_bid("BLOCK_RATIO");  }
    if (in.q_ratio  <= -in.ask_block_ratio) { block_ask("BLOCK_RATIO");  }

    // 2. Toxic-режим без достаточного числа fill'ов
    if (regime_.bid.mode == mm::SymMode::Toxic
        && regime_.bid.n_markouts_4h >= kToxicMinMarkouts4h
        && regime_.bid.hit_pct_1s    <  kToxicMinHitPct1s) {
        block_bid("TOXIC_NO_FILLS");
    }
    if (regime_.ask.mode == mm::SymMode::Toxic
        && regime_.ask.n_markouts_4h >= kToxicMinMarkouts4h
        && regime_.ask.hit_pct_1s    <  kToxicMinHitPct1s) {
        block_ask("TOXIC_NO_FILLS");
    }

    // 3. OBI-EMA
    if (obi_ema_ < -kObiBlockThreshold) { block_bid("OBI_EMA_NEG"); }
    if (obi_ema_ >  kObiBlockThreshold) { block_ask("OBI_EMA_POS"); }

    // 4. Position risk
    if (in.q_ratio >  kPositionRiskBlockRatio) { block_bid("POSITION_RISK"); }
    if (in.q_ratio < -kPositionRiskBlockRatio) { block_ask("POSITION_RISK"); }

    // 5. Cooldown
    if (in.now < bid_cooldown_until_) { block_bid("COOLDOWN"); }
    if (in.now < ask_cooldown_until_) { block_ask("COOLDOWN"); }

    // 6. Drift
    if (in.drift_bps < -in.drift_thr_bps) { block_bid("DRIFT_DOWN"); }
    if (in.drift_bps >  in.drift_thr_bps) { block_ask("DRIFT_UP");   }

    return g;
}

// ============================================================
// Trend-cut
// ============================================================
bool MarketMakingStrategy::handleTrendCut(uint64_t now, double mid,
                                          double best_bid, double best_ask)
{
    double trend_thr_bps;
    if (trend_thr_override_bps_ > 0.0) {
        trend_thr_bps = trend_thr_override_bps_;
    } else {
        const double base_bps = base_spread_pct_
                                * mm_cfg::kTrendThresholdSpreadFactor
                                * mm_cfg::kPctToBps;
        trend_thr_bps = std::max(base_bps, mm_cfg::kMinTrendThresholdBps);
    }

    const double drift_bps = computeDrift60sBps(now, mid);
    if (std::abs(drift_bps) <= trend_thr_bps) {
        return false;
    }

    cancelAllOrders(nullptr);

    if (std::abs(inventory_usd_) > mm_cfg::kActionableInventoryUsd) {
        close_position_market(best_bid, best_ask, "TREND");
        return true;
    }
    last_trend_cut_ms_ = now;
    return true;
}


// ============================================================
// Calculations
// ============================================================
auto MarketMakingStrategy::computeMomentumSkew(uint64_t now, double mid) const -> double
{
    if (momentum_weight_ <= 0.01
        || mid_history_.size() < mm_cfg::kMinHistoryForMomentum) {
        return 0.0;
    }

    const uint64_t t_ago = (now > mm_cfg::kMomentumDriftWindowMs)
                           ? (now - mm_cfg::kMomentumDriftWindowMs) : 0;

    double mid_ago = mid;
    for (auto it = mid_history_.rbegin(); it != mid_history_.rend(); ++it) {
        if (it->first <= t_ago) {
            mid_ago = it->second;
            break;
        }
    }
    if (mid_ago <= 0.0) {
        return 0.0;
    }

    const double drift = std::clamp((mid - mid_ago) / mid_ago,
                                    -mm_cfg::kMomentumMaxDriftPct,
                                    mm_cfg::kMomentumMaxDriftPct);
    return drift * mid * momentum_weight_;
}

double MarketMakingStrategy::computeTimeUrgency(uint64_t now) const
{
    if (position_opened_time_ == 0) {
        return 0.0;
    }
    const double abs_inv = std::abs(inventory_usd_);
    if (abs_inv <= mm_cfg::kMinInventoryUsdForAction
        || abs_inv >= mm_cfg::kUrgencyMaxUsd) {
        return 0.0;
    }

    const uint64_t age_ms = (now > position_opened_time_)
                            ? (now - position_opened_time_) : 0;
    const double age_ratio = std::min(
            1.0, static_cast<double>(age_ms) / static_cast<double>(mm_cfg::kUrgencyAgeCapMs));
    return age_ratio * age_ratio;
}

double MarketMakingStrategy::computeDrift60sBps(uint64_t now, double mid) const
{
    const double mid_ago = findMidAgo(now, mm_cfg::kDriftWindowMs, mid);
    if (mid_ago <= 0.0) {
        return 0.0;
    }
    return (mid - mid_ago) / mid_ago * mm_cfg::kPctToBps;
}

double MarketMakingStrategy::findMidAgo(uint64_t now,
                                        uint64_t window_ms,
                                        double fallback) const
{
    if (mid_history_.empty()) {
        return fallback;
    }
    const uint64_t t_ago = (now > window_ms) ? (now - window_ms) : 0;

    for (auto it = mid_history_.rbegin(); it != mid_history_.rend(); ++it) {
        if (it->first <= t_ago) {
            return it->second;
        }
    }
    return mid_history_.front().second;   // самый старый из имеющихся
}

void MarketMakingStrategy::updateSigmaEma(uint64_t now, double sigma)
{
    if (last_sigma_ema_ms_ != 0
        && now - last_sigma_ema_ms_ <= mm_cfg::kSigmaEmaIntervalMs) {
        return;
    }
    const double alpha = mm_cfg::kSigmaEmaAlpha;
    avg_sigma_1m_ = (avg_sigma_1m_ <= 0.0)
                    ? sigma
                    : avg_sigma_1m_ * (1.0 - alpha) + sigma * alpha;
    last_sigma_ema_ms_ = now;
}

void MarketMakingStrategy::pushMidHistory(uint64_t now, double mid)
{
    mid_history_.push_back({now, mid});
    while (!mid_history_.empty()
           && now - mid_history_.front().first > mm_cfg::kMidHistoryWindowMs) {
        mid_history_.pop_front();
    }
}

bool MarketMakingStrategy::handleToxicFillCheck(uint64_t now, double mid,
                                                double best_bid, double best_ask)
{
    using namespace mm_cfg;

    if (!toxic_check_pending_) {
        return false;
    }
    toxic_check_pending_ = false;   // one-shot

    const double mk_bps = (toxic_check_side_ == "Buy")
                          ? (mid - toxic_mid_at_fill_) / toxic_mid_at_fill_ * kPctToBps
                          : (toxic_mid_at_fill_ - mid) / toxic_mid_at_fill_ * kPctToBps;

    if (mk_bps >= kToxicThresholdBps) {
        return false;
    }

    adverse_fill_count_++;

    std::cerr << "[MM-TOXIC] " << symbol_
              << " side=" << toxic_check_side_
              << " mk_100ms=" << std::fixed << std::setprecision(2)
              << mk_bps << "bps"
              << " count=" << adverse_fill_count_ << "\n" << std::defaultfloat;

    if (std::abs(inventory_qty_) > kPosEpsilon) {
        close_position_market(best_bid, best_ask, "TOXIC_100MS");
    }

    close_blocked_until_ = now + kToxicCloseBlockMs;

    if (adverse_fill_count_ >= 2) {
        close_blocked_until_ = now + kBlacklistBlockMs;
        std::cerr << "[MM-BLACKLIST] " << symbol_
                  << " disabled 1h (adverse_fill_count="
                  << adverse_fill_count_ << ")\n";
    }
    return true;
}

bool MarketMakingStrategy::handleVpinPause(uint64_t now)
{
    using namespace mm_cfg;

    // Активная пауза — просто ждём
    if (vpin_pause_until_ > now) {
        cancelAllOrders("TOXIC_PAUSE");
        return true;
    }

    // Просроченная метка — снимаем
    if (vpin_pause_until_ > 0) {
        vpin_pause_until_ = 0;
    }

    // Пауза выключена
    if (vpin_pause_ > 1.0) {
        return false;
    }

    // Новая пауза
    if (cluster_.get_vpin(kVpinWindow) > vpin_pause_) {
        vpin_pause_until_ = now + kVpinPauseDurationMs;
        cancelAllOrders("");
        return true;
    }
    return false;
}

auto MarketMakingStrategy::computeSignals(uint64_t now, double mid) -> Signals
{
    using namespace mm_cfg;
    Signals s;

    s.q_ratio = std::clamp(inventory_usd_ / max_position_usd_, -1.0, 1.0);
    s.obi     = orderbook_.get_imbalance_at_levels(kObiLevels);

    const double ofi_raw  = orderbook_.get_ofi_sum(kOfiSumLevels);
    const double bbo_size = std::max(orderbook_.get_best_bid_size(),
                                     orderbook_.get_best_ask_size());
    if (bbo_size > kPosEpsilon) {
        s.ofi_norm = ofi_raw / bbo_size;
    }
    s.ofi_norm = std::clamp(s.ofi_norm, -kOfiNormClampAbs, kOfiNormClampAbs);

    s.momentum_skew = computeMomentumSkew(now, mid);
    return s;
}

auto MarketMakingStrategy::computeVolatility(uint64_t now) -> VolState
{
    using namespace mm_cfg;
    VolState v;

    double sigma = 0.0;
    if (!vol_estimator_.get_sigma_per_sqrt_sec(sigma) || sigma < min_sigma_) {
        sigma = last_sigma_;
    } else {
        last_sigma_ = sigma;
        sigma = std::clamp(sigma, min_sigma_, max_sigma_);
    }
    v.sigma = sigma;

    updateSigmaEma(now, sigma);
    v.vol_ratio = (avg_sigma_1m_ > 0.0) ? (sigma / avg_sigma_1m_) : 1.0;

    if (v.vol_ratio > vol_ratio_max_) {
        v.cancel_all = true;
        return v;
    }

    if (v.vol_ratio > kStressVolRatioThreshold) {
        v.stress_mult_spread = 1.0 + (v.vol_ratio - 1.0) * kStressSpreadMultBase;
        v.stress_mult_qty    = 1.0 / v.stress_mult_spread;
    }
    return v;
}

auto MarketMakingStrategy::computeSpreadParams(
        double mid, double best_bid, double best_ask,
        double sigma, const VolState& vol) -> SpreadState
{
    using namespace mm_cfg;
    SpreadState sp;

    const double eff_spread_pct = use_adaptive_ ? adaptive_params_.base_spread_pct
                                                : base_spread_pct_;
    sp.eff_qty_usd = (use_adaptive_ ? adaptive_params_.order_qty_usd
                                    : order_qty_usd_) * vol.stress_mult_qty;

    const double vpin = cluster_.get_vpin(kVpinWindow);
    const double vpin_mult = (vpin > kVpinMultThreshold)
                             ? (1.0 + (vpin - kVpinMultThreshold) * kVpinMultSlope)
                             : 1.0;

    sp.sigma_ratio = std::clamp(sigma / sigma_ref_, kSigmaRatioClampLo, kSigmaRatioClampHi);

    double effective_pct = eff_spread_pct * std::sqrt(sp.sigma_ratio)
                           * vol.stress_mult_spread * vpin_mult;

    const double actual_bps = (best_ask - best_bid) / mid * kPctToBps;
    double min_bps = kMinSpreadUltraTightBps;
    for (const auto& c : kSpreadTable) {
        if (actual_bps > c.threshold_bps) {
            min_bps = c.min_spread_bps;
            break;
        }
    }
    min_bps = std::max(min_bps,
                       kMinSpreadFeeMultiplier * maker_fee_bps_ + kMinSpreadFeePremiumBps);

    effective_pct = std::max(effective_pct, min_bps / kPctToBps);
    effective_pct = std::clamp(effective_pct,
                               min_half_spread_pct_ * kTwoTicksFactor,
                               max_half_spread_pct_ * kTwoTicksFactor);

    sp.half_spread = mid * effective_pct / kTwoTicksFactor;
    sp.half_spread = std::clamp(sp.half_spread,
                                mid * min_half_spread_pct_,
                                mid * max_half_spread_pct_);
    return sp;
}


auto MarketMakingStrategy::computeSideHalves(
        double half_spread, double q_ratio, double hma_skew_bps,
        const RegimeTuning& bid_tuning,
        const RegimeTuning& ask_tuning) const -> SideHalves
{
    using namespace mm_cfg;
    SideHalves h;

    h.bid = half_spread * bid_tuning.half_spread_mult;
    h.ask = half_spread * ask_tuning.half_spread_mult;

    if (q_ratio > kSideBiasActivationRatio) {
        h.ask = half_spread * ask_tuning.half_spread_mult
                * (1.0 + q_ratio * kSideBiasHalfSpreadSlope);
    } else if (q_ratio < -kSideBiasActivationRatio) {
        h.bid = half_spread * bid_tuning.half_spread_mult
                * (1.0 + (-q_ratio) * kSideBiasHalfSpreadSlope);
    }

    if (std::abs(hma_skew_bps) > kHmaSkewThresholdBps) {
        const double f = std::clamp(hma_skew_bps / kHmaSkewScaleBps,
                                    -kHmaSkewMaxFactor, kHmaSkewMaxFactor);
        h.bid *= (1.0 - f);
        h.ask *= (1.0 + f);
    }
    return h;
}


auto MarketMakingStrategy::computeCrossSkew(uint64_t now, double mid) -> CrossSkew
{
    using namespace mm_cfg;
    CrossSkew s;

    if (signal_engine_ != nullptr) {
        const auto sig = signal_engine_->get_signal(symbol_);
        cross_skew_bps_ = std::clamp(sig.target_skew_bps, -kMaxCrossBps, kMaxCrossBps);
        s.skew = cross_skew_bps_ / kPctToBps * mid;
    }
    if (tp_engine_) {
        s.hawkes = tp_engine_->target_skew_bps(now);
    }
    s.shadow = s.hawkes;
    return s;
}

double MarketMakingStrategy::clampQtyToMinNotional(double q, double mid,
                                                   mm::SymMode mode) const
{
    using namespace mm_cfg;
    if (q <= 0.0)                    { return 0.0; }
    if (q * mid >= kMinNotionalUsd)  { return q; }
    if (mode == mm::SymMode::Toxic)  { return 0.0; }
    return api_.normalize_qty(symbol_, kMinNotionalUsd / mid);
}

auto MarketMakingStrategy::computeQuantities(
        double mid, double q_ratio, double eff_qty_usd,
        const RegimeTuning& bid_tuning,
        const RegimeTuning& ask_tuning) -> Qtys
{
    using namespace mm_cfg;
    Qtys q;

    const double qty_factor       = 1.0 - (std::abs(q_ratio) * kQtyInventoryPenalty);
    const double adaptive_qty_usd = eff_qty_usd * qty_factor;

    q.bid = clampQtyToMinNotional(
            api_.normalize_qty(symbol_, adaptive_qty_usd * bid_tuning.qty_mult / mid),
            mid, regime_.bid.mode);
    q.ask = clampQtyToMinNotional(
            api_.normalize_qty(symbol_, adaptive_qty_usd * ask_tuning.qty_mult / mid),
            mid, regime_.ask.mode);

    q.allow_bid = (q.bid > 0.0);
    q.allow_ask = (q.ask > 0.0);
    return q;
}

auto MarketMakingStrategy::computeReplaceFlags(
        uint64_t now, double bid_price, double ask_price,
        double bid_half, double ask_half, double tick_size) const -> ReplaceFlags
{
    using namespace mm_cfg;
    ReplaceFlags r;
    r.bid_price = bid_price;
    r.ask_price = ask_price;

    const double bid_thr = (regime_.bid.mode == mm::SymMode::Tier1)
                           ? kMoveThrTier1 : kMoveThrDefault;
    const double ask_thr = (regime_.ask.mode == mm::SymMode::Tier1)
                           ? kMoveThrTier1 : kMoveThrDefault;

    r.bid = (last_bid_p_ == 0.0)
            || (std::abs(last_bid_p_ - bid_price) / bid_half > bid_thr);
    if (now - last_bid_place_ms_ < kMinReplaceIntervalMs && !bid_order_id_.empty()) {
        r.bid = false;
    }

    r.ask = (last_ask_p_ == 0.0)
            || (std::abs(last_ask_p_ - ask_price) / ask_half > ask_thr);
    if (now - last_ask_place_ms_ < kMinReplaceIntervalMs && !ask_order_id_.empty()) {
        r.ask = false;
    }

    // Снап: не replace, но цена рядом с last — фиксируем last, чтобы не дёргать ордер
    if (!r.bid && !bid_order_id_.empty()
        && std::abs(last_bid_p_ - bid_price) < tick_size) {
        r.bid_price = last_bid_p_;
    }
    if (!r.ask && !ask_order_id_.empty()
        && std::abs(last_ask_p_ - ask_price) < tick_size) {
        r.ask_price = last_ask_p_;
    }
    return r;
}

auto MarketMakingStrategy::buildPlaceContext(
        double mid, double best_bid, double best_ask,
        double half_spread, double skew, double sigma,
        double q_ratio, double hma_skew_bps,
        double tp_shadow_bps, uint64_t now) const -> PlaceContext
{
    PlaceContext ctx{
            .mid = mid, .best_bid = best_bid, .best_ask = best_ask,
            .half_spread = half_spread, .skew = skew, .sigma = sigma,
            .q_ratio = q_ratio, .inv_usd = inventory_usd_
    };
    if (signal_engine_ != nullptr) {
        const auto sig = signal_engine_->get_signal(symbol_);
        ctx.cross_bps = tp_shadow_bps;
        ctx.ret_500   = sig.ret_500ms_bybit;
        ctx.flow_b    = sig.bybit_flow_imbalance;
        ctx.flow_bnx  = sig.bitunix_flow_imbalance;
        ctx.ofi       = sig.ofi_bybit;
        ctx.delta     = sig.delta_bps;
    }
    ctx.hma_skew_bps = hma_skew_bps;
    ctx.hma_fast_val = hma_fast_.value();
    ctx.hma_slow_val = hma_slow_.value();
    ctx.kernel_val   = kernel_.value(now);
    return ctx;
}

bool MarketMakingStrategy::handleEarlyExits(uint64_t now) const
{
    using namespace mm_cfg;

    if (shutting_down_)                                                            { return true; }
    if (now < close_blocked_until_)                                                { return true; }
    if (last_trend_cut_ms_ > 0 && now - last_trend_cut_ms_ < kTrendCutCooldownMs)  { return true; }
    return false;
}

std::optional<MarketMakingStrategy::MarketSnapshot>
MarketMakingStrategy::readMarketSnapshot(uint64_t now) const
{
    using namespace mm_cfg;

    const double mid = orderbook_.get_mid_price();
    if (mid <= 0.0) {
        return std::nullopt;
    }

    const double best_bid = orderbook_.get_best_bid();
    const double best_ask = orderbook_.get_best_ask();
    if (best_bid <= 0.0 || best_ask <= 0.0) {
        return std::nullopt;
    }

    if (now - orderbook_.get_last_update_ms() > kBookStaleMs) {
        return std::nullopt;
    }

    double micro = orderbook_.get_microprice();
    if (micro <= 0.0) {
        micro = mid;
    }
    return MarketSnapshot{mid, best_bid, best_ask, micro};
}

bool MarketMakingStrategy::refreshPositionIfDue(uint64_t now)
{
    using namespace mm_cfg;

    if (last_pos_refresh_ms_ != 0 && now - last_pos_refresh_ms_ <= kPositionRefreshMs) {
        return true;   // нечего делать — продолжаем
    }
    const bool ok = refresh_position();
    last_pos_refresh_ms_ = now;
    return ok;
}

double MarketMakingStrategy::getTickSize(double mid) const
{
    using namespace mm_cfg;

    double tick_size = api_.get_instrument_info(symbol_).tick_size;
    if (tick_size > 0.0) {
        return tick_size;
    }

    const double probe = api_.normalize_price(symbol_, mid * kTickProbeMult);
    tick_size = probe - mid;
    if (tick_size > 0.0) {
        return tick_size;
    }
    return mid * kTickFallbackFraction;
}

bool MarketMakingStrategy::handleRegimeFilter()
{
    if (regime_filter_ == nullptr
        || !regime_filter_->is_initialized()
        || regime_filter_->is_mr_mode()) {
        return false;
    }
    cancelAllOrders("");
    return true;
}

bool MarketMakingStrategy::handleKillSwitch(double best_bid, double best_ask)
{
    using namespace mm_cfg;

    if (adaptive_params_.enabled) {
        return false;
    }
    cancelAllOrders(nullptr);
    if (std::abs(inventory_usd_) > kMinInventoryUsdForAction) {
        close_position_market(best_bid, best_ask, "KILL");
    }
    return true;
}

bool MarketMakingStrategy::handleRiskExits(uint64_t now, double q_ratio,
                                           double best_bid, double best_ask)
{
    using namespace mm_cfg;

    if (std::abs(q_ratio) > kSoftExitQtyRatio
        && last_unrealized_pnl_pct_ < kSoftExitPnlPct) {
        close_position_market(best_bid, best_ask, "SOFT_EXIT");
        return true;
    }

    if (std::abs(inventory_usd_) <= kMaxHoldTriggerUsd || position_opened_time_ == 0) {
        return false;
    }
    const uint64_t age_ms = now - position_opened_time_;
    const double   pnl_bps = last_unrealized_pnl_pct_ * kPctToBps;
    if (age_ms < kFastAdverseMinAgeMs || age_ms >= kFastAdverseMaxAgeMs) {
        return false;
    }
    if (pnl_bps >= kFastAdversePnlBps) {
        return false;
    }
    close_position_market(best_bid, best_ask, "FAST_ADVERSE");
    return true;
}

double MarketMakingStrategy::computeInventorySkew(
        uint64_t now, double half_spread, double sigma_ratio, double q_ratio) const
{
    using namespace mm_cfg;

    const double q_sq        = q_ratio * q_ratio;
    const double inv_penalty = q_ratio / std::max(kPosEpsilon,
                                                  1.0 - kInvPenaltyQuadFactor * q_sq);
    const double urgency     = computeTimeUrgency(now);
    const double skew_raw    = -inv_penalty * half_spread * gamma_ * sigma_ratio
                               * (1.0 + kSkewUrgencyMult * urgency);
    return std::clamp(skew_raw, -half_spread, half_spread);
}

void MarketMakingStrategy::updateObiEma(uint64_t now, double obi)
{
    using namespace mm_cfg;

    if (obi_ema_last_ms_ != 0 && now - obi_ema_last_ms_ <= kObiEmaIntervalMs) {
        return;
    }
    obi_ema_         = obi_ema_ * kObiEmaKeep + obi * kObiEmaAlpha;
    obi_ema_last_ms_ = now;
}

bool MarketMakingStrategy::handleHardCap(double best_bid, double best_ask) const
{
    if (std::abs(inventory_usd_) < max_position_usd_ * hard_cap_ratio_) {
        return false;
    }
    const_cast<MarketMakingStrategy*>(this)
            ->close_position_market(best_bid, best_ask, "HARD_CAP");
    return true;
}

auto MarketMakingStrategy::computeBlockRatios() const noexcept -> BlockRatios
{
    auto for_side = [this](mm::SymMode m) noexcept {
        return (m == mm::SymMode::Toxic) ? mm_cfg::kToxicBlockRatio : block_ratio_;
    };
    return {for_side(regime_.bid.mode), for_side(regime_.ask.mode)};
}

void MarketMakingStrategy::update_pending_markouts(uint64_t now) {
    if (pending_markouts_.empty()) return;

    static constexpr std::array<uint64_t, 7> DELAYS = {100, 500, 1000, 5000, 30000, 60000, 120000};

    double cur_mid = orderbook_.get_mid_price();
    if (cur_mid <= 0.0) return;

    std::vector<std::string> to_remove;
    to_remove.reserve(pending_markouts_.size());

    static constexpr uint64_t MAX_MARKOUT_ELAPSED_MS = 130000;

    for (auto& [oid, m] : pending_markouts_) {
        if (m.finalized) { to_remove.push_back(oid); continue; }

        uint64_t elapsed = (now > m.fill_time_ms) ? (now - m.fill_time_ms) : 0;

        // Если сильно проспали (например, бот был заблокирован на 35+ сек),
        // НЕ записываем cur_mid во все незаполненные слоты — это ложь.
        // Финализируем с тем, что успели записать.
        if (elapsed > MAX_MARKOUT_ELAPSED_MS) {
            m.finalized = true;
            persist_markout(m);
            to_remove.push_back(oid);
            continue;
        }

        // Иначе — записываем слоты, которые наступили
        for (size_t i = 0; i < DELAYS.size(); ++i) {
            if (!m.recorded[i] && elapsed >= DELAYS[i]) {
                // Проверка на "прыжок через несколько delays" за один tick:
                // если у нас tick раз в 100ms, это невозможно. Если tick 1 раз в 5 сек — возможно.
                // В таком случае записываем cur_mid только в последний достигнутый слот.
                m.mids[i] = cur_mid;
                m.recorded[i] = true;
            }
        }

        bool all_done = true;
        for (bool r : m.recorded) if (!r) { all_done = false; break; }

        if (all_done) {
            m.finalized = true;
            persist_markout(m);

            // Для backtest: сохраняем в памяти (live идёт в БД)
            bool has_db = db_conn_ && db_conn_->is_open();
            if (!has_db) {
                FinalizedMarkout fm;
                fm.order_id           = m.order_id;
                fm.side               = m.side;
                fm.qty                = m.qty;
                fm.fill_price         = m.fill_price;
                fm.fill_time_ms       = m.fill_time_ms;
                fm.fair_price_at_fill = m.fair_price_at_fill;
                fm.mid_100ms          = m.mids[0];
                fm.mid_500ms          = m.mids[1];
                fm.mid_1s             = m.mids[2];
                fm.mid_5s             = m.mids[3];
                fm.mid_30s            = m.mids[4];

                auto sign = (m.side == "Buy") ? 1.0 : -1.0;
                auto mk_bps = [&](double mk_mid) -> double {
                    if (mk_mid <= 0.0 || m.fill_price <= 0.0) return 0.0;
                    return sign * (mk_mid - m.fill_price) / m.fill_price * 10000.0;
                };
                fm.mk_100ms_bps = mk_bps(fm.mid_100ms);
                fm.mk_500ms_bps = mk_bps(fm.mid_500ms);
                fm.mk_1s_bps    = mk_bps(fm.mid_1s);
                fm.mk_5s_bps    = mk_bps(fm.mid_5s);
                fm.mk_30s_bps   = mk_bps(fm.mid_30s);

                // Cap памяти (защита от unbounded)
                if (finalized_markouts_.size() >= 100'000) {
                    finalized_markouts_.erase(finalized_markouts_.begin(),
                                              finalized_markouts_.begin() + 50'000);
                }
                finalized_markouts_.push_back(std::move(fm));
            }

            to_remove.push_back(oid);
        }
    }
    for (const auto& oid : to_remove) pending_markouts_.erase(oid);
}

void MarketMakingStrategy::persist_markout(const PendingMarkout& m) {
    if (!db_conn_ || !db_conn_->is_open()) return;

    double sign = (m.side == "Buy") ? 1.0 : -1.0;
    auto mk_bps = [&](double mk_mid) -> std::optional<double> {
        if (mk_mid <= 0.0 || m.fill_price <= 0.0) return std::nullopt;
        return sign * (mk_mid - m.fill_price) / m.fill_price * 10000.0;
    };

    try {
        pqxx::work txn(*db_conn_);
        txn.exec(
                "INSERT INTO mm_markouts "
                "(order_id, symbol, side, qty, fill_price, fill_time_ms, run_tag, "
                " fair_price_at_fill, mid_100ms, mid_500ms, mid_1s, mid_5s, mid_30s, "
                " mk_100ms_bps, mk_500ms_bps, mk_1s_bps, mk_5s_bps, mk_30s_bps, "
                " finalized) "
                "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17,$18,true) "
                "ON CONFLICT (order_id) DO NOTHING",
                pqxx::params{
                        m.order_id, symbol_, m.side, m.qty, m.fill_price,
                        m.fill_time_ms, run_tag_,
                        m.fair_price_at_fill,
                        m.mids[0], m.mids[1], m.mids[2], m.mids[3], m.mids[4],
                        mk_bps(m.mids[0]), mk_bps(m.mids[1]), mk_bps(m.mids[2]),
                        mk_bps(m.mids[3]), mk_bps(m.mids[4])
                });
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[MM-DB] persist markout error: " << e.what() << "\n";
    }
}

double MarketMakingStrategy::compute_hma_skew_bps() const {
    if (!hma_fast_.ready() || !hma_slow_.ready()) return 0.0;

    double mid = orderbook_.get_mid_price();
    if (mid <= 0.0) return 0.0;

    double hma_fast_val = hma_fast_.value();
    double hma_slow_val = hma_slow_.value();
    if (hma_fast_val <= 0.0 || hma_slow_val <= 0.0) return 0.0;

    // HMA trend: (fast - slow) / mid в bps
    double hma_trend_bps = (hma_fast_val - hma_slow_val) / mid * 10000.0;

    // Kernel filter (если готов)
    double kernel_bps = 0.0;
    if (kernel_.ready()) {
        double k = kernel_.value(this->now());
        if (k > 0.0) {
            kernel_bps = (k - mid) / mid * 10000.0;
        }
    }

    // Комбинация: HMA momentum + Kernel trend
    double skew_bps = 0.5 * hma_trend_bps + 0.5 * kernel_bps;

    // Ограничиваем как signal_engine (max 8 bps)
    return std::clamp(skew_bps, -4.0, 4.0);
}

std::vector<MarketMakingStrategy::FinalizedMarkout>
MarketMakingStrategy::drain_finalized_markouts() {
    std::vector<FinalizedMarkout> out;
    out.swap(finalized_markouts_);
    return out;
}

void MarketMakingStrategy::cancel_ask_on_through(double trade_price, const std::string& reason) {
    if (ask_order_id_.empty()) return;
    if (last_ask_p_ <= 0.0) return;
    if (trade_price < last_ask_p_ - 1e-9) return;
    cancel_ask_now(reason);
}

void MarketMakingStrategy::cancel_bid_on_through(double trade_price, const std::string& reason) {
    if (bid_order_id_.empty()) return;
    if (last_bid_p_ <= 0.0) return;
    if (trade_price > last_bid_p_ + 1e-9) return;
    cancel_bid_now(reason);
}

void MarketMakingStrategy::cancel_ask_now(const std::string& reason) {
    if (ask_order_id_.empty()) return;
    api_.cancel_order(symbol_, ask_order_id_);
    log_cancel(ask_order_id_, reason);
    ask_order_id_.clear();
    last_ask_p_ = 0.0;
    ask_cooldown_until_ = this->now() + 5'000;
}

void MarketMakingStrategy::cancel_bid_now(const std::string& reason) {
    if (bid_order_id_.empty()) return;
    api_.cancel_order(symbol_, bid_order_id_);
    log_cancel(bid_order_id_, reason);
    bid_order_id_.clear();
    last_bid_p_ = 0.0;
    bid_cooldown_until_ = this->now() + 5'000;
}

// ============================================================
// Momentum signal — ранжирование по expected value
// ============================================================
MarketMakingStrategy::MomentumSignal MarketMakingStrategy::compute_momentum_signal(uint64_t now) const {
    MomentumSignal sig{};
    sig.symbol = symbol_;
    sig.venue  = is_bitunix_symbol_ ? "bitunix" : "bybit";
    sig.is_valid = false;

    if (!tp_engine_ || !momentum_enabled_) return sig;

    double hawkes = tp_engine_->target_skew_bps(now);
    if (std::abs(hawkes) < 1.0) return sig;

    double best_bid = orderbook_.get_best_bid();
    double best_ask = orderbook_.get_best_ask();
    if (best_bid <= 0 || best_ask <= 0) return sig;

    auto info = api_.get_instrument_info(symbol_);
    double tick = info.tick_size;
    if (tick <= 0) return sig;

    double spread_ticks = (best_ask - best_bid) / tick;
    if (spread_ticks <= 0.0) {        // bb==ba или crossed book
        sig.is_valid = false;
        return sig;
    }

    // fill_prob
    double fill_prob;
    if      (spread_ticks < 1.5) fill_prob = 0.20;
    else if (spread_ticks < 2.5) fill_prob = 0.60;
    else                         fill_prob = 0.85;

    // duration_factor
    double decay = tp_engine_->decay_for_venue(0);
    double duration_factor = std::exp(-decay * 0.5);

    // adverse — ОСЛАБЛЕНО: 3.0 → 1.0, чтобы не блокировать все
    double adverse_bps = 0.0;
    const auto& reg_side = (hawkes > 0) ? regime_.bid : regime_.ask;
    if (reg_side.n_markouts_4h >= 20) {
        double mk = reg_side.med_mk_30s;
        if (mk < -5.0) adverse_bps = 1.0;   // было 3.0 при mk<-2
        else if (mk < -2.0) adverse_bps = 0.5;
    }

    // fee — для теста 2.0 (maker entry), close пока taker=0
    double fee_bps = 2.0;

    sig.hawkes_bps      = hawkes;
    sig.spread_ticks    = spread_ticks;
    sig.fill_prob       = fill_prob;
    sig.duration_factor = duration_factor;
    sig.adverse_bps     = adverse_bps;
    sig.side            = (hawkes > 0) ? +1 : -1;

    sig.score_bps = std::abs(hawkes) * fill_prob * duration_factor
                    - fee_bps
                    - adverse_bps;
    double mid = orderbook_.get_mid_price();
    // ═══ maker+maker требует >= 3 tick spread ═══
    // spread_bps блокирует только если совсем tight + низкий hawkes
    double spread_bps = spread_ticks * (tick / mid) * 10000.0;
    if (spread_bps < 35.0) {          // ← без && hawkes
        sig.is_valid = false;
        return sig;
    }
    sig.is_valid = true;

    return sig;
}

void MarketMakingStrategy::shutdown_gracefully(const std::string& reason) {
    if (shutting_down_) return;
    shutting_down_ = true;

    std::cout << "[MM] " << symbol_ << " shutdown_gracefully: " << reason << "\n";

    // 1) Отменить открытые bid/ask
    if (!bid_order_id_.empty()) {
        api_.cancel_order(symbol_, bid_order_id_);
        log_cancel(bid_order_id_, reason);
        bid_order_id_.clear();
        last_bid_p_ = 0.0;
    }
    if (!ask_order_id_.empty()) {
        api_.cancel_order(symbol_, ask_order_id_);
        log_cancel(ask_order_id_, reason);
        ask_order_id_.clear();
        last_ask_p_ = 0.0;
    }

    // 2) Если позиция значимая — закрыть market-ом, чтобы не висела orphan
    constexpr double kMinCloseUsd = 1.0;
    if (std::abs(inventory_usd_) > kMinCloseUsd) {
        double best_bid = orderbook_.get_best_bid();
        double best_ask = orderbook_.get_best_ask();
        if (best_bid > 0.0 && best_ask > 0.0) {
            close_position_market(best_bid, best_ask, reason);
            std::cout << "[MM] " << symbol_
                      << " closed position $" << inventory_usd_
                      << " on shutdown\n";
        } else {
            std::cerr << "[MM] " << symbol_
                      << " cannot close (no book), orphan pos=$" << inventory_usd_ << "\n";
        }
    } else if (std::abs(inventory_usd_) > 1e-9) {
        std::cout << "[MM] " << symbol_
                  << " position $" << inventory_usd_
                  << " below min ($" << kMinCloseUsd << "), left as orphan\n";
    }
}