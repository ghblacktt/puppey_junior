#pragma once
#include <deque>
#include <mutex>
#include <cstdint>

class TimeWindowVWMA {
public:
    struct Tick {
        double price;
        double volume;
    };

    explicit TimeWindowVWMA(int window_size = 200);

    void add_tick(double price, double volume, uint64_t timestamp_ms);
    double get_vwma() const;
    double get_std_dev() const;
    bool is_ready() const;
    size_t get_tick_count() const;

    double get_vwma_slope() const;
    double get_price_deviation(double current_price) const;

private:
    int window_size_;
    std::deque<Tick> ticks_;
    mutable std::mutex mutex_;
};