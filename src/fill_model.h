#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mm {

// Config вынесен из класса, чтобы default argument в конструкторе
// FillModel видел complete type.
    struct FillModelConfig {
        bool   model_queue           = true;
        double touch_fill_volume_usd = 100.0;
    };

    class FillModel {
    public:
        using Config = FillModelConfig;

        struct Order {
            std::string order_id;
            std::string symbol;
            std::string side;          // "Buy" | "Sell"
            double   price       = 0.0;
            double   qty         = 0.0;
            double   filled_qty  = 0.0;
            double   queue_ahead = 0.0;
            double   cum_touch_volume_usd = 0.0;
            uint64_t placed_ms   = 0;
        };

        struct Fill {
            std::string order_id;
            std::string symbol;
            std::string side;
            double   exec_price = 0.0;
            double   exec_qty   = 0.0;
            uint64_t exec_ms    = 0;
        };

        struct Stats {
            uint64_t hit_touch_fills     = 0;
            uint64_t hit_through_fills   = 0;
            uint64_t hit_touch_rejects   = 0;
        };

        // Два конструктора: default + explicit от Config.
        // Config complete (глобальный) → default arg безопасен,
        // но проще не использовать default arg вообще.
        FillModel() = default;
        explicit FillModel(Config cfg) : cfg_(cfg) {}

        void set_config(Config cfg) { cfg_ = cfg; }
        const Config& config() const { return cfg_; }

        void register_order(Order order,
                            double best_bid, double best_ask,
                            double bid_size, double ask_size);

        void cancel(const std::string& order_id);

        void on_book(const std::string& symbol,
                     double best_bid, double best_ask,
                     double bid_size, double ask_size);

        void on_trade(const std::string& symbol,
                      double px, double qty,
                      bool is_buyer_maker, uint64_t now_ms);

        std::vector<Fill> drain_fills();

        std::optional<Order> get_order(const std::string& oid) const;
        std::size_t order_count() const { return orders_.size(); }
        Stats       stats() const       { return stats_; }

    private:
        Config cfg_;
        std::unordered_map<std::string, Order> orders_;
        std::vector<Fill> pending_;
        Stats stats_;
    };

} // namespace mm