#include "vwma_engine.h"
#include <algorithm>

VWMAEngine::VWMAEngine(int window_seconds) : window_seconds_(window_seconds) {}

void VWMAEngine::add_tick(double price, double volume, uint64_t timestamp_ms) {
    std::scoped_lock lock(mutex_);

    Tick tick{timestamp_ms, price, volume};
    ticks_.push_back(tick);

    // Инкрементальное обновление сумм
    sum_pv_ += price * volume;
    sum_v_ += volume;

    // Удаляем старые тики и корректируем суммы
    remove_expired_ticks(timestamp_ms);

    // Пересчитываем VWMA
    if (sum_v_ > 0) {
        current_vwma_ = sum_pv_ / sum_v_;
    }
}

void VWMAEngine::remove_expired_ticks(uint64_t now_ms) {
    uint64_t cutoff = now_ms - (window_seconds_ * 1000);

    while (!ticks_.empty() && ticks_.front().timestamp_ms < cutoff) {
        sum_pv_ -= ticks_.front().price * ticks_.front().volume;
        sum_v_ -= ticks_.front().volume;
        ticks_.pop_front();
    }
}

void VWMAEngine::recalculate() {
    sum_pv_ = 0.0;
    sum_v_ = 0.0;

    for (const auto& tick : ticks_) {
        sum_pv_ += tick.price * tick.volume;
        sum_v_ += tick.volume;
    }

    if (sum_v_ > 0) {
        current_vwma_ = sum_pv_ / sum_v_;
    }
}

auto VWMAEngine::get_vwma() const -> double {
    std::scoped_lock lock(mutex_);
    return current_vwma_;
}

auto VWMAEngine::get_vwma_slope(int seconds) const -> double {
    std::scoped_lock lock(mutex_);

    if (ticks_.size() < 2) return 0.0;

    uint64_t now = ticks_.back().timestamp_ms;
    uint64_t cutoff = now - (seconds * 1000);

    // Считаем VWMA в начале и конце окна
    double pv_early = 0.0, v_early = 0.0;
    double pv_late = 0.0, v_late = 0.0;

    // Разбиваем окно на 2 половины
    uint64_t mid_time = cutoff + (seconds * 500);

    for (const auto& tick : ticks_) {
        if (tick.timestamp_ms < cutoff) continue;

        double pv = tick.price * tick.volume;

        if (tick.timestamp_ms < mid_time) {
            pv_early += pv;
            v_early += tick.volume;
        } else {
            pv_late += pv;
            v_late += tick.volume;
        }
    }

    if (v_early == 0 || v_late == 0) return 0.0;

    double vwma_early = pv_early / v_early;
    double vwma_late = pv_late / v_late;

    // Возвращаем наклон в % в секунду
    double price_change = vwma_late - vwma_early;
    double avg_price = (vwma_early + vwma_late) / 2.0;

    if (avg_price == 0) return 0.0;

    return (price_change / avg_price) / seconds;
}

auto VWMAEngine::get_price_deviation(double current_price) const -> double {
    std::scoped_lock lock(mutex_);

    if (current_vwma_ <= 0) return 0.0;

    return (current_price - current_vwma_) / current_vwma_;
}

auto VWMAEngine::is_price_above_vwma(double current_price) const -> bool {
    std::scoped_lock lock(mutex_);
    return current_price > current_vwma_;
}

auto VWMAEngine::get_vwma_history(int points) const -> std::vector<double> {
    std::scoped_lock lock(mutex_);

    std::vector<double> history;
    if (ticks_.size() < 2) return history;

    // Разбиваем историю на `points` временных интервалов
    uint64_t start_time = ticks_.front().timestamp_ms;
    uint64_t end_time = ticks_.back().timestamp_ms;
    uint64_t duration = end_time - start_time;

    if (duration == 0) return history;

    uint64_t step = duration / points;
    if (step == 0) step = 1;

    for (int i = 0; i < points; ++i) {
        uint64_t target_time = start_time + i * step;

        // Находим ближайший тик к target_time
        double pv = 0.0, v = 0.0;
        int count = 0;

        for (const auto& tick : ticks_) {
            if (tick.timestamp_ms >= target_time && tick.timestamp_ms < target_time + step) {
                pv += tick.price * tick.volume;
                v += tick.volume;
                count++;
            }
        }

        if (v > 0) {
            history.push_back(pv / v);
        }
    }

    return history;
}