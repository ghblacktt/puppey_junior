#pragma once
#include <string>

namespace arb {

    struct ExchangeInstrumentInfo {
        double tick_size = 0.0;
        double step_size = 0.0;
        double min_order_qty = 0.0;
        double min_order_amt = 0.0;
        bool   is_valid = false;
    };

    struct ExchangePositionInfo {
        std::string symbol;
        std::string side;
        double size = 0.0;
        double signed_qty = 0.0;        // NEW: для paper mode (со знаком)
        double avg_price = 0.0;
        double unrealized_pnl = 0.0;
        bool   is_valid = false;
        bool   rate_limited = false;
    };

    struct ExchangeOrder {
        std::string order_id;
        std::string symbol;
        std::string side;
        std::string order_status;       // "New" | "Filled" | "Cancelled" | ...
        double qty = 0.0;               // executed (как в BybitOrder.qty)
        double price = 0.0;             // avg_price (как в BybitOrder.price)
    };

    struct ExchangeExecution {
        std::string order_id;
        std::string symbol;
        std::string side;
        double exec_price = 0.0;
        double exec_qty = 0.0;
        double exec_fee = 0.0;
        bool   is_valid = false;
        bool is_full_fill = true;
    };

    class IExchange {
    public:
        virtual ~IExchange() = default;

        // Совместимо с BybitApiClient::place_limit_order (7 args)
        virtual std::string place_limit_order(
                const std::string& symbol, const std::string& side,
                double price, double qty,
                double take_profit = 0.0, double stop_loss = 0.0,
                int position_idx = 0) = 0;
        virtual std::string last_ret_msg() const = 0;
        // Совместимо с BybitApiClient::place_market_order (3 args)
        virtual std::string place_market_order(
                const std::string& symbol, const std::string& side,
                double qty) = 0;

        virtual bool cancel_order(const std::string& symbol, const std::string& order_id) = 0;

        // Совместимо с BybitApiClient::get_order → ExchangeOrder
        virtual ExchangeOrder get_order(const std::string& symbol, const std::string& order_id) = 0;

        // Совместимо с BybitApiClient::get_position_info(symbol, positionIdx)
        virtual ExchangePositionInfo get_position_info(const std::string& symbol, int position_idx = 0) = 0;

        virtual ExchangeExecution get_last_execution(const std::string& symbol, const std::string& order_id) = 0;

        // Совместимо с BybitApiClient::get_instrument_info(symbol, category)
        virtual ExchangeInstrumentInfo get_instrument_info(
                const std::string& symbol, const std::string& category = "linear") = 0;

        virtual double normalize_price(const std::string& symbol, double px) = 0;
        virtual double normalize_qty(const std::string& symbol, double qty) = 0;

        // Просто поля, как в BybitApiClient (не методы!)
        virtual int last_ret_code() const = 0;
    };

} // namespace arb