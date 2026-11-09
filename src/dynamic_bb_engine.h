#pragma once
#include <deque>
#include <vector>
#include <mutex>
#include <cstdint>
#include <cmath>

class DynamicBBEngine {
public:
    struct Tick {
        uint64_t timestamp;
        double price;
        double volume;
    };

    struct Bands {
        double alma = 0.0;
        double std_dev = 0.0;
        double dynamic_multiplier = 2.0;
        double band1_upper = 0.0, band1_lower = 0.0;  // multiplier 1
        double band2_upper = 0.0, band2_lower = 0.0;  // multiplier 1.5
        double band3_upper = 0.0, band3_lower = 0.0;  // multiplier 2.5
        double band4_upper = 0.0, band4_lower = 0.0;  // multiplier 4
        double volatility = 0.0;  // заменяет ADX
    };

    DynamicBBEngine(
            int alma_period = 200,      // было 1000
            double alma_offset = 0.85,
            double alma_sigma = 6.0,
            int bb_period = 200,        // было 1000
            double base_multiplier = 2.0,
            int vol_lookback = 80,
            double percentile_coverage = 90.0
    );
    // Добавить тик
    void add_tick(double price, double volume, uint64_t timestamp_ms);

    void add_price(double price, uint64_t timestamp);   // новый метод для добавления исторических цен

    // Получить текущие bands
    Bands get_bands() const;

    // Достаточно ли данных
    bool is_ready() const;

    // Количество тиков
    size_t get_tick_count() const;

private:
    int alma_period_, bb_period_, vol_lookback_;
    double alma_offset_, alma_sigma_, base_multiplier_, percentile_coverage_;
    std::vector<double> band_multipliers_ = {1.0, 1.5, 2.5, 4.0};

    std::deque<Tick> ticks_;
    std::deque<double> volatility_history_;
    mutable std::mutex mutex_;

    static constexpr size_t MAX_TICKS = 100000;
    static constexpr size_t MAX_VOL_HISTORY = 10000;

    double calculate_alma() const;
    double calculate_std_dev(double mean) const;
    double calculate_volatility() const;
    double calculate_dynamic_multiplier(double current_vol) const;
    double percentile(std::vector<double> values, double p) const;
};