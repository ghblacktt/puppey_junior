#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <vector>
#include <algorithm>
#include <cmath>

class FillOutcomeTracker {
public:
    static constexpr size_t MAX_PENDING = 500;
    static constexpr size_t MAX_HISTORY = 200;
    static constexpr size_t N_HORIZONS  = 7;

    static inline constexpr std::array<uint64_t, N_HORIZONS> HORIZONS_MS =
            {2000ULL, 5000ULL, 10000ULL, 30000ULL, 60000ULL, 180000ULL, 300000ULL};

    struct PendingFill {
        double   fill_price;
        int      side;          // +1 Buy, -1 Sell
        uint64_t fill_ts_ms;
        bool     done_mask[N_HORIZONS];
    };

    struct AdverseStats {
        size_t n    = 0;
        double mean = 0.0;
        double p25  = 0.0;
        double p50  = 0.0;
        double p75  = 0.0;
    };

    // Совместимо с вызовом: record_fill(price, side_int, ts_ms)
    void record_fill(double fill_price, int side, uint64_t ts_ms) {
        if (fill_price <= 0) return;
        PendingFill f;
        f.fill_price = fill_price;
        f.side       = (side >= 0) ? +1 : -1;
        f.fill_ts_ms = ts_ms;
        for (size_t i = 0; i < N_HORIZONS; ++i) f.done_mask[i] = false;
        pending_.push_back(f);
        while (pending_.size() > MAX_PENDING) pending_.pop_front();
    }

    // Перегрузка для совместимости со старым API (если где-то ещё вызывается)
    void record_fill(const std::string& /*order_id*/, double fill_price,
                     const std::string& side, uint64_t now_ms) {
        record_fill(fill_price, (side == "Buy") ? +1 : -1, now_ms);
    }

    // Вызывать каждый tick с текущим mid
    void update_mid(double mid, uint64_t now_ms) {
        if (mid <= 0) return;
        for (auto it = pending_.begin(); it != pending_.end(); ) {
            uint64_t age = now_ms - it->fill_ts_ms;

            for (size_t i = 0; i < N_HORIZONS; ++i) {
                if (it->done_mask[i]) continue;
                if (age < HORIZONS_MS[i]) continue;

                double raw     = (mid - it->fill_price) / it->fill_price * 10000.0;
                double adverse = raw * static_cast<double>(it->side);

                if (it->side > 0) stats_buy_[i].push_back(adverse);
                else              stats_sell_[i].push_back(adverse);

                while (stats_buy_[i].size()  > MAX_HISTORY) stats_buy_[i].pop_front();
                while (stats_sell_[i].size() > MAX_HISTORY) stats_sell_[i].pop_front();

                it->done_mask[i] = true;
            }

            if (age > 600'000ULL) it = pending_.erase(it);
            else                  ++it;
        }
    }

    AdverseStats get_stats(int side, size_t h) const {
        if (h >= N_HORIZONS) return {};
        return compute_stats(side > 0 ? stats_buy_[h] : stats_sell_[h]);
    }

    AdverseStats get_stats_2s (int side) const { return get_stats(side, 0); }
    AdverseStats get_stats_60s(int side) const { return get_stats(side, 4); }

    uint64_t get_optimal_hold_ms() const {
        uint64_t best_ms  = 60'000ULL;
        double   best_adv = -1e9;
        for (size_t i = 0; i < N_HORIZONS; ++i) {
            auto b = get_stats(+1, i);
            auto s = get_stats(-1, i);
            double mean = 0.0;
            int cnt = 0;
            if (b.n > 0) { mean += b.mean; ++cnt; }
            if (s.n > 0) { mean += s.mean; ++cnt; }
            if (cnt == 0) continue;
            mean /= cnt;
            if (mean > best_adv) { best_adv = mean; best_ms = HORIZONS_MS[i]; }
        }
        return best_ms;
    }

    size_t pending_count() const { return pending_.size(); }

private:
    static AdverseStats compute_stats(const std::deque<double>& d) {
        AdverseStats s;
        if (d.empty()) return s;
        std::vector<double> v(d.begin(), d.end());
        std::sort(v.begin(), v.end());
        s.n = v.size();
        double sum = 0.0;
        for (double x : v) sum += x;
        s.mean = sum / static_cast<double>(s.n);
        s.p25  = v[v.size() * 25 / 100];
        s.p50  = v[v.size() * 50 / 100];
        s.p75  = v[v.size() * 75 / 100];
        return s;
    }

    std::deque<PendingFill> pending_;
    std::deque<double> stats_buy_[N_HORIZONS];
    std::deque<double> stats_sell_[N_HORIZONS];
};