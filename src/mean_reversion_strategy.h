#pragma once
#include <string>
#include <memory>
#include "order_book.h"
#include "cluster_engine.h"
#include "time_window_vwma.h"
#include "dynamic_bb_engine.h"
#include "bar_aggregator.h"
#include "regime_filter.h"

class MeanReversionStrategy {
    // Коллбэк для агрегатора
    void on_bar_closed(const Bar& bar);

public:
    enum Signal { NONE, LONG, SHORT };
    virtual void update(double price, double volume, uint64_t timestamp);
    struct SignalDetails {
        Signal signal = NONE;
        double entry_price = 0.0;
        double stop_loss = 0.0;
        double take_profit_1 = 0.0;
        double take_profit_2 = 0.0;
        double confidence = 0.0;
        std::string reason;
        bool at_extreme = false;
        bool absorption_present = false;
        bool delta_weakening = false;
        bool in_range = false;
    };

    enum class Direction {
        NONE,
        BOUNCING_UP,    // Отскок вверх от нижней границы
        BOUNCING_DOWN   // Отскок вниз от верхней границы
    };

    Direction get_bounce_direction() const;

    MeanReversionStrategy(OrderBook& orderbook, ClusterEngine& cluster,
                          TimeWindowVWMA& vwma, DynamicBBEngine& bb);

    double last_price_ = 0.0;

    Signal get_signal() const;
    double calculate_atr();
    SignalDetails get_signal_details() const;
    void print_state() const;

    // ПУБЛИЧНЫЕ методы для логирования (перенесены из private)
    bool is_price_at_extreme() const;
    bool is_absorption_zone_present() const;
    bool is_delta_weakening() const;

    OrderBook& orderbook_;
    ClusterEngine& cluster_;
    TimeWindowVWMA& vwma_;
    DynamicBBEngine& bb_;
    std::unique_ptr<BarAggregator> bar_aggregator_;
    std::unique_ptr<RegimeFilter> regime_filter_;

    Signal current_signal_ = NONE;
    SignalDetails details_;
    uint64_t last_signal_timestamp_ = 0;
    static constexpr uint64_t SIGNAL_COOLDOWN_MS = 30000;

    double calculate_confidence() const;
    virtual void generate_signal();
    void generate_extreme_signal(double current_price, double vwma, const DynamicBBEngine::Bands& bands);
    void generate_range_signal(double current_price, double vwma, const DynamicBBEngine::Bands& bands);
    virtual void generate_bounce_signal(
            double current_price, double vwma, const DynamicBBEngine::Bands& bands, bool is_long, bool absorption_present
    );
};