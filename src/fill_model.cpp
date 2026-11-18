#include "fill_model.h"
#include <algorithm>
#include <cmath>

namespace mm {

    void FillModel::register_order(Order o,
                                   double bb, double ba,
                                   double bs, double as) {
        const bool is_buy = (o.side == "Buy");

        // queue_ahead: сколько объёма впереди нас на нашей цене
        if (is_buy) {
            if      (std::abs(o.price - bb) < 1e-12) o.queue_ahead = bs * 0.02;
            else if (o.price > bb)                   o.queue_ahead = 0.0;
            else                                     o.queue_ahead = std::max(bs * 1.0, 1.0);
        } else {
            if      (std::abs(o.price - ba) < 1e-12) o.queue_ahead = as * 0.02;
            else if (o.price < ba)                   o.queue_ahead = 0.0;
            else                                     o.queue_ahead = std::max(as * 1.0, 1.0);
        }

        o.filled_qty             = 0.0;
        o.cum_touch_volume_usd   = 0.0;
        orders_[o.order_id]      = std::move(o);
    }

    void FillModel::cancel(const std::string& oid) {
        orders_.erase(oid);
    }

    void FillModel::on_book(const std::string& /*sym*/,
                            double /*bb*/, double /*ba*/,
                            double /*bs*/, double /*as*/) {
        // Пока не используется: queue_ahead фиксируется при register_order.
        // TODO (Sprint 3c): динамический queue_ahead при амендменте.
    }

    void FillModel::on_trade(const std::string& sym, double px, double qty,
                             bool is_buyer_maker, uint64_t now) {
        if (qty <= 0.0) return;

        for (auto& [oid, o] : orders_) {
            if (o.symbol != sym) continue;
            if (o.filled_qty >= o.qty - 1e-12) continue;

            const bool is_buy = (o.side == "Buy");
            if (is_buyer_maker != is_buy) continue;   // aggressor на нашей стороне

            auto emit = [&](double fq) {
                if (fq <= 1e-12) return;
                pending_.push_back({oid, sym, o.side, o.price, fq, now});
                o.filled_qty += fq;
            };

            if (cfg_.model_queue) {
                const bool hit_through = is_buy ? (px < o.price - 1e-12)
                                                : (px > o.price + 1e-12);
                const bool hit_touch   = std::abs(px - o.price) < 1e-12;

                if (hit_through) {
                    // ★★ FIX ★★ Trade прошёл насквозь нашего уровня —
                    // наш bid/ask был пересечён, fill guaranteed по нашей цене.
                    double fq = std::min(qty, o.qty - o.filled_qty);
                    if (fq > 1e-12) {
                        emit(fq);
                        stats_.hit_through_fills++;
                    }
                } else if (hit_touch) {
                    double consumed = std::min(o.queue_ahead, qty);
                    o.queue_ahead -= consumed;
                    double rem = qty - consumed;
                    if (rem > 1e-12) {
                        double fq = std::min(rem, o.qty - o.filled_qty);
                        if (fq > 1e-12) {
                            emit(fq);
                            stats_.hit_touch_fills++;
                        }
                    } else {
                        stats_.hit_touch_rejects++;
                    }
                }
            } else {
                // Legacy: без queue, накапливаем объём на touch.
                const bool hit_through = is_buy ? (px < o.price)
                                                : (px > o.price);
                const bool hit_touch   = std::abs(px - o.price) < 1e-9;

                if (hit_through) {
                    double fq = std::min(qty, o.qty - o.filled_qty);
                    if (fq > 1e-12) {
                        emit(fq);
                        stats_.hit_through_fills++;
                    }
                } else if (hit_touch) {
                    o.cum_touch_volume_usd += px * qty;
                    if (o.cum_touch_volume_usd >= cfg_.touch_fill_volume_usd) {
                        double fq = std::min(qty, o.qty - o.filled_qty);
                        if (fq > 1e-12) {
                            emit(fq);
                            o.cum_touch_volume_usd = 0.0;
                            stats_.hit_touch_fills++;
                        }
                    } else {
                        stats_.hit_touch_rejects++;
                    }
                }
            }
        }
    }

    std::vector<FillModel::Fill> FillModel::drain_fills() {
        std::vector<Fill> out = std::move(pending_);
        pending_.clear();

        // Убираем полностью заполненные ордера
        for (auto it = orders_.begin(); it != orders_.end(); ) {
            if (it->second.filled_qty >= it->second.qty - 1e-12) {
                it = orders_.erase(it);
            } else {
                ++it;
            }
        }
        return out;
    }

    std::optional<FillModel::Order> FillModel::get_order(const std::string& oid) const {
        auto it = orders_.find(oid);
        if (it == orders_.end()) return std::nullopt;
        return it->second;
    }

} // namespace mm