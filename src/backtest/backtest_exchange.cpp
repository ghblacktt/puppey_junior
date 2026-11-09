#include "backtest_exchange.h"
#include <cmath>
#include <iostream>
#include <sstream>
#include <iomanip>
using namespace arb;
namespace bt {

    BacktestExchange::BacktestExchange(MatchingEngine& engine, OrderBook& book,
                                       const std::string& symbol)
            : engine_(engine), book_(book), symbol_(symbol) {}

    void BacktestExchange::set_instrument(double tick_size, double step_size,
                                          double min_qty, double min_notional) {
        tick_size_ = tick_size;
        step_size_ = step_size;
        min_qty_ = min_qty;
        min_notional_ = min_notional;
    }

    double BacktestExchange::apply_position(double side_sign, double price, double qty) {
        double old_qty = pos_.signed_qty;
        double new_qty = old_qty + side_sign * qty;
        double realized = 0.0;

        if (std::abs(old_qty) > 1e-9 && std::signbit(old_qty) != std::signbit(side_sign)) {
            double closed = std::min(std::abs(old_qty), qty);
            realized = (side_sign > 0)
                       ? (pos_.avg_price - price) * closed
                       : (price - pos_.avg_price) * closed;
            pos_.realized_pnl += realized;
            realized_pnl_ += realized;
        }

        if (std::abs(new_qty) > 1e-9 && std::signbit(new_qty) == std::signbit(side_sign)) {
            if (std::abs(old_qty) < 1e-9 || std::signbit(old_qty) != std::signbit(new_qty)) {
                pos_.avg_price = price;
            } else {
                pos_.avg_price = (pos_.avg_price * std::abs(old_qty) + price * qty) / std::abs(new_qty);
            }
        }
        pos_.signed_qty = new_qty;
        return realized;
    }

    void BacktestExchange::apply_fill(const BtFill& f) {
        // SAFETY: если fill выводит позицию за max_position (например, из-за qty=inf)
        // — reject, чтобы не накапливать мусор
        constexpr double kMaxPositionUsd = 60.0;   // hard cap
        double new_qty = pos_.signed_qty + ((f.side == "Buy") ? 1.0 : -1.0) * f.qty;
        double new_notional = std::abs(new_qty) * f.price;
        if (new_notional > kMaxPositionUsd * 1.5) {
            static int reject_count = 0;
            if (reject_count < 10) {
                std::cerr << "[BT-SAFETY] reject fill: pos would be "
                          << new_notional << " USD (> " << kMaxPositionUsd * 1.5 << ")\n";
                reject_count++;
            }
            return;   // ← НЕ применяем
        }

        double sign = (f.side == "Buy") ? 1.0 : -1.0;
        double realized = apply_position(sign, f.price, f.qty);
        fees_paid_ += f.fee;
        pos_.realized_pnl -= f.fee;

        last_fills_[f.order_id] = f;

        if (on_fill_) on_fill_(f);

        (void)realized;
    }

    std::string BacktestExchange::place_limit_order(
            const std::string& s, const std::string& side,
            double px, double qty,
            double /*tp*/, double /*sl*/, int /*idx*/) {
        if (s != symbol_) {
            last_ret_code_ = -1;
            last_ret_msg_ = "wrong symbol";
            return "";
        }

        double bb = book_.get_best_bid();
        double ba = book_.get_best_ask();
        if (bb <= 0.0 || ba <= 0.0) {
            last_ret_code_ = -1;
            last_ret_msg_ = "no book";
            return "";
        }

        BtOrder o;
        o.order_id = "BT_" + std::to_string(current_ts_) + "_" +
                     std::to_string(engine_.open_order_count() + 1);
        o.symbol = s;
        o.side = side;
        o.type = "Limit";
        o.price = px;
        o.qty = qty;
        o.status = "Pending";

        if (qty <= 0.0 || qty != qty) {   // qty=0 или NaN
            last_ret_code_ = -1;
            last_ret_msg_ = "invalid qty";
            return "";
        }
        if (qty * px > 500.0) {   // notional > $10k — явно баг
            static int big_qty_count = 0;
            if (big_qty_count < 10) {
                std::cerr << "[BT-SAFETY] reject huge order: qty=" << qty
                          << " px=" << px << " notional=" << (qty*px) << "\n";
                big_qty_count++;
            }
            last_ret_code_ = -1;
            last_ret_msg_ = "huge qty";
            return "";
        }

        engine_.submit_limit(o, bb, ba,
                             book_.get_best_bid_size(),
                             book_.get_best_ask_size(),
                             tick_size_,
                             current_ts_);

        last_ret_code_ = 0;
        last_ret_msg_.clear();
        return o.order_id;
    }

