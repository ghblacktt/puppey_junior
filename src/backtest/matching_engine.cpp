#include "matching_engine.h"
#include <algorithm>
#include <iostream>
#include <cmath>

namespace bt {

    MatchingEngine::MatchingEngine() {
        rng_.seed(std::random_device{}());
    }

    uint64_t MatchingEngine::random_latency() {
        if (latency_.jitter_ms == 0) return latency_.mean_ms;
        std::uniform_int_distribution<int64_t> dist(
                (int64_t)latency_.mean_ms - (int64_t)latency_.jitter_ms,
                (int64_t)latency_.mean_ms + (int64_t)latency_.jitter_ms);
        int64_t v = dist(rng_);
        if (v < 0) v = 0;
        return (uint64_t)v;
    }

    void MatchingEngine::submit_limit(const BtOrder& o,
                                      double best_bid, double best_ask,
                                      double bid_size, double ask_size,
                                      uint64_t now_ts) {
        BtOrder ord = o;
        ord.submit_ts = now_ts;
        ord.status = "Pending";

        // Вычисляем queue_ahead (при постановке в очередь через latency):
        //   Buy:  если price < best_bid -> 0 (новая цена)
        //         если price == best_bid -> bid_size (все впереди)
        //         если price > best_bid (cross) -> 0 (taker-like fill)
        //   Sell: симметрично
        bool is_buy = (ord.side == "Buy");
// Queue model по глубине от best:
// - на best_bid: весь объём впереди
// - 1 tick от best: 90% от best_bid_size (конкуренты ставят там же)
// - 2 tick: 50%
// - 3+ tick: 20%
// Queue model: чем глубже уровень, тем больше конкурентов
        double tick_est = is_buy ? best_bid * 1e-5 : best_ask * 1e-5;
        if (tick_est <= 0) tick_est = 1e-6;

        double price_gap = is_buy ? (best_bid - ord.price) : (ord.price - best_ask);
        double best_size = is_buy ? bid_size : ask_size;

        if (price_gap < 0.0) {
            ord.queue_ahead = 0.0;
        } else {
            double gap_ticks = price_gap / tick_est;
            // Чем глубже, тем больше впереди. Минимум best_size.
            ord.queue_ahead = best_size * std::max(1.0, gap_ticks);
        }

        ord.placed_ts = now_ts + random_latency();

        orders_[ord.order_id] = ord;
        pending_order_ids_.push_back(ord.order_id);
    }

    void MatchingEngine::submit_market(const BtOrder& o,
                                       double best_bid, double best_ask,
                                       uint64_t now_ts,
                                       std::function<void(const BtFill&)> on_fill) {
        if (!on_fill) return;

        BtOrder ord = o;
        ord.submit_ts = now_ts;
        ord.placed_ts = now_ts + random_latency();

        bool is_buy = (ord.side == "Buy");
        double fill_price = is_buy ? best_ask : best_bid;
        if (fill_price <= 0.0) {
            ord.status = "Cancelled";
            orders_[ord.order_id] = ord;
            return;
        }

        double fee = fill_price * ord.qty * taker_fee_bps_ / 10000.0;

        ord.filled_qty = ord.qty;
        ord.avg_fill_price = fill_price;
        ord.status = "Filled";

        BtFill f;
        f.order_id = ord.order_id;
        f.symbol = ord.symbol;
        f.side = ord.side;
        f.price = fill_price;
        f.qty = ord.qty;
        f.fee = fee;
        f.timestamp_ms = ord.placed_ts;
        f.is_maker = false;
        f.is_full = true;

        orders_[ord.order_id] = ord;
        on_fill(f);
    }

    void MatchingEngine::cancel(const std::string& order_id, uint64_t now_ts) {
        auto it = orders_.find(order_id);
        if (it == orders_.end()) return;
        if (it->second.status == "Filled" || it->second.status == "Cancelled") return;
        it->second.status = "Cancelled";
    }

    void MatchingEngine::process_pending(uint64_t now_ts,
                                         std::function<void(const std::string&)> on_activated) {
        std::vector<std::string> still_pending;
        still_pending.reserve(pending_order_ids_.size());

        for (const auto& oid : pending_order_ids_) {
            auto it = orders_.find(oid);
            if (it == orders_.end()) continue;
            BtOrder& o = it->second;

            if (o.status == "Cancelled") continue;

            if (now_ts >= o.placed_ts) {
                o.status = "Open";
                if (on_activated) on_activated(oid);
            } else {
                still_pending.push_back(oid);
            }
        }
        pending_order_ids_ = std::move(still_pending);
    }

