#pragma once
#include "backtest_types.h"
#include "matching_engine.h"
#include "backtest_exchange.h"
#include "market_making_strategy.h"
#include "order_book.h"
#include <memory>
using namespace arb;
namespace bt {
    struct BtMarkout {
        std::string order_id;
        std::string symbol;
        std::string side;
        double fill_price = 0.0;
        uint64_t fill_ts = 0;
        double mid_100ms = 0.0;
        double mid_1s = 0.0;
        double mid_5s = 0.0;
        double mid_30s = 0.0;
        bool finalized = false;
        int stage = 0;   // 0..3 — какой markout записан
    };

    struct BacktestResult {
        size_t total_events = 0;
        size_t book_events = 0;
        size_t trade_events = 0;
        size_t fills = 0;
        size_t full_fills = 0;
        double total_volume = 0.0;
        double total_fees = 0.0;
        double realized_pnl = 0.0;
        double net_pnl = 0.0;
        double final_position_qty = 0.0;
        double final_position_avg = 0.0;
        uint64_t first_ts = 0;
        uint64_t last_ts = 0;
        std::vector<BtMarkout> markouts;
    };

    class BacktestRunner {
    public:
        BacktestRunner(const RunConfig& cfg, const InstrumentParams& inst);

        BacktestResult run(const std::vector<TimedEvent>& events);

    private:
        RunConfig cfg_;
        InstrumentParams inst_;
        MatchingEngine engine_;
        OrderBook book_;
        std::unique_ptr<MarketMakingStrategy> mm_;
        std::unique_ptr<BacktestExchange> exchange_;
    };

}  // namespace bt