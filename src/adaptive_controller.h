#pragma once
#include "fill_outcome_tracker.h"
#include <algorithm>
#include <cmath>

class AdaptiveController {
public:
    struct Params {
        double   base_spread_pct = 0.002;
        double   order_qty_usd   = 15.0;
        uint64_t max_hold_ms     = 60'000;
        bool     enabled         = true;
    };

    struct Config {
        double   min_half_spread_bps = 4.0;
        double   max_half_spread_bps = 30.0;
        double   min_qty_usd         = 10.0;
        double   max_qty_usd         = 50.0;
        uint64_t min_hold_ms         = 3'000ULL;
        uint64_t max_hold_ms         = 180'000ULL;
        double   kill_adverse_bps    = 8.0;
        size_t   min_samples         = 10;
    };

    // clang-friendly: без default argument, отдельный default ctor
    AdaptiveController() = default;
    explicit AdaptiveController(Config c) : cfg_(c) {}

    Params recompute(const FillOutcomeTracker& tracker,
                     Params current,
                     double maker_fee_bps) {
        auto buy_2s  = tracker.get_stats_2s(+1);
        auto sell_2s = tracker.get_stats_2s(-1);

        size_t n_total = buy_2s.n + sell_2s.n;
        if (n_total < cfg_.min_samples) return current;

        double worst_p25 = 0.0;
        bool has = false;
        if (buy_2s.n  > 0) { worst_p25 = std::min(worst_p25, buy_2s.p25);  has = true; }
        if (sell_2s.n > 0) { worst_p25 = std::min(worst_p25, sell_2s.p25); has = true; }
        if (!has) return current;

        if (worst_p25 < -cfg_.kill_adverse_bps) {
            current.enabled = false;
            return current;
        }

        double adverse_pen = std::max(0.0, -worst_p25);
        double desired_half_bps = 2.0 * maker_fee_bps + adverse_pen + 2.0;
        desired_half_bps = std::clamp(desired_half_bps,
                                      cfg_.min_half_spread_bps,
                                      cfg_.max_half_spread_bps);
        current.base_spread_pct = 2.0 * desired_half_bps / 10000.0;

        current.max_hold_ms = std::clamp(tracker.get_optimal_hold_ms(),
                                         cfg_.min_hold_ms, cfg_.max_hold_ms);

        double edge_bps = desired_half_bps - maker_fee_bps + worst_p25;
        if (edge_bps > 1.0)      current.order_qty_usd = std::min(current.order_qty_usd * 1.2, cfg_.max_qty_usd);
        else if (edge_bps < 0.0) current.order_qty_usd = std::max(current.order_qty_usd * 0.8, cfg_.min_qty_usd);

        current.enabled = true;
        return current;
    }

    Config& config() { return cfg_; }

private:
    Config cfg_{};
};