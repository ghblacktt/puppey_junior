#pragma once
#include "backtest_types.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <random>

namespace bt {

    struct BtOrder {
        std::string order_id;
        std::string symbol;
        std::string side;          // "Buy" | "Sell"
        std::string type;          // "Limit" | "Market"
        double price = 0.0;
        double qty = 0.0;
        double filled_qty = 0.0;
        double avg_fill_price = 0.0;
        uint64_t submit_ts = 0;    // когда вызвали submit
        uint64_t placed_ts = 0;    // когда реально встал в очередь
        double queue_ahead = 0.0;  // units ahead на нашем уровне
        std::string status;        // "Pending" | "Open" | "Filled" | "Cancelled"
    };

    struct BtFill {
        std::string order_id;
        std::string symbol;
        std::string side;
        double price = 0.0;
        double qty = 0.0;
        double fee = 0.0;
        uint64_t timestamp_ms = 0;
        bool is_maker = false;
        bool is_full = false;      // полностью ли ордер закрыт
    };

    class MatchingEngine {
    public:
        struct Latency {
            uint64_t mean_ms   = 50;
            uint64_t jitter_ms = 30;
        };

        MatchingEngine();
        ~MatchingEngine() = default;

        void set_latency(const Latency& l) { latency_ = l; }
        void set_fees(double maker_bps, double taker_bps) {
            maker_fee_bps_ = maker_bps;
            taker_fee_bps_ = taker_bps;
        }

        // Поставить лимитный ордер. Попадёт в очередь через latency_.
        // queue_ahead вычисляется из текущего BBO:
        //   - если цена хуже best (bid ниже / ask выше) -> 0
        //   - если на best -> размер на этом уровне
        void submit_limit(const BtOrder& o,
                          double best_bid, double best_ask,
                          double bid_size, double ask_size,
                          double tick_size_real,
                          uint64_t now_ts);

        // Market order — fills сразу по BBO (с проскальзыванием)
        // fills отправляются через on_fill callback
        void submit_market(const BtOrder& o,
                           double best_bid, double best_ask,
                           uint64_t now_ts,
                           std::function<void(const BtFill&)> on_fill);

        // Отмена
        void cancel(const std::string& order_id, uint64_t now_ts);

        // Активирует pending orders (submit + latency прошли). Вызывать на каждом event.
        void process_pending(uint64_t now_ts,
                             std::function<void(const std::string&)> on_activated = nullptr);

        // Обработка trade — может дать fills нашим лимитным ордерам
        void on_trade(const TradeEvent& trade, uint64_t now_ts,
                      std::function<void(const BtFill&)> on_fill);

        // Query
        const BtOrder* get_order(const std::string& order_id) const;
        std::vector<BtOrder> get_open_orders() const;
        size_t open_order_count() const;

        // Очистка завершённых (Filled/Cancelled) старше N ms
        void prune_completed(uint64_t now_ts, uint64_t max_age_ms = 60000);

    private:
        Latency latency_;
        double maker_fee_bps_ = 2.0;
        double taker_fee_bps_ = 5.5;

        std::unordered_map<std::string, BtOrder> orders_;
        std::vector<std::string> pending_order_ids_;   // ждут latency
        std::mt19937 rng_;

        uint64_t random_latency();
        uint64_t next_order_seq_ = 1;
    };

}  // namespace bt