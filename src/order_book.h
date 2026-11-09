#pragma once
#include <map>
#include <string>
#include <string_view>
#include <mutex>
#include <simdjson.h>
#include <iostream>
#include <iomanip>
#include <utility>
#include "bybit/bybit_api_client.h"

struct BookAnalysis {
    double imbalance = 0.0;          // -1 (сильный ask) … +1 (сильный bid)
    double weighted_mid = 0.0;
    double bid_pressure = 0.0;       // top_of_book_size / avg_level_size
    double ask_pressure = 0.0;       // top_of_book_size / avg_level_size
    bool   has_bid_wall = false;
    bool   has_ask_wall = false;
    double wall_distance_pct = 100.0;
    double liquidity_quality = 0.0;
    double support_level = 0.0;
    double resistance_level = 0.0;
    bool   favorable_for_long = false;
    bool   favorable_for_short = false;
};

class OrderBook {
public:
    struct BBO {
        double bid = 0.0;
        double ask = 0.0;
        double bid_size = 0.0;
        double ask_size = 0.0;
        bool is_valid() const { return bid > 0.0 && ask > 0.0; }
    };

    OrderBook() = default;

    // Основной метод: принимает распарсенный JSON и обновляет стакан
    void update(const simdjson::dom::element& data, std::string_view type);

    // Геттеры BBO
    double get_best_bid()      const;
    double get_best_ask()      const;
    double get_mid_price()     const;
    double get_best_bid_size() const;
    double get_best_ask_size() const;

    // Атомарный снимок BBO (bid/ask/bid_size/ask_size из одного lock'а)
    BBO get_bbo() const;

    void print_top_levels() const;

    double get_bid_ask_imbalance(int levels = 5) const;
    double get_imbalance_at_levels(int levels) const;
    double get_weighted_mid_price() const;
    double get_book_slope(int levels = 5) const;
    double get_liquidity_ratio(double price_range_pct = 0.001) const;

    BookAnalysis analyze_book() const;
    std::pair<double, double> get_walls(double threshold_multiplier = 5.0) const;

    void initialize(const BybitApiClient::OrderBookSnapshot& snap);

    enum Side { BID = 0, ASK = 1 };

    uint64_t get_last_update_ms() const;
    void set_last_update_ms(uint64_t ts) {
        std::lock_guard<std::mutex> lock(mutex_);
        last_update_ms_ = ts;
    }
    double get_microprice() const;
    double get_ofi_sum(int n_samples = 100) const;

private:
    std::map<double, double, std::greater<>> bids_;
    std::map<double, double, std::less<>>    asks_;
    struct BestQuote {
        double bid = 0.0, ask = 0.0;
        double bid_size = 0.0, ask_size = 0.0;
    };
    BestQuote prev_quote_;
    std::deque<double> ofi_history_;
    double compute_ofi_step(const BestQuote& prev, const BestQuote& cur) const;
    uint64_t last_seq_       = 0;
    uint64_t last_update_ms_ = 0;

    mutable std::mutex mutex_;
};