    std::string BacktestExchange::place_market_order(
            const std::string& s, const std::string& side, double qty) {
        if (s != symbol_) {
            last_ret_code_ = -1;
            last_ret_msg_ = "wrong symbol";
            return "";
        }

        double bb = book_.get_best_bid();
        double ba = book_.get_best_ask();
        if (bb <= 0.0 || ba <= 0.0) {
            last_ret_code_ = -1;
            last_ret_msg_ = "no book";
            return "";
        }

        BtOrder o;
        o.order_id = "BT_MKT_" + std::to_string(current_ts_) + "_" +
                     std::to_string(engine_.open_order_count() + 1);
        o.symbol = s;
        o.side = side;
        o.type = "Market";
        o.price = (side == "Buy") ? ba : bb;
        o.qty = qty;
        o.status = "Pending";

        engine_.submit_market(o, bb, ba, current_ts_,
                              [this](const BtFill& f) { apply_fill(f); });

        last_ret_code_ = 0;
        last_ret_msg_.clear();
        return o.order_id;
    }

    bool BacktestExchange::cancel_order(const std::string& s, const std::string& oid) {
        if (s != symbol_) return false;
        engine_.cancel(oid, current_ts_);
        return true;
    }

    ExchangeOrder BacktestExchange::get_order(const std::string& /*s*/, const std::string& oid) {
        ExchangeOrder r;
        auto o = engine_.get_order(oid);
        if (!o) return r;
        r.order_id = o->order_id;
        r.order_status = o->status;
        r.qty = o->filled_qty;
        r.price = o->avg_fill_price;
        return r;
    }

    ExchangePositionInfo BacktestExchange::get_position_info(
            const std::string& s, int /*idx*/) {
        ExchangePositionInfo r;
        r.symbol = s;
        r.is_valid = true;
        r.rate_limited = false;       // ← КРИТИЧНО: без этого refresh_position падает
        r.signed_qty = pos_.signed_qty;
        r.size = std::abs(pos_.signed_qty);
        r.side = (pos_.signed_qty > 0) ? "Buy" : ((pos_.signed_qty < 0) ? "Sell" : "");
        r.avg_price = pos_.avg_price;

        // Сброс кода последнего вызова — get_position_info успешен
        last_ret_code_ = 0;
        last_ret_msg_.clear();

        double mid = book_.get_mid_price();
        if (mid > 0 && std::abs(pos_.signed_qty) > 1e-9) {
            if (pos_.signed_qty > 0) {
                r.unrealized_pnl = (mid - pos_.avg_price) * pos_.signed_qty;
            } else {
                r.unrealized_pnl = (pos_.avg_price - mid) * (-pos_.signed_qty);
            }
        }
        return r;
    }

    ExchangeExecution BacktestExchange::get_last_execution(
            const std::string& /*s*/, const std::string& oid) {
        ExchangeExecution r;
        auto it = last_fills_.find(oid);
        if (it == last_fills_.end()) return r;
        r.order_id = it->second.order_id;
        r.symbol = it->second.symbol;
        r.side = it->second.side;
        r.exec_price = it->second.price;
        r.exec_qty = it->second.qty;
        r.exec_fee = it->second.fee;
        r.is_valid = true;
        return r;
    }

    ExchangeInstrumentInfo BacktestExchange::get_instrument_info(
            const std::string& /*s*/, const std::string& /*cat*/) {
        ExchangeInstrumentInfo r;
        r.tick_size = tick_size_;
        r.step_size = step_size_;
        r.min_order_qty = min_qty_;
        r.min_order_amt = min_notional_;
        r.is_valid = true;
        return r;
    }

    double BacktestExchange::normalize_price(const std::string& /*s*/, double px) {
        if (tick_size_ <= 0) return px;
        return std::round(px / tick_size_) * tick_size_;
    }

    double BacktestExchange::normalize_qty(const std::string& /*s*/, double qty) {
        if (step_size_ <= 0) return std::floor(qty);
        double n = std::floor(qty / step_size_) * step_size_;
        if (n < min_qty_) n = min_qty_;
        return n;
    }

    void BacktestExchange::on_trade(const TradeEvent& trade) {
        engine_.on_trade(trade, current_ts_,
                         [this](const BtFill& f) { apply_fill(f); });
    }

    void BacktestExchange::process_pending_orders() {
        engine_.process_pending(current_ts_);
    }

}  // namespace bt