    void MatchingEngine::on_trade(const TradeEvent& trade, uint64_t now_ts,
                                  std::function<void(const BtFill&)> on_fill) {
        if (!on_fill) return;
// ДИАГНОСТИКА
        static int64_t trades_processed = 0;
        static int64_t trades_with_orders = 0;
        static int64_t fills_count = 0;
        trades_processed++;
        if (trades_processed <= 10 || trades_processed % 10000 == 0) {
            std::cout << "[ENG-TRADE] #" << trades_processed
                      << " px=" << trade.price << " vol=" << trade.volume
                      << " orders_in_book=" << orders_.size() << "\n";
        }
        double trade_remaining = trade.volume;   // ← ОДИН РАЗ, до цикла
        if (trade_remaining <= 1e-9) return;     // пустой trade — выходим

        for (auto& [oid, o] : orders_) {
            if (trade_remaining <= 1e-9) break;
            if (o.symbol != trade.symbol) continue;
            if (o.status != "Open") continue;
            if (o.filled_qty >= o.qty - 1e-9) continue;

            bool is_buy = (o.side == "Buy");

            // Buy — fill только если trade цена <= наш bid (рынок пробил вниз)
            // Sell — fill только если trade цена >= наш ask (рынок пробил вверх)
            // Для maker: trade должен "съесть" наш уровень
            if (orders_.size() > 0) trades_with_orders++;
// Trade должен ТОЧНО пробить нашу цену, не просто коснуться
            bool hit = false;
            if (is_buy) {
                hit = (trade.price < o.price);   // строго ниже
            } else {
                hit = (trade.price > o.price);   // строго выше
            }
            if (!hit) continue;

            // Сколько trade может съесть на нашем уровне
            double trade_remaining = trade.volume;

            // 1) Сначала съедаем queue_ahead
            if (o.queue_ahead > 0.0) {
                double consumed = std::min(o.queue_ahead, trade_remaining);
                o.queue_ahead -= consumed;
                trade_remaining -= consumed;
                if (trade_remaining <= 1e-9) continue;
            }

            // 2) Теперь наш ордер получает fill
            double our_remaining = o.qty - o.filled_qty;
            double fill_qty = std::min(our_remaining, trade_remaining);
            if (fill_qty <= 1e-9) continue;

            double fee = o.price * fill_qty * maker_fee_bps_ / 10000.0;

            o.filled_qty += fill_qty;
            double old_notional = o.avg_fill_price * (o.filled_qty - fill_qty);
            o.avg_fill_price = (old_notional + o.price * fill_qty) / o.filled_qty;

            bool is_full = (o.filled_qty >= o.qty - 1e-9);
            if (is_full) o.status = "Filled";

            BtFill f;
            f.order_id = o.order_id;
            f.symbol = o.symbol;
            f.side = o.side;
            f.price = o.price;
            f.qty = fill_qty;
            f.fee = fee;
            f.timestamp_ms = now_ts;
            f.is_maker = true;
            f.is_full = is_full;

            on_fill(f);

            fills_count++;
            if (fills_count <= 5) {
                std::cout << "[ENG-FILL-" << fills_count << "] "
                          << o.order_id << " " << o.side
                          << " px=" << o.price << " qty=" << fill_qty << "\n";
            }
        }
    }

    const BtOrder* MatchingEngine::get_order(const std::string& order_id) const {
        auto it = orders_.find(order_id);
        if (it == orders_.end()) return nullptr;
        return &it->second;
    }

    std::vector<BtOrder> MatchingEngine::get_open_orders() const {
        std::vector<BtOrder> out;
        for (const auto& [oid, o] : orders_) {
            if (o.status == "Open" || o.status == "Pending") {
                out.push_back(o);
            }
        }
        return out;
    }

    size_t MatchingEngine::open_order_count() const {
        size_t n = 0;
        for (const auto& [oid, o] : orders_) {
            if (o.status == "Open" || o.status == "Pending") ++n;
        }
        return n;
    }

    void MatchingEngine::prune_completed(uint64_t now_ts, uint64_t max_age_ms) {
        std::vector<std::string> to_erase;
        for (auto& [oid, o] : orders_) {
            if (o.status == "Filled" || o.status == "Cancelled") {
                if (now_ts > o.placed_ts && now_ts - o.placed_ts > max_age_ms) {
                    to_erase.push_back(oid);
                }
            }
        }
        for (const auto& oid : to_erase) orders_.erase(oid);
    }

}  // namespace bt