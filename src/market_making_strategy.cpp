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

// ============================================================
// Конструктор
// ============================================================
MarketMakingStrategy::MarketMakingStrategy(
        OrderBook& orderbook,
        ClusterEngine& cluster,
        IExchange& exchange,              // ← здесь
        std::string  symbol,
        double base_spread_pct,
        double order_qty_usd,
        double max_position_usd)
        : orderbook_(orderbook),
          cluster_(cluster),
          api_(exchange),                  // ← и здесь
          symbol_(std::move(symbol)),
          base_spread_pct_(base_spread_pct),
          order_qty_usd_(order_qty_usd),
          max_position_usd_(max_position_usd)
{
    adaptive_params_.base_spread_pct = base_spread_pct_;
    adaptive_params_.order_qty_usd   = order_qty_usd_;
    adaptive_params_.max_hold_ms     = 25'000;
    adaptive_params_.enabled         = true;
    base_gamma_ = gamma_;
}

// ============================================================
// refresh_position
// ============================================================
auto MarketMakingStrategy::refresh_position() -> bool {
    auto pos = api_.get_position_info(symbol_);

    if (pos.rate_limited) return false;

    if (!pos.is_valid) {
        consecutive_invalid_pos_++;
        if (consecutive_invalid_pos_ <= 3) {
            std::cerr << "[MM] " << symbol_
                      << " position fetch failed (streak="
                      << consecutive_invalid_pos_
                      << ") code=" << api_.last_ret_code() << "\n";
        }
        if (consecutive_invalid_pos_ < 3) return false;
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

    if (mid <= 0) mid = pos.avg_price;

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
            if (tick_size <= 0) tick_size = best_bid * 0.0001;
        }
    }

    double close_px = NAN;
    if (force_aggressive) {
        close_px = (close_side == "Sell") ? best_bid : best_ask;
    } else {
        // Passive, но подрезаем best — front of queue
        if (close_side == "Sell") {
            close_px = best_ask - tick_size;
        } else {
            close_px = best_bid + tick_size;
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
void MarketMakingStrategy::tick() {
    uint64_t now = this->now();

    if (shutting_down_) return;
    if (now < close_blocked_until_) return;
    if (last_trend_cut_ms_ > 0 && now - last_trend_cut_ms_ < 60'000) return;

    if (pending_cancel_ask_.exchange(false, std::memory_order_relaxed)) {
        cancel_ask_now("TOXIC_THROUGH");
    }
    if (pending_cancel_bid_.exchange(false, std::memory_order_relaxed)) {
        cancel_bid_now("TOXIC_THROUGH");
    }

    if (last_pos_refresh_ms_ == 0 || now - last_pos_refresh_ms_ > 20'000) {
        bool ok = refresh_position();
        last_pos_refresh_ms_ = now;
        if (!ok) return;
    }

    double mid = orderbook_.get_mid_price();
    if (mid <= 0) return;
    double best_bid = orderbook_.get_best_bid();
    double best_ask = orderbook_.get_best_ask();
    double micro = orderbook_.get_microprice();
    if (micro <= 0.0) micro = mid;
    if (best_bid <= 0 || best_ask <= 0) return;

    {
        uint64_t book_age = now - orderbook_.get_last_update_ms();
        if (book_age > 10'000) return;
    }

    double tick_size = 0.0;
    {
        auto info = api_.get_instrument_info(symbol_);
        tick_size = info.tick_size;
        if (tick_size <= 0) {
            double probe = api_.normalize_price(symbol_, mid * 1.001);
            tick_size = probe - mid;
            if (tick_size <= 0) tick_size = mid * 0.0001;
        }
    }

    // ═══ MOMENTUM MODE ═══
    if (tp_engine_ && momentum_enabled_) {
        double hawkes = tp_engine_->target_skew_bps(now);

        // ─── EXIT: активная позиция ───
        if (momentum_active_) {
            uint64_t age = (now > momentum_entry_ms_) ? (now - momentum_entry_ms_) : 0;
            double pnl_bps = last_unrealized_pnl_pct_ * 10000.0;
            bool has_pos = std::abs(inventory_usd_) > 1.0;

            // 1) Take profit / forced timeout — сбрасываем флаг СРАЗУ
            if (has_pos && (pnl_bps > momentum_tp_bps_ || age > momentum_max_ms_)) {
                close_position_market(best_bid, best_ask,
                                      (pnl_bps > momentum_tp_bps_) ? "MOMENTUM_TP" : "MOMENTUM_TIMEOUT");

                // ═══ КРИТИЧЕСКИЙ ФИКС: сброс флага + cooldown ═══
                momentum_active_ = false;
                momentum_retry_  = 0;
                momentum_last_entry_attempt_ms_ = now;
                return;
            }

            // 2) Позиции нет (entry не fill) — сбрасываем через 500ms
            if (!has_pos && age > 500) {
                if (!bid_order_id_.empty()) { api_.cancel_order(symbol_, bid_order_id_); bid_order_id_.clear(); last_bid_p_ = 0.0; }
                if (!ask_order_id_.empty()) { api_.cancel_order(symbol_, ask_order_id_); ask_order_id_.clear(); last_ask_p_ = 0.0; }
                momentum_active_ = false;
                momentum_retry_  = 0;
                momentum_last_entry_attempt_ms_ = now;
                return;
            }

            // 3) Retry entry если не fill
            if (age > 500 && momentum_retry_ < 3 && !has_pos) {
                if (!bid_order_id_.empty()) { api_.cancel_order(symbol_, bid_order_id_); bid_order_id_.clear(); }
                if (!ask_order_id_.empty()) { api_.cancel_order(symbol_, ask_order_id_); ask_order_id_.clear(); }

                double qty = api_.normalize_qty(symbol_, momentum_qty_usd_ / mid);
                if (qty > 0) {
                    if (momentum_skew_ > 0) {
                        double px = api_.normalize_price(symbol_, best_bid + tick_size * (momentum_retry_ + 1));
                        bid_order_id_ = api_.place_limit_order(symbol_, "Buy", px, qty, 0, 0, 0);
                        if (!bid_order_id_.empty()) last_bid_p_ = px;
                    } else {
                        double px = api_.normalize_price(symbol_, best_ask - tick_size * (momentum_retry_ + 1));
                        ask_order_id_ = api_.place_limit_order(symbol_, "Sell", px, qty, 0, 0, 0);
                        if (!ask_order_id_.empty()) last_ask_p_ = px;
                    }
                }
                momentum_retry_++;
            }
            return;
        }

        // ─── ENTRY ───
        double vpin_now = cluster_.get_vpin(20);
        bool cooldown_ok = (momentum_last_entry_attempt_ms_ == 0)
                           || (now - momentum_last_entry_attempt_ms_ > momentum_cooldown_ms_);
        bool vpin_ok = (vpin_now < vpin_pause_);
        bool no_pos = std::abs(inventory_usd_) < 1.0;

        if (cooldown_ok && vpin_ok && no_pos
            && std::abs(hawkes) > momentum_thr_
            && regime_.bid.mode != mm::SymMode::Toxic
            && regime_.ask.mode != mm::SymMode::Toxic)
        {
            double qty = api_.normalize_qty(symbol_, momentum_qty_usd_ / mid);
            if (qty > 0) {
                if (!bid_order_id_.empty()) { api_.cancel_order(symbol_, bid_order_id_); bid_order_id_.clear(); last_bid_p_ = 0.0; }
                if (!ask_order_id_.empty()) { api_.cancel_order(symbol_, ask_order_id_); ask_order_id_.clear(); last_ask_p_ = 0.0; }

                std::string oid;
                if (hawkes > 0) {
                    double px = api_.normalize_price(symbol_, best_bid + tick_size);
                    oid = api_.place_limit_order(symbol_, "Buy", px, qty, 0, 0, 0);
                    if (!oid.empty()) { bid_order_id_ = oid; last_bid_p_ = px; }
                } else {
                    double px = api_.normalize_price(symbol_, best_ask - tick_size);
                    oid = api_.place_limit_order(symbol_, "Sell", px, qty, 0, 0, 0);
                    if (!oid.empty()) { ask_order_id_ = oid; last_ask_p_ = px; }
                }

                if (!oid.empty()) {
                    momentum_active_   = true;
                    momentum_skew_     = hawkes;
                    momentum_entry_ms_ = now;
                    momentum_retry_    = 0;
                    momentum_last_entry_attempt_ms_ = now;
                    std::cout << "[MOMENTUM] " << symbol_
                              << (hawkes > 0 ? " BUY " : " SELL ")
                              << "skew=" << std::fixed << std::setprecision(2) << hawkes
                              << std::defaultfloat
                              << " qty=$" << momentum_qty_usd_ << "\n";
                }
            }
            return;
        }
    }
    // ═══ /MOMENTUM MODE ═══

    // ── Дальше — обычная MM (без изменений) ──
    if (vpin_pause_until_ > 0 && now < vpin_pause_until_) {
        if (!bid_order_id_.empty()) { api_.cancel_order(symbol_, bid_order_id_); log_cancel(bid_order_id_, "TOXIC_PAUSE"); bid_order_id_.clear(); last_bid_p_ = 0.0; }
        if (!ask_order_id_.empty()) { api_.cancel_order(symbol_, ask_order_id_); log_cancel(ask_order_id_, "TOXIC_PAUSE"); ask_order_id_.clear(); last_ask_p_ = 0.0; }
        return;
    }
    if (vpin_pause_until_ > 0 && now >= vpin_pause_until_) vpin_pause_until_ = 0;

    {
        double vpin = cluster_.get_vpin(20);
        if (vpin_pause_ <= 1.0) {
            if (now < vpin_pause_until_) {
                if (vpin < vpin_resume_) vpin_pause_until_ = 0;
                else {
                    if (!bid_order_id_.empty()) { api_.cancel_order(symbol_, bid_order_id_); bid_order_id_.clear(); last_bid_p_ = 0.0; }
                    if (!ask_order_id_.empty()) { api_.cancel_order(symbol_, ask_order_id_); ask_order_id_.clear(); last_ask_p_ = 0.0; }
                    return;
                }
            } else if (vpin > vpin_pause_) {
                vpin_pause_until_ = now + 15'000;
                if (!bid_order_id_.empty()) { api_.cancel_order(symbol_, bid_order_id_); bid_order_id_.clear(); last_bid_p_ = 0.0; }
                if (!ask_order_id_.empty()) { api_.cancel_order(symbol_, ask_order_id_); ask_order_id_.clear(); last_ask_p_ = 0.0; }
                return;
            }
        }
    }

    if ((regime_filter_ != nullptr) && regime_filter_->is_initialized() && !regime_filter_->is_mr_mode()) {
        if (!bid_order_id_.empty()) { api_.cancel_order(symbol_, bid_order_id_); bid_order_id_.clear(); last_bid_p_ = 0.0; }
        if (!ask_order_id_.empty()) { api_.cancel_order(symbol_, ask_order_id_); ask_order_id_.clear(); last_ask_p_ = 0.0; }
        return;
    }

    if (std::abs(inventory_qty_) > 1e-9) inventory_usd_ = inventory_qty_ * mid;
    update_pending_markouts(now);
    vol_estimator_.update(now, mid);
    fill_tracker_.update_mid(mid, now);
    run_adaptive_update(now);
    prune_placed_orders(now);

    if (!adaptive_params_.enabled) {
        if (!bid_order_id_.empty()) { api_.cancel_order(symbol_, bid_order_id_); bid_order_id_.clear(); last_bid_p_ = 0.0; }
        if (!ask_order_id_.empty()) { api_.cancel_order(symbol_, ask_order_id_); ask_order_id_.clear(); last_ask_p_ = 0.0; }
        if (std::abs(inventory_usd_) > 1.0) close_position_market(best_bid, best_ask, "KILL");
        return;
    }

    // MAX HOLD
    {
        const double abs_inv = std::abs(inventory_usd_);
        if (abs_inv > 1.0 && position_opened_time_ > 0) {
            uint64_t age_ms = (now > position_opened_time_) ? (now - position_opened_time_) : 0;
            uint64_t base_hold = 300'000;
            uint64_t dynamic_max_hold;
            if (max_hold_override_sec_ > 0) dynamic_max_hold = uint64_t(max_hold_override_sec_) * 1000ULL;
            else dynamic_max_hold = std::min(base_hold, adaptive_params_.max_hold_ms);

            if (regime_.bid.mode == mm::SymMode::Toxic || regime_.ask.mode == mm::SymMode::Toxic)
                dynamic_max_hold = std::min<uint64_t>(dynamic_max_hold, 5'000ULL);

            if (age_ms > dynamic_max_hold) close_position_market(best_bid, best_ask, "MAX_HOLD");
        }
    }

    if (std::abs(inventory_usd_) >= max_position_usd_ * hard_cap_ratio_) {
        close_position_market(best_bid, best_ask, "HARD_CAP");
        return;
    }

    double q_ratio = std::clamp(inventory_usd_ / max_position_usd_, -1.0, 1.0);

    double obi = orderbook_.get_imbalance_at_levels(5);
    constexpr double obi_skew_max = 0.30;

    double ofi_raw = orderbook_.get_ofi_sum(100);
    double ofi_norm = 0.0;
    double bbo_size = std::max(orderbook_.get_best_bid_size(), orderbook_.get_best_ask_size());
    if (bbo_size > 1e-9) ofi_norm = ofi_raw / bbo_size;
    ofi_norm = std::clamp(ofi_norm, -3.0, 3.0);
    constexpr double ofi_skew_max = 0.15;

    double momentum_skew = 0.0;
    if (momentum_weight_ > 0.01 && mid_history_.size() >= 3) {
        uint64_t t_200ms_ago = (now > 200) ? (now - 200) : 0;
        double mid_200ms_ago = mid;
        for (auto it = mid_history_.rbegin(); it != mid_history_.rend(); ++it) {
            if (it->first <= t_200ms_ago) { mid_200ms_ago = it->second; break; }
        }
        if (mid_200ms_ago > 0.0) {
            double drift_200ms = std::clamp((mid - mid_200ms_ago) / mid_200ms_ago, -0.00005, 0.00005);
            momentum_skew = +drift_200ms * mid * momentum_weight_;
        }
    }

    if (std::abs(q_ratio) > 0.15 && last_unrealized_pnl_pct_ < -0.005) {
        close_position_market(best_bid, best_ask, "SOFT_EXIT");
        return;
    }

    if (std::abs(inventory_usd_) > 5.0 && position_opened_time_ > 0) {
        uint64_t age_ms = (now > position_opened_time_) ? (now - position_opened_time_) : 0;
        double pnl_bps = last_unrealized_pnl_pct_ * 10000.0;
        if (age_ms >= 5'000 && age_ms < 30'000 && pnl_bps < -15.0) {
            close_position_market(best_bid, best_ask, "FAST_ADVERSE");
            return;
        }
    }

    mid_history_.push_back({now, mid});
    while (!mid_history_.empty() && now - mid_history_.front().first > 60'000) mid_history_.pop_front();

    hma_fast_.on_mid(now, mid);
    hma_slow_.on_mid(now, mid);
    kernel_.on_mid(now, mid);

    double hma_skew_bps = compute_hma_skew_bps();
    hma_skew_bps_ = hma_skew_bps;

    bool trend_detected = false;
    double drift_60s = 0.0;
    double trend_thr = base_spread_pct_ * 0.3;
    if (trend_thr_override_bps_ > 0.0) trend_thr = trend_thr_override_bps_ / 10000.0;
    else if (trend_thr < 0.0050) trend_thr = 0.0050;

    if (mid_history_.size() >= 15) {
        double mid_60s_ago = mid_history_.front().second;
        if (mid_60s_ago > 0) {
            drift_60s = (mid - mid_60s_ago) / mid_60s_ago;
            if (std::abs(drift_60s) > trend_thr) trend_detected = true;
        }
    }

    if (trend_detected) {
        if (!bid_order_id_.empty()) { api_.cancel_order(symbol_, bid_order_id_); bid_order_id_.clear(); last_bid_p_ = 0.0; }
        if (!ask_order_id_.empty()) { api_.cancel_order(symbol_, ask_order_id_); ask_order_id_.clear(); last_ask_p_ = 0.0; }
        if (std::abs(inventory_usd_) > 5.0) {
            close_position_market(best_bid, best_ask, "TREND");
            return;
        }
        last_trend_cut_ms_ = now;
        return;
    }

    double sigma = 0.0;
    if (!vol_estimator_.get_sigma_per_sqrt_sec(sigma) || sigma < min_sigma_) sigma = last_sigma_;
    else last_sigma_ = sigma;
    sigma = std::clamp(sigma, min_sigma_, max_sigma_);

    if (last_sigma_ema_ms_ == 0 || now - last_sigma_ema_ms_ > 1'000) {
        double alpha = 0.05;
        avg_sigma_1m_ = (avg_sigma_1m_ <= 0.0) ? sigma : avg_sigma_1m_ * (1.0 - alpha) + sigma * alpha;
        last_sigma_ema_ms_ = now;
    }
    double vol_ratio = (avg_sigma_1m_ > 0.0) ? (sigma / avg_sigma_1m_) : 1.0;

    double stress_mult_spread = 1.0, stress_mult_qty = 1.0;
    if (vol_ratio > vol_ratio_max_) {
        if (!bid_order_id_.empty()) { api_.cancel_order(symbol_, bid_order_id_); bid_order_id_.clear(); last_bid_p_ = 0.0; }
        if (!ask_order_id_.empty()) { api_.cancel_order(symbol_, ask_order_id_); ask_order_id_.clear(); last_ask_p_ = 0.0; }
        return;
    }
    if (vol_ratio > 1.3) {
        stress_mult_spread = 1.0 + (vol_ratio - 1.0) * 1.5;
        stress_mult_qty = 1.0 / stress_mult_spread;
    }

    double eff_spread_pct = use_adaptive_ ? adaptive_params_.base_spread_pct : base_spread_pct_;
    double eff_qty_usd = (use_adaptive_ ? adaptive_params_.order_qty_usd : order_qty_usd_) * stress_mult_qty;

    double vpin = cluster_.get_vpin(20);
    double vpin_mult = (vpin > 0.5) ? (1.0 + (vpin - 0.5) * 2.0) : 1.0;

    double sigma_ratio = std::clamp(sigma / sigma_ref_, 0.5, 3.0);
    double effective_spread_pct = eff_spread_pct * std::sqrt(sigma_ratio) * stress_mult_spread * vpin_mult;

    double min_spread_bps = std::max(10.0, 2.0 * maker_fee_bps_ + 2.0);
    double min_spread_pct = min_spread_bps / 10000.0;
    if (effective_spread_pct < min_spread_pct) effective_spread_pct = min_spread_pct;
    effective_spread_pct = std::clamp(effective_spread_pct, min_half_spread_pct_ * 2.0, max_half_spread_pct_ * 2.0);

    double half_spread = mid * effective_spread_pct / 2.0;
    half_spread = std::clamp(half_spread, mid * min_half_spread_pct_, mid * max_half_spread_pct_);

    double q_sq = q_ratio * q_ratio;
    double inv_penalty = q_ratio / std::max(1e-6, 1.0 - 0.5 * q_sq);

    double time_urgency = 0.0;
    if (position_opened_time_ > 0 && std::abs(inventory_usd_) > 1.0 && std::abs(inventory_usd_) < 20.0) {
        uint64_t age_ms = (now > position_opened_time_) ? (now - position_opened_time_) : 0;
        double age_ratio = std::min(1.0, (double)age_ms / 30000.0);
        time_urgency = age_ratio * age_ratio;
    }
    double skew_mult = 1.0 + 1.5 * time_urgency;
    double skew_raw = -inv_penalty * half_spread * gamma_ * sigma_ratio * skew_mult;
    double skew = std::clamp(skew_raw, -1.0 * half_spread, 1.0 * half_spread);

    double bid_regime_mult = 1.0, ask_regime_mult = 1.0;
    if (regime_.bid.mode == mm::SymMode::Toxic)    bid_regime_mult = 1.5;
    if (regime_.ask.mode == mm::SymMode::Toxic)    ask_regime_mult = 1.5;
    if (regime_.bid.mode == mm::SymMode::Illiquid) bid_regime_mult = 2.0;
    if (regime_.ask.mode == mm::SymMode::Illiquid) ask_regime_mult = 2.0;

    double bid_half = half_spread * bid_regime_mult;
    double ask_half = half_spread * ask_regime_mult;

    if (q_ratio > 0.05) ask_half = half_spread * ask_regime_mult * (1.0 + q_ratio * 0.5);
    else if (q_ratio < -0.05) bid_half = half_spread * bid_regime_mult * (1.0 + (-q_ratio) * 0.5);

    if (std::abs(hma_skew_bps) > 0.5) {
        double hma_factor = std::clamp(hma_skew_bps / 30.0, -0.05, 0.05);
        bid_half *= (1.0 - hma_factor);
        ask_half *= (1.0 + hma_factor);
    }

    double obi_skew = obi * half_spread * obi_skew_max;
    double ofi_skew = ofi_norm * half_spread * ofi_skew_max;

    double reservation = micro;
    double cross_skew = 0.0;
    constexpr double kMaxCrossBps = 8.0;

    double hawkes_bps = 0.0;
    if (tp_engine_) hawkes_bps = tp_engine_->target_skew_bps(now);

    if (signal_engine_) {
        auto sig = signal_engine_->get_signal(symbol_);
        double clamped_bps = std::clamp(sig.target_skew_bps, -kMaxCrossBps, kMaxCrossBps);
        cross_skew_bps_ = clamped_bps;
        cross_skew = clamped_bps / 10000.0 * mid;
    }

    double tp_shadow_bps = hawkes_bps;

    double bid_price = reservation - bid_half + skew + momentum_skew + obi_skew + ofi_skew + cross_skew;
    double ask_price = reservation + ask_half + skew + momentum_skew + obi_skew + ofi_skew + cross_skew;

    bid_price = std::floor(api_.normalize_price(symbol_, bid_price) / tick_size + 1e-9) * tick_size;
    ask_price = std::ceil (api_.normalize_price(symbol_, ask_price) / tick_size - 1e-9) * tick_size;

    constexpr int kMinTicksFromTop = 1;
    constexpr double kMinBpsFromTop = 0.5;
    double min_gap = std::max(tick_size * kMinTicksFromTop, best_bid * 1e-4);

    if (bid_price >= best_bid - tick_size * 0.5) bid_price = best_bid - min_gap;
    if (ask_price <= best_ask + tick_size * 0.5) ask_price = best_ask + min_gap;

    auto floor_to_tick = [&](double p) { return std::floor(p / tick_size + 1e-9) * tick_size; };
    auto ceil_to_tick  = [&](double p) { return std::ceil (p / tick_size - 1e-9) * tick_size; };

    double min_abs_bid = std::max(tick_size * kMinTicksFromTop, best_bid * kMinBpsFromTop / 10000.0);
    double min_abs_ask = std::max(tick_size * kMinTicksFromTop, best_ask * kMinBpsFromTop / 10000.0);

    double max_allowed_bid = floor_to_tick(best_bid - min_abs_bid);
    if (bid_price > max_allowed_bid) bid_price = max_allowed_bid;
    double min_allowed_ask = ceil_to_tick(best_ask + min_abs_ask);
    if (ask_price < min_allowed_ask) ask_price = min_allowed_ask;

    if (bid_price >= ask_price) return;

    double bid_block_ratio = block_ratio_;
    double ask_block_ratio = block_ratio_;
    if (regime_.bid.mode == mm::SymMode::Toxic) bid_block_ratio = 0.30;
    if (regime_.ask.mode == mm::SymMode::Toxic) ask_block_ratio = 0.30;

    bool allow_bid = (bid_price < best_ask) && (q_ratio < bid_block_ratio);
    bool allow_ask = (ask_price > best_bid) && (q_ratio > -ask_block_ratio);

    if (regime_.bid.mode == mm::SymMode::Toxic && regime_.bid.n_markouts_4h >= 20 && regime_.bid.hit_pct_1s < 20.0) allow_bid = false;
    if (regime_.ask.mode == mm::SymMode::Toxic && regime_.ask.n_markouts_4h >= 20 && regime_.ask.hit_pct_1s < 20.0) allow_ask = false;

    constexpr double kPositionRiskBlock = 0.5;
    if (q_ratio > kPositionRiskBlock) allow_bid = false;
    if (q_ratio < -kPositionRiskBlock) allow_ask = false;
    if (now < ask_cooldown_until_) allow_ask = false;
    if (now < bid_cooldown_until_) allow_bid = false;
    if (!allow_bid && !bid_order_id_.empty()) { api_.cancel_order(symbol_, bid_order_id_); log_cancel(bid_order_id_, "BLOCKED"); bid_order_id_.clear(); last_bid_p_ = 0.0; }
    if (!allow_ask && !ask_order_id_.empty()) { api_.cancel_order(symbol_, ask_order_id_); log_cancel(ask_order_id_, "BLOCKED"); ask_order_id_.clear(); last_ask_p_ = 0.0; }

    double qty_factor = 1.0 - std::abs(q_ratio) * 0.7;
    double adaptive_qty_usd = eff_qty_usd * qty_factor;

    double bid_qty_mult = 1.0, ask_qty_mult = 1.0;
    if (regime_.bid.mode == mm::SymMode::Toxic) bid_qty_mult = 0.3;
    if (regime_.ask.mode == mm::SymMode::Toxic) ask_qty_mult = 0.3;
    if (regime_.bid.mode == mm::SymMode::Illiquid) bid_qty_mult = 0.5;
    if (regime_.ask.mode == mm::SymMode::Illiquid) ask_qty_mult = 0.5;

    double qty_bid = api_.normalize_qty(symbol_, adaptive_qty_usd * bid_qty_mult / mid);
    double qty_ask = api_.normalize_qty(symbol_, adaptive_qty_usd * ask_qty_mult / mid);

    if (qty_bid <= 0) { allow_bid = false; if (!bid_order_id_.empty()) { api_.cancel_order(symbol_, bid_order_id_); log_cancel(bid_order_id_, "ZERO_QTY"); bid_order_id_.clear(); last_bid_p_ = 0.0; } }
    if (qty_ask <= 0) { allow_ask = false; if (!ask_order_id_.empty()) { api_.cancel_order(symbol_, ask_order_id_); log_cancel(ask_order_id_, "ZERO_QTY"); ask_order_id_.clear(); last_ask_p_ = 0.0; } }

    auto min_notional_fix = [&](double q, mm::SymMode mode) -> double {
        if (q <= 0.0) return 0.0;
        if (q * mid >= 5.5) return q;
        if (mode == mm::SymMode::Toxic) return 0.0;
        return api_.normalize_qty(symbol_, 5.5 / mid);
    };
    qty_bid = min_notional_fix(qty_bid, regime_.bid.mode);
    qty_ask = min_notional_fix(qty_ask, regime_.ask.mode);
    if (qty_bid <= 0) allow_bid = false;
    if (qty_ask <= 0) allow_ask = false;

    const double bid_move_thr = (regime_.bid.mode == mm::SymMode::Tier1) ? 2.0 : 0.5;
    const double ask_move_thr = (regime_.ask.mode == mm::SymMode::Tier1) ? 2.0 : 0.5;

    bool bid_needs_replace = (last_bid_p_ == 0.0) || (std::abs(last_bid_p_ - bid_price) / bid_half > bid_move_thr);
    if (now - last_bid_place_ms_ < 30'000 && !bid_order_id_.empty()) bid_needs_replace = false;
    bool ask_needs_replace = (last_ask_p_ == 0.0) || (std::abs(last_ask_p_ - ask_price) / ask_half > ask_move_thr);
    if (now - last_ask_place_ms_ < 30'000 && !ask_order_id_.empty()) ask_needs_replace = false;

    if (!bid_needs_replace && !bid_order_id_.empty()) { if (std::abs(last_bid_p_ - bid_price) < tick_size * 1.0) bid_price = last_bid_p_; }
    if (!ask_needs_replace && !ask_order_id_.empty()) { if (std::abs(last_ask_p_ - ask_price) < tick_size * 1.0) ask_price = last_ask_p_; }

    PlaceContext ctx{mid, best_bid, best_ask, half_spread, skew, sigma, q_ratio, inventory_usd_};
    if (signal_engine_) {
        auto sig = signal_engine_->get_signal(symbol_);
        ctx.cross_bps = tp_shadow_bps;
        ctx.ret_500 = sig.ret_500ms_bybit;
        ctx.flow_b = sig.bybit_flow_imbalance;
        ctx.flow_bnx = sig.bitunix_flow_imbalance;
        ctx.ofi = sig.ofi_bybit;
        ctx.delta = sig.delta_bps;
    }
    ctx.hma_skew_bps = hma_skew_bps;
    ctx.hma_fast_val = hma_fast_.value();
    ctx.hma_slow_val = hma_slow_.value();
    ctx.kernel_val = kernel_.value(now);

    if (allow_bid && bid_needs_replace) {
        if (!bid_order_id_.empty()) { api_.cancel_order(symbol_, bid_order_id_); log_cancel(bid_order_id_, "REPLACED"); bid_order_id_.clear(); last_bid_p_ = 0.0; }
        bid_order_id_ = api_.place_limit_order(symbol_, "Buy", bid_price, qty_bid, 0, 0, 0);
        if (!bid_order_id_.empty()) {
            log_order(bid_order_id_, "Buy", bid_price, qty_bid, ctx);
            last_bid_p_ = bid_price; last_bid_p_time_ms_ = now; last_bid_place_ms_ = now;
            placed_orders_[bid_order_id_] = {bid_price, "Buy", now};
        } else {
            int rc = api_.last_ret_code();
            if (rc == 110126) { adaptive_params_.enabled = false; return; }
            if (rc == -1) return;
            log_reject("Buy", bid_price, qty_bid, ctx, rc, api_.last_ret_msg());
        }
    }

    if (allow_ask && ask_needs_replace) {
        if (!ask_order_id_.empty()) { api_.cancel_order(symbol_, ask_order_id_); log_cancel(ask_order_id_, "REPLACED"); ask_order_id_.clear(); last_ask_p_ = 0.0; }
        ask_order_id_ = api_.place_limit_order(symbol_, "Sell", ask_price, qty_ask, 0, 0, 0);
        if (!ask_order_id_.empty()) {
            log_order(ask_order_id_, "Sell", ask_price, qty_ask, ctx);
            last_ask_p_ = ask_price; last_ask_p_time_ms_ = now; last_ask_place_ms_ = now;
            placed_orders_[ask_order_id_] = {ask_price, "Sell", now};
        } else {
            int rc = api_.last_ret_code();
            if (rc == 110126) { adaptive_params_.enabled = false; return; }
            if (rc == -1) return;
            log_reject("Sell", ask_price, qty_ask, ctx, rc, api_.last_ret_msg());
        }
    }

    last_half_spread_ = half_spread;
    last_skew_ = skew;

    static std::unordered_map<std::string, RateLimiter> _rl;
    auto& rl = _rl[symbol_];
    if (rl.ok()) {
        double spread_bps = half_spread * 2.0 / mid * 10000.0;
        double pnl_pct = last_unrealized_pnl_pct_ * 100.0;
        const char* pnl_color = pnl_pct >= 0 ? clr::green : clr::red;
        const char* inv_color = std::abs(inventory_usd_) > max_position_usd_ * 0.5 ? clr::yellow : clr::reset;

        std::cout << clr::gray << fmt_ts() << clr::reset << "  "
                  << clr::bold << "MM" << clr::reset << "  "
                  << pad_r(symbol_, 14)
                  << pad_l(num(bid_price, 4), 10)
                  << pad_l(num(ask_price, 4), 10)
                  << pad_l(num(spread_bps, 1), 5) << "bps"
                  << " " << (hma_skew_bps_ >= 0 ? clr::green : clr::red)
                  << pad_l(num(hma_skew_bps_, 1), 5) << clr::reset
                  << pad_l(num(std::abs(sigma * 10000), 1), 5)
                  << inv_color << pad_l(num(inventory_usd_, 0), 6) << clr::reset
                  << pnl_color << pad_l(num(pnl_pct, 2), 7) << "%" << clr::reset
                  << "  " << (adaptive_params_.enabled ? clr::green : clr::red)
                  << (adaptive_params_.enabled ? "ok" : "off") << clr::reset
                  << "  " << clr::cyan
                  << "b:" << mm::to_str(regime_.bid.mode)
                  << "/a:" << mm::to_str(regime_.ask.mode)
                  << clr::reset << "\n";
    }
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
    // NOTE: paper-fallback вызывает это из WS-треда. Live идёт через on_private_execution
// на strand'е pos_manage_timer_. Для paper TODO: сериализовать через post(exec_).
    double      p = fill_price;
    std::string s = side;

    auto it = placed_orders_.find(order_id);
    if (it != placed_orders_.end()) {
        if (p <= 0)    p = it->second.price;
        if (s.empty()) s = it->second.side;
        placed_orders_.erase(it);
    }

    if (fill_qty <= 0) fill_qty = 0.0;

    if (p > 0 && !s.empty()) {
        uint64_t ts = fill_ts_ms > 0 ? fill_ts_ms : this->now();
        int side_sign = (s == "Buy") ? +1 : -1;

        inventory_qty_ += side_sign * fill_qty;
        inventory_usd_  = inventory_qty_ * p;   // приблизительно
        if (std::abs(inventory_qty_) < 1e-9) {
            inventory_side_.clear();
            position_opened_time_ = 0;
        } else {
            inventory_side_ = (inventory_qty_ > 0) ? "Buy" : "Sell";
            if (position_opened_time_ == 0) position_opened_time_ = ts;
        }

        fill_tracker_.record_fill(p, side_sign, ts);

        bool is_close_fill = false;
        if (!inventory_side_.empty() && inventory_side_ != s) {
            is_close_fill = true;
        }

        // ── Markout tracker ──
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
            m.order_id = markout_key;

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
    }

    if (order_id == bid_order_id_) {
        if (is_full_fill) {
            bid_order_id_.clear();
            last_bid_p_ = 0.0;
            std::cout << "[MM] " << symbol_ << " bid fully filled, will re-place\n";
        } else {
            std::cout << "[MM] " << symbol_ << " bid PARTIAL fill, keep tracking\n";
        }
    }
    if (order_id == ask_order_id_) {
        if (is_full_fill) {
            ask_order_id_.clear();
            last_ask_p_ = 0.0;
            std::cout << "[MM] " << symbol_ << " ask fully filled, will re-place\n";
        } else {
            std::cout << "[MM] " << symbol_ << " ask PARTIAL fill, keep tracking\n";
        }
    }

    if (order_id == pending_close_oid_) {
        pending_close_oid_.clear();
        pending_close_ms_ = 0;
        pending_close_first_attempt_ms_ = 0;
    }
}

// ============================================================
// TOXIC DETECTION
// ============================================================
void MarketMakingStrategy::on_trade_through_ask(double price, double size) {
    // Cheap pre-filter, race-free (только чтение аргументов)
    if (price * size < 3000.0) return;

    if (has_exec_) {
        // Публикуем на executor-странд; сами проверки — там.
        net::post(exec_, [self = weak_from_this(), price]() {
            if (auto sp = self.lock()) sp->cancel_ask_on_through(price, "TOXIC_THROUGH");
        });
        return;
    }
    // Fallback (бэктест / paper без executor) — старое поведение
    pending_cancel_ask_.store(true, std::memory_order_relaxed);
}

void MarketMakingStrategy::on_trade_through_bid(double price, double size) {
    if (price * size < 3000.0) return;

    if (has_exec_) {
        net::post(exec_, [self = weak_from_this(), price]() {
            if (auto sp = self.lock()) sp->cancel_bid_on_through(price, "TOXIC_THROUGH");
        });
        return;
    }
    pending_cancel_bid_.store(true, std::memory_order_relaxed);
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