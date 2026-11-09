#pragma once
#include "iexchange.h"
#include "bybit/bybit_api_client.h"

namespace arb {

    class RealBybitAdapter : public IExchange {
    public:
        explicit RealBybitAdapter(BybitApiClient& api) : api_(api) {}

        std::string place_limit_order(
                const std::string& s, const std::string& side,
                double px, double qty,
                double tp = 0.0, double sl = 0.0, int idx = 0) override {
            return api_.place_limit_order(s, side, px, qty, tp, sl, idx);
        }

        std::string place_market_order(
                const std::string& s, const std::string& side, double qty) override {
            return api_.place_market_order(s, side, qty);
        }

        bool cancel_order(const std::string& s, const std::string& oid) override {
            return api_.cancel_order(s, oid);
        }

        ExchangeOrder get_order(const std::string& s, const std::string& oid) override {
            auto o = api_.get_order(s, oid);
            ExchangeOrder r;
            r.order_id = o.order_id;
            r.symbol = s;
            r.side = o.side;
            r.order_status = o.order_status;
            r.qty = o.qty;
            r.price = o.price;
            return r;
        }

        ExchangePositionInfo get_position_info(const std::string& s, int idx = 0) override {
            auto p = api_.get_position_info(s, idx);
            ExchangePositionInfo r;
            r.symbol = s;
            r.side = p.side;
            r.size = p.size;
            r.signed_qty = (p.side == "Buy") ? p.size : (p.side == "Sell" ? -p.size : 0.0);
            r.avg_price = p.avg_price;
            r.unrealized_pnl = p.unrealized_pnl;
            r.is_valid = p.is_valid;
            r.rate_limited = p.rate_limited;
            return r;
        }

        ExchangeExecution get_last_execution(const std::string& s, const std::string& oid) override {
            auto e = api_.get_last_execution(s, oid);
            ExchangeExecution r;
            r.order_id = oid; r.symbol = s;
            r.exec_price = e.exec_price; r.exec_qty = e.exec_qty;
            r.exec_fee = e.exec_fee; r.side = e.side;
            r.is_valid = e.is_valid;
            return r;
        }

        ExchangeInstrumentInfo get_instrument_info(
                const std::string& s, const std::string& cat = "linear") override {
            auto i = api_.get_instrument_info(s, cat);
            ExchangeInstrumentInfo r;
            r.tick_size = i.tick_size; r.step_size = i.step_size;
            r.min_order_qty = i.min_order_qty; r.min_order_amt = i.min_order_amt;
            r.is_valid = i.tick_size > 0 && i.step_size > 0;
            return r;
        }

        double normalize_price(const std::string& s, double px) override {
            return api_.normalize_price(s, px);
        }
        double normalize_qty(const std::string& s, double q) override {
            return api_.normalize_qty(s, q);
        }
        int last_ret_code() const override { return api_.last_ret_code; }
        std::string last_ret_msg() const override { return api_.last_ret_msg; }

    private:
        BybitApiClient& api_;
    };

}