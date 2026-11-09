#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace bt {

    struct BookLevel {
        double price = 0.0;
        double size = 0.0;
    };

    struct BookSnapshot {
        std::string symbol;
        uint64_t    timestamp_ms = 0;
        uint64_t    update_id = 0;
        std::string update_type;   // "snapshot" | "delta"
        std::vector<BookLevel> bids;
        std::vector<BookLevel> asks;
    };

    struct TradeEvent {
        std::string symbol;
        uint64_t    timestamp_ms = 0;
        double      price = 0.0;
        double      volume = 0.0;
        bool        is_buyer_maker = false;
    };

    struct TimedEvent {
        enum Type { BOOK, TRADE };
        Type type;
        uint64_t    timestamp_ms = 0;
        // Один из двух (по type)
        BookSnapshot book;
        TradeEvent   trade;
    };

    struct InstrumentParams {
        double tick_size = 0.0001;
        double step_size = 0.1;
        double min_order_qty = 0.0;
        double min_order_amt = 0.0;
    };

// Параметры для backtest запуска
    struct RunConfig {
        std::string symbol;
        uint64_t    from_ts_ms = 0;
        uint64_t    to_ts_ms   = 0;

        // MM параметры
        double  base_spread_bps = 12.0;
        double  qty_usd = 5.5;
        double  max_position_usd = 50.0;
        double  min_hs_bps = 10.0;
        double  momentum_weight = 1.0;
        int     max_hold_sec = 45;
        double  maker_fee_bps = 2.0;

        // Симуляция
        uint64_t latency_ms = 50;     // средняя задержка
        double   latency_jitter_ms = 30;  // jitter ±30ms
    };

}  // namespace bt