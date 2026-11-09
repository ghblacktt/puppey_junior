#include "bar_aggregator.h"
#include "iostream"

BarAggregator::BarAggregator(uint64_t duration_ms, BarCallback callback)
        : duration_ms_(duration_ms), callback_(callback) {}

void BarAggregator::add_historical_bar(const Bar& bar) {
    closed_bars_.push_back(bar);
    if (closed_bars_.size() > max_bars_) {
        closed_bars_.pop_front();
    }
}

std::optional<Bar> BarAggregator::add_tick(double price, double volume, uint64_t ts_ms) {
    uint64_t bar_ts = (ts_ms / 60'000) * 60'000;   // 1-minute buckets

    if (current_bar_.timestamp != bar_ts) {
        // новый бар начинается
        std::optional<Bar> closed;
        if (current_bar_.timestamp != 0) {
            current_bar_.close = last_price_;
            closed = current_bar_;   // предыдущий закрылся
        }
        current_bar_ = Bar{};
        current_bar_.timestamp = bar_ts;
        current_bar_.open = price;
        current_bar_.high = price;
        current_bar_.low  = price;
        current_bar_.volume = 0;
        last_price_ = price;
        current_bar_.volume += volume;
        return closed;
    }

    current_bar_.high = std::max(current_bar_.high, price);
    current_bar_.low  = std::min(current_bar_.low,  price);
    current_bar_.volume += volume;
    last_price_ = price;
    return std::nullopt;
}



void BarAggregator::close_bar() {
    if (callback_) {
        callback_(current_bar_);
    }
}