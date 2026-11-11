#include "paper_exchange.h"
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <algorithm>

namespace arb {

    static inline uint64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
    }

    PaperExchange::PaperExchange(BybitApiClient& api, boost::asio::io_context& ioc)
            : PaperExchange(api, ioc, Config{}) {}

    PaperExchange::PaperExchange(BybitApiClient& api, boost::asio::io_context& ioc, Config cfg)
            : public_api_(api),
              ioc_(ioc),
              cfg_(cfg),
              balance_usd_(cfg.initial_balance_usd) {
        rng_.seed(static_cast<uint32_t>(
                          std::chrono::steady_clock::now().time_since_epoch().count()));
    }

    // ============================================================
    // Latency model
    // ============================================================
    int PaperExchange::random_latency_ms() {
        if (cfg_.latency_ms_max <= cfg_.latency_ms_min) return cfg_.latency_ms_min;
        std::uniform_int_distribution<int> d(cfg_.latency_ms_min, cfg_.latency_ms_max);
        return d(rng_);
    }

    void PaperExchange::schedule_fill(const ExchangeExecution& exec, int latency_ms) {
        auto timer = std::make_shared<boost::asio::steady_timer>(ioc_);
        timer->expires_after(std::chrono::milliseconds(latency_ms));
        auto fill_cb = on_fill_;

        timer->async_wait([timer, exec, fill_cb](boost::system::error_code ec) {
            if (ec) return;
            if (fill_cb) fill_cb(exec);
        });

        std::lock_guard lk(mtx_);
        pending_timers_.push_back(timer);
        if (pending_timers_.size() > 2048) {
            auto now = std::chrono::steady_clock::now();
            pending_timers_.erase(
                    std::remove_if(pending_timers_.begin(), pending_timers_.end(),
                                   [&](const auto& t) { return t->expiry() < now; }),
                    pending_timers_.end());
        }
    }

    void PaperExchange::dispatch_fills(const std::vector<ExchangeExecution>& execs) {
        for (const auto& e : execs) {
            schedule_fill(e, random_latency_ms());
        }
    }

    // ============================================================
    // Book / trade handlers
    // ============================================================
    void PaperExchange::on_book_update(const std::string& sym,
                                       double bb, double ba,
                                       double bb_size, double ba_size) {
        std::lock_guard lk(mtx_);
        bbo_[sym] = {bb, ba, bb_size, ba_size};
    }

    void PaperExchange::on_trade(const std::string& sym, double px, double qty,
                                 bool is_buyer_maker) {
        std::vector<ExchangeExecution> execs;
        {
            std::lock_guard lk(mtx_);

            for (auto& [oid, o] : orders_) {
                if (o.symbol != sym) continue;
                if (o.status != "New" && o.status != "PartiallyFilled") continue;

                bool is_buy = (o.side == "Buy");
                if (is_buyer_maker != is_buy) continue;

                double fill_qty = 0.0;

                if (cfg_.model_queue) {
                    bool hit_through = is_buy ? (px < o.price - 1e-12)
                                              : (px > o.price + 1e-12);
                    bool hit_touch   = std::abs(px - o.price) < 1e-12;

                    if (hit_through) {
                        o.queue_ahead = 0.0;
                        fill_qty = std::min(qty, o.qty - o.filled_qty);
                    } else if (hit_touch) {
                        double consumed = std::min(o.queue_ahead, qty);
                        o.queue_ahead -= consumed;
                        double remaining_after = qty - consumed;
                        if (remaining_after <= 1e-12) continue;
                        fill_qty = std::min(remaining_after, o.qty - o.filled_qty);
                    } else {
                        continue;
                    }
                } else {
                    bool hit_through = is_buy ? (px < o.price) : (px > o.price);
                    bool hit_touch   = std::abs(px - o.price) < 1e-9;
                    if (!hit_through && !hit_touch) continue;
                    if (hit_touch) {
                        std::string key = sym + "_" + std::to_string(o.price) + "_" + o.side;
                        touch_cum_volume_[key] += px * qty;
                        if (touch_cum_volume_[key] < cfg_.touch_fill_volume_usd) continue;
                    }
                    fill_qty = std::min(qty, o.qty - o.filled_qty);
                }

                if (fill_qty <= 1e-12) continue;

                auto& pos = positions_[sym];
                double cur_notional = std::abs(pos.signed_qty * o.price);
                if (cur_notional + o.qty * o.price > cfg_.max_position_usd) {
                    for (auto& [oid2, o2] : orders_) {
                        if (o2.symbol == sym && o2.status == "New") o2.status = "Cancelled";
                    }
                    break;
                }

                apply_fill_locked(o, o.price, fill_qty, true, execs);
            }
        }
        dispatch_fills(execs);
    }

    // ============================================================
    // Market price estimation
    // ============================================================
    double PaperExchange::estimate_market_price_locked(
            const std::string& sym, const std::string& side, double qty) const {
        auto it = bbo_.find(sym);
        if (it == bbo_.end()) return 0.0;
        const auto& bbo = it->second;

        bool is_buy = (side == "Buy");
        double best      = is_buy ? bbo.ask : bbo.bid;
        double best_size = is_buy ? bbo.ask_size : bbo.bid_size;
        if (best <= 0.0) return 0.0;

        if (!cfg_.model_market_slippage) {
            double slip = cfg_.taker_slippage_bps / 10000.0;
            return is_buy ? best * (1.0 + slip) : best * (1.0 - slip);
        }

        if (best_size <= 1e-12 || qty <= best_size) {
            return best;
        }
        double over = (qty - best_size) / best_size;
        double penalty_bps = std::min(over, 2.0) * cfg_.taker_slippage_bps;
        double penalty = penalty_bps / 10000.0;
        return is_buy ? best * (1.0 + penalty) : best * (1.0 - penalty);
    }

    // ============================================================
    // Fill application (locked) — с полным логом
    // ============================================================
    void PaperExchange::apply_fill_locked(POrder& o, double px, double qty, bool is_maker,
                                          std::vector<ExchangeExecution>& out_execs) {
        auto& pos_check = positions_[o.symbol];
        double cur_notional = std::abs(pos_check.signed_qty * px);
        double sign_fill = (o.side == "Buy") ? 1.0 : -1.0;
        bool same_direction = (pos_check.signed_qty == 0.0) ||
                              (std::signbit(pos_check.signed_qty) == std::signbit(sign_fill));
        if (same_direction && cur_notional + qty * px > cfg_.max_position_usd) {
            double allowed_qty = (cfg_.max_position_usd - cur_notional) / px;
            if (allowed_qty <= 1e-9) {
                o.status = "Cancelled";
                std::cout << "[LIMIT-HIT] " << o.symbol << " " << o.side
                          << " cur=$" << std::fixed << std::setprecision(2) << cur_notional
                          << " > max=$" << cfg_.max_position_usd
                          << " — fill rejected\n" << std::defaultfloat;
                return;
            }
            std::cout << "[LIMIT-TRIM] " << o.symbol << " " << o.side
                      << " cur=$" << std::fixed << std::setprecision(2) << cur_notional
                      << " trim qty " << qty << "->" << allowed_qty
                      << "\n" << std::defaultfloat;
            qty = std::min(qty, allowed_qty);
        }

        double notional = px * qty;
        double fee_bps  = is_maker ? cfg_.maker_fee_bps : cfg_.taker_fee_bps;
        double fee      = notional * fee_bps / 10000.0;

        auto& pos = positions_[o.symbol];
        double old_qty = pos.signed_qty;
        double sign    = (o.side == "Buy") ? 1.0 : -1.0;
        double new_qty = old_qty + sign * qty;

        // ═══ CLOSE detection (противоположный знак = закрытие) ═══
        bool is_close_fill = (std::abs(old_qty) > 1e-9)
                             && (std::signbit(old_qty) != std::signbit(sign));

        double realized_leg = 0.0;
        double entry_px_logged = 0.0;
        double closed_qty_logged = 0.0;
        if (!is_close_fill) {
            std::cout << "[PAPER-OPEN] " << o.symbol << " " << o.side
                      << " px=" << std::fixed << std::setprecision(4) << px
                      << " qty=" << qty
                      << " notional=$" << std::setprecision(2) << notional
                      << " fee=$" << std::setprecision(4) << fee
                      << " is_maker=" << (is_maker ? "1" : "0")
                      << std::defaultfloat << "\n";
        }
        if (is_close_fill) {

            double closed_qty = std::min(std::abs(old_qty), qty);
            double entry_px   = pos.avg_price;
            realized_leg = (o.side == "Buy")
                           ? (entry_px - px) * closed_qty
                           : (px - entry_px) * closed_qty;
            realized_pnl_ += realized_leg;
            pos.realized_pnl += realized_leg;
            entry_px_logged = entry_px;
            closed_qty_logged = closed_qty;
        }

        if (std::abs(new_qty) > 1e-9 && std::signbit(new_qty) == std::signbit(sign)) {
            if (std::abs(old_qty) < 1e-9 || std::signbit(old_qty) != std::signbit(new_qty)) {
                pos.avg_price = px;
            } else {
                pos.avg_price = (pos.avg_price * std::abs(old_qty) + px * qty) / std::abs(new_qty);
            }
        }
        pos.signed_qty = new_qty;

        balance_usd_ += realized_leg - fee;
        fees_paid_   += fee;

        // ═══ ЛОГ: каждый close показывает entry, exit, leg, fees, totals ═══
        if (is_close_fill) {
            double bps = (entry_px_logged > 0.0)
                         ? ((px - entry_px_logged) / entry_px_logged * 10000.0 * sign_fill * -1)
                         : 0.0;
            std::cout << "[PAPER-CLOSE] " << o.symbol << " " << o.side
                      << " entry=" << std::fixed << std::setprecision(4) << entry_px_logged
                      << " exit=" << px
                      << " qty=" << closed_qty_logged
                      << " leg=$" << std::setprecision(6) << realized_leg
                      << " fee=$" << fee
                      << " bps=" << std::setprecision(2) << bps
                      << " | total_realized=$" << realized_pnl_
                      << " total_fees=$" << fees_paid_
                      << " bal=$" << balance_usd_
                      << std::defaultfloat << "\n";
        }

        o.filled_qty += qty;
        o.avg_fill_price = (o.avg_fill_price * (o.filled_qty - qty) + px * qty) / o.filled_qty;
        o.fee_paid += fee;
        o.status = (std::abs(o.filled_qty - o.qty) < 1e-9) ? "Filled" : "PartiallyFilled";

        PExec e;
        e.order_id = o.order_id;
        e.symbol = o.symbol;
        e.side = o.side;
        e.exec_price = px;
        e.exec_qty = qty;
        e.exec_fee = fee;
        e.exec_ms = now_ms();
        executions_[o.symbol].push_back(e);

        ExchangeExecution xe;
        xe.order_id = o.order_id;
        xe.symbol = o.symbol;
        xe.side = o.side;
        xe.exec_price = px;
        xe.exec_qty = qty;
        xe.exec_fee = fee;
        xe.is_valid = true;
        xe.is_full_fill = (o.status == "Filled");
        out_execs.push_back(xe);
    }

    // ============================================================
    // Place limit order
    // ============================================================
    std::string PaperExchange::place_limit_order(
            const std::string& sym, const std::string& side,
            double px, double qty,
            double /*tp*/, double /*sl*/, int /*idx*/) {
        std::vector<ExchangeExecution> execs;
        std::string oid;
        {
            std::lock_guard lk(mtx_);
            auto it = bbo_.find(sym);
            if (it == bbo_.end() || it->second.bid <= 0 || it->second.ask <= 0) {
                last_ret_code_ = -1;
                last_ret_msg_ = "no BBO yet";
                return "";
            }
            const auto& bbo = it->second;

            POrder o;
            std::string id_suffix = std::to_string(next_oid_.fetch_add(1));
            if (run_tag_.empty()) {
                o.order_id = "PAPER_" + id_suffix;
            } else {
                o.order_id = "PAPER_" + run_tag_ + "_" + id_suffix;
            }
            o.symbol = sym; o.side = side; o.price = px; o.qty = qty;
            o.placed_ms = now_ms(); o.reduce_only = false;
            o.status = "New";

            bool is_buy = (side == "Buy");
            if (is_buy) {
                if (std::abs(px - bbo.bid) < 1e-12) {
                    o.queue_ahead = bbo.bid_size * 0.02;
                }
                else if (px > bbo.bid) o.queue_ahead = 0.0;
                else                   o.queue_ahead = std::max(1.0 * bbo.bid_size, 1.0);
            } else {
                if (std::abs(px - bbo.ask) < 1e-12) {
                    o.queue_ahead = bbo.ask_size * 0.02;
                }
                else if (px < bbo.ask) o.queue_ahead = 0.0;
                else                   o.queue_ahead = std::max(1.0 * bbo.ask_size, 1.0);
            }

            bool aggressive = false;
            double fill_px = 0.0;
            if (is_buy && bbo.ask > 0 && px + 1e-12 >= bbo.ask) {
                aggressive = true;
                fill_px = estimate_market_price_locked(sym, "Buy", qty);
                if (fill_px > px) fill_px = px;
            } else if (!is_buy && bbo.bid > 0 && px - 1e-12 <= bbo.bid) {
                aggressive = true;
                fill_px = estimate_market_price_locked(sym, "Sell", qty);
                if (fill_px < px) fill_px = px;
            }

            orders_[o.order_id] = o;
            oid = o.order_id;

            if (aggressive && fill_px > 0) {
                apply_fill_locked(orders_[o.order_id], fill_px, qty, false, execs);
                if (orders_[o.order_id].status == "Cancelled") {
                    oid = "";
                }
            }

            last_ret_code_ = 0;
            last_ret_msg_.clear();
        }
        dispatch_fills(execs);
        return oid;
    }

    // ============================================================
    // Place market order
    // ============================================================
    std::string PaperExchange::place_market_order(
            const std::string& sym, const std::string& side, double qty) {
        std::vector<ExchangeExecution> execs;
        std::string oid;
        {
            std::lock_guard lk(mtx_);
            auto it = bbo_.find(sym);
            if (it == bbo_.end() || it->second.bid <= 0 || it->second.ask <= 0) {
                last_ret_code_ = -1; last_ret_msg_ = "no BBO"; return "";
            }

            double fill_px = estimate_market_price_locked(sym, side, qty);
            if (fill_px <= 0) {
                last_ret_code_ = -2; last_ret_msg_ = "no price"; return "";
            }

            POrder o;
            std::string id_suffix = std::to_string(next_oid_.fetch_add(1));
            if (run_tag_.empty()) {
                o.order_id = "PAPER_MKT_" + id_suffix;
            } else {
                o.order_id = "PAPER_MKT_" + run_tag_ + "_" + id_suffix;
            }
            o.symbol = sym; o.side = side; o.price = fill_px; o.qty = qty;
            o.placed_ms = now_ms(); o.reduce_only = false;
            o.status = "New";

            orders_[o.order_id] = o;
            oid = o.order_id;

            apply_fill_locked(orders_[o.order_id], fill_px, qty, false, execs);
            last_ret_code_ = 0; last_ret_msg_.clear();
        }
        dispatch_fills(execs);
        return oid;
    }

    // ============================================================
    // Cancel / query
    // ============================================================
    bool PaperExchange::cancel_order(const std::string& /*sym*/, const std::string& oid) {
        std::lock_guard lk(mtx_);
        auto it = orders_.find(oid);
        if (it == orders_.end()) { last_ret_code_ = -2; return false; }
        if (it->second.status == "Filled" || it->second.status == "Cancelled") {
            last_ret_code_ = -3; return false;
        }
        it->second.status = "Cancelled";
        last_ret_code_ = 0;
        return true;
    }

    ExchangeOrder PaperExchange::get_order(const std::string& /*sym*/, const std::string& oid) {
        std::lock_guard lk(mtx_);
        ExchangeOrder r;
        auto it = orders_.find(oid);
        if (it == orders_.end()) return r;
        r.order_id = it->second.order_id;
        r.order_status = it->second.status;
        r.qty = it->second.filled_qty;
        r.price = it->second.avg_fill_price;
        return r;
    }

    ExchangePositionInfo PaperExchange::get_position_info(const std::string& sym, int /*idx*/) {
        std::lock_guard lk(mtx_);
        ExchangePositionInfo r;
        r.symbol = sym;
        auto it = positions_.find(sym);
        if (it == positions_.end() || std::abs(it->second.signed_qty) < 1e-9) {
            r.is_valid = true;
            r.signed_qty = 0.0;
            r.size = 0.0;
            return r;
        }
        r.signed_qty = it->second.signed_qty;
        r.side = (it->second.signed_qty > 0) ? "Buy" : "Sell";
        r.size = std::abs(it->second.signed_qty);
        r.avg_price = it->second.avg_price;
        auto bbo_it = bbo_.find(sym);
        if (bbo_it != bbo_.end()) {
            double mid = (bbo_it->second.bid + bbo_it->second.ask) / 2.0;
            r.unrealized_pnl = (it->second.signed_qty > 0)
                               ? (mid - it->second.avg_price) * it->second.signed_qty
                               : (it->second.avg_price - mid) * (-it->second.signed_qty);
        }
        r.is_valid = true;
        return r;
    }

    ExchangeExecution PaperExchange::get_last_execution(
            const std::string& sym, const std::string& oid) {
        std::lock_guard lk(mtx_);
        ExchangeExecution r;
        auto it = executions_.find(sym);
        if (it == executions_.end()) return r;
        for (auto rit = it->second.rbegin(); rit != it->second.rend(); ++rit) {
            if (rit->order_id == oid) {
                r.order_id = rit->order_id; r.symbol = rit->symbol; r.side = rit->side;
                r.exec_price = rit->exec_price; r.exec_qty = rit->exec_qty;
                r.exec_fee = rit->exec_fee; r.is_valid = true;
                return r;
            }
        }
        return r;
    }

    ExchangeInstrumentInfo PaperExchange::get_instrument_info(
            const std::string& sym, const std::string& category) {
        auto i = public_api_.get_instrument_info(sym, category);
        ExchangeInstrumentInfo r;
        r.tick_size = i.tick_size; r.step_size = i.step_size;
        r.min_order_qty = i.min_order_qty; r.min_order_amt = i.min_order_amt;
        r.is_valid = i.tick_size > 0 && i.step_size > 0;
        return r;
    }

    double PaperExchange::normalize_price(const std::string& sym, double px) {
        return public_api_.normalize_price(sym, px);
    }
    double PaperExchange::normalize_qty(const std::string& sym, double qty) {
        return public_api_.normalize_qty(sym, qty);
    }

    double PaperExchange::balance() const {
        std::lock_guard lk(mtx_);
        return balance_usd_;
    }

    // ============================================================
    // Snapshot — 4 знака, все totals сходятся
    // ============================================================
    void PaperExchange::print_snapshot() const {
        std::lock_guard lk(mtx_);

        double pos_realized_sum = 0.0;
        int open_pos_count = 0;
        for (auto& [sym, p] : positions_) {
            if (std::abs(p.signed_qty) < 1e-9) continue;
            pos_realized_sum += p.realized_pnl;
            ++open_pos_count;
        }

        std::cout << std::fixed << std::setprecision(4);
        std::cout << "[PAPER] bal=$" << balance_usd_
                  << " | realized=$" << realized_pnl_
                  << " | fees=$" << fees_paid_
                  << " | pos=" << open_pos_count
                  << " | pos_realized_sum=$" << pos_realized_sum
                  << " | orders=" << orders_.size() << "\n";

        std::cout << std::setprecision(8);
        for (auto& [sym, p] : positions_) {
            if (std::abs(p.signed_qty) < 1e-9) continue;
            std::cout << "  POS " << sym << " qty=" << p.signed_qty
                      << " avg=" << p.avg_price
                      << " realized=$" << p.realized_pnl << "\n";
        }
        std::cout << std::defaultfloat;
    }

    int PaperExchange::close_orphan_positions(const std::set<std::string>& active_symbols) {
        std::vector<std::pair<std::string, double>> orphans;
        {
            std::lock_guard lk(mtx_);
            for (const auto& [sym, p] : positions_) {
                if (std::abs(p.signed_qty) < 1e-9) continue;
                if (active_symbols.count(sym)) continue;
                orphans.emplace_back(sym, p.signed_qty);
            }
        }

        if (orphans.empty()) return 0;

        int closed = 0;
        for (const auto& [sym, qty] : orphans) {
            std::string side = (qty > 0) ? "Sell" : "Buy";
            try {
                std::string oid = place_market_order(sym, side, std::abs(qty));
                if (!oid.empty()) {
                    ++closed;
                    std::cout << "[PAPER] orphan closed " << sym
                              << " qty=" << qty
                              << " order=" << oid << "\n";
                } else {
                    std::cerr << "[PAPER] orphan close failed " << sym
                              << " qty=" << qty
                              << " code=" << last_ret_code_
                              << " msg=" << last_ret_msg_ << "\n";
                }
            } catch (const std::exception& e) {
                std::cerr << "[PAPER] orphan close exception " << sym
                          << ": " << e.what() << "\n";
            }
        }
        return closed;
    }

} // namespace arb