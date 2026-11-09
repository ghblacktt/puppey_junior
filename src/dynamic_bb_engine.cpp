#include "dynamic_bb_engine.h"
#include <algorithm>
#include <numeric>
#include <cmath>

DynamicBBEngine::DynamicBBEngine(
        int alma_period, double alma_offset, double alma_sigma,
        int bb_period, double base_multiplier,
        int vol_lookback, double percentile_coverage
) : alma_period_(alma_period), alma_offset_(alma_offset), alma_sigma_(alma_sigma),
    bb_period_(bb_period), base_multiplier_(base_multiplier),
    vol_lookback_(vol_lookback), percentile_coverage_(percentile_coverage) {}

void DynamicBBEngine::add_tick(double price, double volume, uint64_t timestamp_ms) {
    std::lock_guard<std::mutex> lock(mutex_);

    ticks_.push_back({timestamp_ms, price, volume});

    if (ticks_.size() > MAX_TICKS) {
        ticks_.pop_front();
    }

    if (ticks_.size() >= (size_t)vol_lookback_ && ticks_.size() % 10 == 0) {
        double vol = calculate_volatility();
        volatility_history_.push_back(vol);

        if (volatility_history_.size() > MAX_VOL_HISTORY) {
            volatility_history_.pop_front();
        }
    }
}

auto DynamicBBEngine::is_ready() const -> bool {
    std::lock_guard<std::mutex> lock(mutex_);
    return ticks_.size() >= (size_t)alma_period_;
}

auto DynamicBBEngine::get_tick_count() const -> size_t {
    std::lock_guard<std::mutex> lock(mutex_);
    return ticks_.size();
}

auto DynamicBBEngine::get_bands() const -> DynamicBBEngine::Bands {
    std::lock_guard<std::mutex> lock(mutex_);

    Bands bands;
    if (ticks_.size() < 2) {
        return bands;
    }

    double sum_pv = 0.0, sum_v = 0.0;
    for (const auto& tick : ticks_) {
        sum_pv += tick.price * tick.volume;
        sum_v += tick.volume;
    }

    if (sum_v <= 0) {
        return bands;
    }

    bands.alma = sum_pv / sum_v;

    bands.std_dev = calculate_std_dev(bands.alma);

    double vol = calculate_volatility();
    bands.volatility = vol;
    bands.dynamic_multiplier = calculate_dynamic_multiplier(vol);

    double mult = bands.dynamic_multiplier;
    bands.band1_upper = bands.alma + mult * 1.0 * bands.std_dev;
    bands.band1_lower = bands.alma - mult * 1.0 * bands.std_dev;
    bands.band2_upper = bands.alma + mult * 1.5 * bands.std_dev;
    bands.band2_lower = bands.alma - mult * 1.5 * bands.std_dev;
    bands.band3_upper = bands.alma + mult * 2.5 * bands.std_dev;
    bands.band3_lower = bands.alma - mult * 2.5 * bands.std_dev;
    bands.band4_upper = bands.alma + mult * 4.0 * bands.std_dev;
    bands.band4_lower = bands.alma - mult * 4.0 * bands.std_dev;

    return bands;
}

auto DynamicBBEngine::calculate_alma() const -> double {
    if (ticks_.size() < (size_t)alma_period_) return 0.0;

    double m = alma_offset_ * (alma_period_ - 1);
    double s = (double)alma_period_ / alma_sigma_;

    double sum_weight = 0.0;
    double sum_weighted = 0.0;

    for (int i = 0; i < alma_period_; ++i) {
        double weight = std::exp(-std::pow(i - m, 2) / (2.0 * s * s));
        sum_weight += weight;
        sum_weighted += weight * ticks_[ticks_.size() - 1 - i].price;
    }

    return (sum_weight > 0) ? (sum_weighted / sum_weight) : 0.0;
}

auto DynamicBBEngine::calculate_std_dev(double mean) const -> double {
    size_t start = (ticks_.size() >= (size_t)bb_period_) ? ticks_.size() - bb_period_ : 0;
    size_t count = ticks_.size() - start;
    if (count < 2) return 0.0;

    double sum_sq = 0.0;
    for (size_t i = start; i < ticks_.size(); ++i) {
        double diff = ticks_[i].price - mean;
        sum_sq += diff * diff;
    }

    return std::sqrt(sum_sq / count);
}

auto DynamicBBEngine::calculate_volatility() const -> double {
    size_t start = (ticks_.size() >= (size_t)vol_lookback_) ? ticks_.size() - vol_lookback_ : 0;
    if (ticks_.size() - start < 2) return 0.0;

    std::vector<double> returns;
    for (size_t i = start + 1; i < ticks_.size(); ++i) {
        if (ticks_[i-1].price > 0) {
            returns.push_back((ticks_[i].price - ticks_[i-1].price) / ticks_[i-1].price);
        }
    }

    if (returns.empty()) return 0.0;

    double mean = std::accumulate(returns.begin(), returns.end(), 0.0) / returns.size();
    double sum_sq = 0.0;
    for (double r : returns) {
        sum_sq += (r - mean) * (r - mean);
    }

    return std::sqrt(sum_sq / returns.size());
}

auto DynamicBBEngine::calculate_dynamic_multiplier(double current_vol) const -> double {
    if (volatility_history_.empty() || current_vol <= 0) return base_multiplier_;

    std::vector<double> vol_hist(volatility_history_.begin(), volatility_history_.end());
    double vol_p90 = percentile(vol_hist, percentile_coverage_);

    if (vol_p90 <= 0) return base_multiplier_;

    double ratio = current_vol / vol_p90;

    if (ratio > 1.0) {
        return base_multiplier_ * (1.0 + 0.5 * (ratio - 1.0));
    } else {
        return std::max(base_multiplier_ * (0.5 + 0.5 * ratio), base_multiplier_ * 0.5);
    }
}

void DynamicBBEngine::add_price(double price, uint64_t timestamp) {
    add_tick(price, 0.0, timestamp);
}

auto DynamicBBEngine::percentile(std::vector<double> values, double p) const -> double {
    if (values.empty()) return 0.0;

    std::ranges::sort(values);
    double index = (p / 100.0) * (values.size() - 1);
    auto lower = (size_t)std::floor(index);
    auto upper = (size_t)std::ceil(index);

    if (lower == upper || upper >= values.size()) {
        return values[lower];
    }

    double frac = index - lower;
    return values[lower] * (1.0 - frac) + values[upper] * frac;
}