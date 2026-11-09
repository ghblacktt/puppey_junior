#pragma once
#include <deque>
#include <cmath>
#include <cstdint>
#include <utility>

// ═══════════════════════════════════════════════════════════
// Nadaraya-Watson Kernel Regression
// ŷ(t) = Σ K((t - t_i)/h) * y_i / Σ K((t - t_i)/h)
// K(u) = exp(-u²/2)  — гауссово ядро
//
// Работает с tick timestamps напрямую (не бары).
// bandwidth_ms — параметр сглаживания (больше = глаже).
// ═══════════════════════════════════════════════════════════
class KernelFilter {
public:
    KernelFilter(uint64_t window_ms = 120'000,
                 double   bandwidth_ms = 30'000)
            : window_ms_(window_ms), bandwidth_ms_(bandwidth_ms) {}

    void on_mid(uint64_t ts_ms, double mid) {
        if (mid <= 0.0) return;
        data_.emplace_back(ts_ms, mid);
        while (!data_.empty() && ts_ms - data_.front().first > window_ms_) {
            data_.pop_front();
        }
    }

    bool ready() const { return data_.size() >= 10; }

    double value(uint64_t now_ts_ms) const {
        if (data_.empty()) return 0.0;

        double num = 0.0, den = 0.0;
        for (const auto& [ts, mid] : data_) {
            double dt = static_cast<double>(now_ts_ms - ts);
            double u  = dt / bandwidth_ms_;
            double k  = std::exp(-0.5 * u * u);
            num += k * mid;
            den += k;
        }
        return den > 0.0 ? num / den : 0.0;
    }

private:
    std::deque<std::pair<uint64_t, double>> data_;
    uint64_t window_ms_;
    double   bandwidth_ms_;
};