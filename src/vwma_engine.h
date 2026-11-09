#pragma once
#include <deque>
#include <mutex>
#include <vector>
#include <cstdint>

class VWMAEngine {
public:
    struct Tick {
        uint64_t timestamp_ms;
        double price;
        double volume;
    };

    explicit VWMAEngine(int window_seconds = 120);

    void add_tick(double price, double volume, uint64_t timestamp_ms);

    double get_vwma() const;
    double get_vwma_slope(int seconds = 30) const;  // НОВОЕ: наклон VWMA
    double get_price_deviation(double current_price) const;  // НОВОЕ: отклонение цены от VWMA
    bool is_price_above_vwma(double current_price) const;

    std::vector<double> get_vwma_history(int points = 100) const;

private:
    int window_seconds_;
    std::deque<Tick> ticks_;
    double current_vwma_ = 0.0;

    // Инкрементальные суммы для O(1) расчёта
    double sum_pv_ = 0.0;  // Σ(price × volume)
    double sum_v_ = 0.0;   // Σ(volume)

    mutable std::mutex mutex_;

    void remove_expired_ticks(uint64_t now_ms);
    void recalculate();
};