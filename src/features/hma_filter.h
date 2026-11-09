#pragma once
#include <deque>
#include <cmath>
#include <cstdint>
#include <vector>

// ═══════════════════════════════════════════════════════════
// Hull Moving Average
// Формула: HMA(n) = 2*WMA(n/2) - WMA(n)
// (каноничный HMA без финального WMA(sqrt(n)) — lag ~n/3
//  вместо n/4, но для HFT 1-2 бара разницы не критично)
//
// Работает на 1s барах (resampling внутренний).
// HMA(8) = 8 секунд, HMA(32) = 32 секунды.
// ═══════════════════════════════════════════════════════════
class HmaFilter {
public:
    explicit HmaFilter(size_t period, uint64_t bar_ms = 1000)
            : period_(period), bar_ms_(bar_ms) {}

    void on_mid(uint64_t ts_ms, double mid) {
        if (mid <= 0.0) return;

        uint64_t bar_ts = (ts_ms / bar_ms_) * bar_ms_;
        if (bar_ts == current_bar_ts_) {
            bar_sum_ += mid;
            ++bar_count_;
        } else {
            if (bar_count_ > 0) {
                bars_.push_back(bar_sum_ / static_cast<double>(bar_count_));
                while (bars_.size() > period_ * 4) {
                    bars_.pop_front();
                }
            }
            bar_sum_   = mid;
            bar_count_ = 1;
            current_bar_ts_ = bar_ts;
        }
    }

    bool ready() const { return bars_.size() >= period_; }

    double value() const {
        if (bars_.size() < period_) return 0.0;

        size_t n  = period_;
        size_t n2 = n / 2;
        if (n2 == 0) n2 = 1;

        double wma_n  = wma(n);
        double wma_n2 = wma(n2);

        return 2.0 * wma_n2 - wma_n;
    }

private:
    double wma(size_t window) const {
        if (bars_.size() < window) return 0.0;

        double sum = 0.0, wsum = 0.0;
        size_t start = bars_.size() - window;
        for (size_t i = 0; i < window; ++i) {
            double w = static_cast<double>(i + 1);
            sum  += bars_[start + i] * w;
            wsum += w;
        }
        return wsum > 0.0 ? sum / wsum : 0.0;
    }

    size_t   period_;
    uint64_t bar_ms_;
    std::deque<double> bars_;

    uint64_t current_bar_ts_ = 0;
    double   bar_sum_        = 0.0;
    int      bar_count_      = 0;
};