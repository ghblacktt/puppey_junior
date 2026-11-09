#include "time_window_vwma.h"
#include <cmath>
#include <numeric>

TimeWindowVWMA::TimeWindowVWMA(int window_size)
        : window_size_(window_size) {}

void TimeWindowVWMA::add_tick(double price, double volume, uint64_t timestamp_ms) {
    std::lock_guard<std::mutex> lock(mutex_);

    ticks_.push_back({price, volume});

    // Оставляем только последние window_size_ тиков
    if (ticks_.size() > static_cast<size_t>(window_size_)) {
        ticks_.pop_front();
    }
}

double TimeWindowVWMA::get_vwma() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (ticks_.empty()) return 0.0;

    double sum_price_volume = 0.0;
    double sum_volume = 0.0;

    for (const auto& tick : ticks_) {
        sum_price_volume += tick.price * tick.volume;
        sum_volume += tick.volume;
    }

    return (sum_volume > 0) ? (sum_price_volume / sum_volume) : 0.0;
}

double TimeWindowVWMA::get_std_dev() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (ticks_.size() < 2) return 0.0;

    double vwma = get_vwma();
    if (vwma <= 0) return 0.0;

    double sum_sq_diff = 0.0;
    for (const auto& tick : ticks_) {
        double diff = tick.price - vwma;
        sum_sq_diff += diff * diff;
    }

    return std::sqrt(sum_sq_diff / ticks_.size());
}

bool TimeWindowVWMA::is_ready() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ticks_.size() >= static_cast<size_t>(window_size_);
}

size_t TimeWindowVWMA::get_tick_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ticks_.size();
}

double TimeWindowVWMA::get_vwma_slope() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (ticks_.size() < 4) return 0.0;

    // Делим тики на 2 половины
    size_t mid = ticks_.size() / 2;

    double pv_early = 0.0, v_early = 0.0;
    double pv_late = 0.0, v_late = 0.0;

    // Первая половина
    for (size_t i = 0; i < mid; ++i) {
        pv_early += ticks_[i].price * ticks_[i].volume;
        v_early += ticks_[i].volume;
    }

    // Вторая половина
    for (size_t i = mid; i < ticks_.size(); ++i) {
        pv_late += ticks_[i].price * ticks_[i].volume;
        v_late += ticks_[i].volume;
    }

    if (v_early == 0 || v_late == 0) return 0.0;

    double vwma_early = pv_early / v_early;
    double vwma_late = pv_late / v_late;

    double price_change = vwma_late - vwma_early;
    double avg_price = (vwma_early + vwma_late) / 2.0;

    if (avg_price == 0) return 0.0;

    // Возвращаем наклон в % от цены
    return price_change / avg_price;
}

double TimeWindowVWMA::get_price_deviation(double current_price) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (ticks_.empty()) return 0.0;

    // Считаем текущий VWMA
    double sum_price_volume = 0.0;
    double sum_volume = 0.0;

    for (const auto& tick : ticks_) {
        sum_price_volume += tick.price * tick.volume;
        sum_volume += tick.volume;
    }

    if (sum_volume == 0) return 0.0;

    double vwma = sum_price_volume / sum_volume;

    // Возвращаем отклонение в % от VWMA
    return (current_price - vwma) / vwma;
}