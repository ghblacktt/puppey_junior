#pragma once
#include "iexchange.h"
#include "bybit/bybit_api_client.h"
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <vector>
#include <memory>
#include <random>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <set>

namespace arb {

    class PaperExchange : public IExchange {
    public:
        struct Config {
            double initial_balance_usd   = 10000.0;
            double maker_fee_bps         = 2.0;
            double taker_fee_bps         = 5.5;
            double taker_slippage_bps    = 3.0;     // floor for market orders
            double touch_fill_volume_usd = 2000.0;  // legacy — used only if model_queue=false
            double max_position_usd      = 60.0;

            // NEW: realistic execution models
            int    latency_ms_min        = 20;
            int    latency_ms_max        = 80;
            bool   model_queue           = true;
            bool   model_market_slippage = true;
        };

        using FillCallback = std::function<void(const ExchangeExecution&)>;

        // NEW: io_context обязателен — для отложенных филлов
        PaperExchange(BybitApiClient& public_api, boost::asio::io_context& ioc);
        PaperExchange(BybitApiClient& public_api, boost::asio::io_context& ioc, Config cfg);

        // NEW: размеры на BBO для queue model и dynamic slippage
        void on_book_update(const std::string& symbol,
                            double best_bid, double best_ask,
                            double best_bid_size = 0.0,
                            double best_ask_size = 0.0);
        void on_trade(const std::string& symbol, double price, double qty, bool is_buyer_maker);

        void set_fill_callback(FillCallback cb) { on_fill_ = std::move(cb); }

        // IExchange
        std::string place_limit_order(
                const std::string& s, const std::string& side,
                double px, double qty,
                double take_profit = 0.0, double stop_loss = 0.0,
                int position_idx = 0) override;
        std::string place_market_order(
                const std::string& s, const std::string& side, double qty) override;
        bool cancel_order(const std::string& s, const std::string& oid) override;
        ExchangeOrder get_order(const std::string& s, const std::string& oid) override;
        ExchangePositionInfo get_position_info(const std::string& s, int position_idx = 0) override;
        ExchangeExecution get_last_execution(const std::string& s, const std::string& oid) override;
        ExchangeInstrumentInfo get_instrument_info(
                const std::string& s, const std::string& category = "linear") override;
        double normalize_price(const std::string& s, double px) override;
        double normalize_qty(const std::string& s, double qty) override;
        int last_ret_code() const override { return last_ret_code_; }

        void print_snapshot() const;
        double balance() const;
        std::string last_ret_msg() const override { return last_ret_msg_; }
        void set_run_tag(const std::string& tag) { run_tag_ = tag; }
        const std::string& run_tag() const { return run_tag_; }
        int close_orphan_positions(const std::set<std::string>& active_symbols);

    private:
        struct POrder {
            std::string order_id;
            std::string symbol;
            std::string side;
            double price = 0.0;
            double qty = 0.0;
            double filled_qty = 0.0;
            double avg_fill_price = 0.0;
            double fee_paid = 0.0;
            uint64_t placed_ms = 0;
            std::string status = "New";
            bool reduce_only = false;
            double queue_ahead = 0.0;   // NEW: qty перед нами на нашем уровне
        };
        struct PPos {
            double signed_qty = 0.0;   // +long / -short
            double avg_price = 0.0;
            double realized_pnl = 0.0;
        };
        struct PExec {
            std::string order_id;
            std::string symbol;
            std::string side;
            double exec_price = 0.0;
            double exec_qty = 0.0;
            double exec_fee = 0.0;
            uint64_t exec_ms = 0;
        };
        struct BBOInfo {
            double bid = 0.0;
            double ask = 0.0;
            double bid_size = 0.0;
            double ask_size = 0.0;
        };

        BybitApiClient& public_api_;
        boost::asio::io_context& ioc_;                    // NEW
        Config cfg_;
        std::mt19937 rng_;                                // NEW
        std::vector<std::shared_ptr<boost::asio::steady_timer>> pending_timers_;  // NEW

        mutable std::mutex mtx_;
        double balance_usd_;
        double fees_paid_ = 0.0;
        double realized_pnl_ = 0.0;

        std::map<std::string, POrder> orders_;
        std::map<std::string, PPos>   positions_;
        std::map<std::string, std::vector<PExec>> executions_;
        std::map<std::string, BBOInfo> bbo_;              // extended
        std::map<std::string, double> touch_cum_volume_;  // legacy

        int last_ret_code_ = 0;
        std::string last_ret_msg_;
        std::atomic<uint64_t> next_oid_{1};
        FillCallback on_fill_;

        // Locked helpers
        void apply_fill_locked(POrder& o, double px, double qty, bool is_maker,
                               std::vector<ExchangeExecution>& out_execs);
        double estimate_market_price_locked(const std::string& sym,
                                            const std::string& side,
                                            double qty) const;

        // Unlocked (called outside mutex)
        int  random_latency_ms();
        void schedule_fill(const ExchangeExecution& exec, int latency_ms);
        void dispatch_fills(const std::vector<ExchangeExecution>& execs);
        std::string run_tag_ = "";   // пусто = legacy формат "PAPER_N"
    };

} // namespace arb