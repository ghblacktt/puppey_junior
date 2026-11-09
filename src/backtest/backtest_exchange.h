#pragma once
#include "iexchange.h"
#include "matching_engine.h"
#include "order_book.h"
#include <unordered_map>
#include <mutex>
#include <atomic>
using namespace arb;
namespace bt {

    class BacktestExchange : public IExchange {
    public:
        BacktestExchange(MatchingEngine& engine, OrderBook& book,
                         const std::string& symbol);

        // IExchange: place/cancel идут в matching engine
        std::string place_limit_order(
                const std::string& s, const std::string& side,
                double px, double qty,
                double take_profit = 0.0, double stop_loss = 0.0,
                int position_idx = 0) override;

        std::string place_market_order(
                const std::string& s, const std::string& side, double qty) override;

        bool cancel_order(const std::string& s, const std::string& oid) override;

        // Query
        ExchangeOrder get_order(const std::string& s, const std::string& oid) override;
        ExchangePositionInfo get_position_info(const std::string& s, int position_idx = 0) override;
        ExchangeExecution get_last_execution(const std::string& s, const std::string& oid) override;
        ExchangeInstrumentInfo get_instrument_info(
                const std::string& s, const std::string& category = "linear") override;

        double normalize_price(const std::string& s, double px) override;
        double normalize_qty(const std::string& s, double qty) override;
        int last_ret_code() const override { return last_ret_code_; }
        std::string last_ret_msg() const override { return last_ret_msg_; }

        // Backtest-specific: продвинуть "время"
        void set_current_ts(uint64_t ts) { current_ts_ = ts; }

        // Fill callback (устанавливается runner'ом)
        using FillCallback = std::function<void(const BtFill&)>;
        void set_fill_callback(FillCallback cb) { on_fill_ = std::move(cb); }

        // Обработка событий от runner
        void on_trade(const TradeEvent& trade);
        void process_pending_orders();

        // Статистика
        double realized_pnl() const { return realized_pnl_; }
        double fees_paid() const { return fees_paid_; }

        void set_instrument(double tick_size, double step_size,
                            double min_qty, double min_notional);

    private:
        MatchingEngine& engine_;
        OrderBook& book_;
        std::string symbol_;
        uint64_t current_ts_ = 0;
        FillCallback on_fill_;

        // Position tracking
        struct Position {
            double signed_qty = 0.0;
            double avg_price = 0.0;
            double realized_pnl = 0.0;
        };
        Position pos_;
        double realized_pnl_ = 0.0;
        double fees_paid_ = 0.0;

        // Last execution (для get_last_execution)
        std::unordered_map<std::string, BtFill> last_fills_;

        // Instrument
        double tick_size_ = 0.0001;
        double step_size_ = 0.1;
        double min_qty_ = 0.0;
        double min_notional_ = 0.0;

        int last_ret_code_ = 0;
        std::string last_ret_msg_;

        void apply_fill(const BtFill& f);
        double apply_position(double side_sign, double price, double qty);
    };

}  // namespace bt