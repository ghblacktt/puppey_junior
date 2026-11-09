#pragma once
#include <string>
#include <cstdint>

struct ActiveTrade {
    int tracking_id = 0;
    int signal_id = 0;
    std::string symbol;
    std::string signal_type;
    double entry_price = 0.0;
    double stop_loss = 0.0;
    double take_profit_1 = 0.0;
    double take_profit_2 = 0.0;
    uint64_t entry_time = 0;
    double confidence = 0.0;
    double volatility = 0.0;
    double absorption_volume = 0.0;
    double position_size_usdt = 0.0;
    double liquidity_depth = 0.0;
    double liquidity_volume_60s = 0.0;
    double liquidity_spread = 0.0;
    double liquidity_avg_trade_size = 0.0;
    double initial_sl_distance = 0.0;
    double max_favorable = 0.0;
    double max_adverse = 0.0;
    double trailing_stop_updates = 0.0;
    std::string real_order_id;
    std::string exit_order_id;
    double time_to_fill_ms = 0.0;
};

struct LiquidityInfo {
    bool is_valid = false;
    double spread_pct = 0.0;
    double depth_usdt = 0.0;
    double volume_60s = 0.0;
    double avg_trade_size = 0.0;
};