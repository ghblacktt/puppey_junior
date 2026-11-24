#include "backtest_runner.h"
#include "book_rebuilder.h"
#include <iostream>
#include <algorithm>
using namespace arb;
namespace bt {

    BacktestRunner::BacktestRunner(const RunConfig& cfg, const InstrumentParams& inst)
            : cfg_(cfg), inst_(inst) {
        engine_.set_latency({cfg_.latency_ms, (uint64_t)cfg_.latency_jitter_ms});
        engine_.set_fees(cfg_.maker_fee_bps, 5.5);

        exchange_ = std::make_unique<BacktestExchange>(engine_, book_, cfg_.symbol);
        exchange_->set_instrument(inst.tick_size, inst.step_size,
                                  inst.min_order_qty, inst.min_order_amt);

        // Fill callback: MM получит on_order_filled когда matching engine дал fill
        exchange_->set_fill_callback([this](const BtFill& f) {
            if (mm_) {
                mm_->on_order_filled(f.order_id, f.timestamp_ms,
                                     f.price, f.qty, f.side, f.is_full);
            }
        });

        mm_ = std::make_shared<MarketMakingStrategy>(
                book_, cluster_, *exchange_,
                cfg_.symbol, cfg_.base_spread_bps / 10000.0,
                cfg_.qty_usd, cfg_.max_position_usd);

        mm_->set_sigma_ref(0.0015);        // σ_ref = 15 bps (реалистично для SOLUSDT)
        mm_->set_trend_thr_bps(200.0);     // TREND только при >200 bps за 60с
        mm_->set_vol_ratio_max(100.0);

        mm_->set_maker_fee_bps(cfg_.maker_fee_bps);
        mm_->set_use_adaptive(false);
        AdaptiveController::Config adaptive;
        adaptive.min_half_spread_bps = cfg_.min_hs_bps;
        adaptive.max_half_spread_bps = 50.0;
        adaptive.min_qty_usd = 4.0;
        adaptive.max_qty_usd = cfg_.qty_usd * 1.8;
        adaptive.kill_adverse_bps = 8.0;
        mm_->set_adaptive_config(adaptive);

        mm_->set_half_spread_bounds(cfg_.min_hs_bps / 10000.0, 0.05);
        mm_->set_hard_cap_ratio(0.85);
        mm_->set_block_ratio(0.10);
        mm_->set_gamma(10.0);
        mm_->set_vpin_thresholds(0.99, 0.98);
        mm_->set_soft_exit_pnl_pct(0.01);
        mm_->set_soft_exit_ratio(0.15);
        mm_->set_momentum_weight(cfg_.momentum_weight);
        if (cfg_.max_hold_sec > 0) mm_->set_max_hold_sec(cfg_.max_hold_sec);
    }

    BacktestResult BacktestRunner::run(const std::vector<TimedEvent>& events) {
        BacktestResult r;
        r.total_events = events.size();
        if (events.empty()) return r;


        r.first_ts = events.front().timestamp_ms;
        r.last_ts  = events.back().timestamp_ms;

        // ── Sprint 3: forced regime для A/B-сравнения ──
        if (cfg_.force_regime_toxic) {
            mm::SymModeSnapshot snap;
            snap.bid.mode = mm::SymMode::Toxic;
            snap.ask.mode = mm::SymMode::Toxic;
            snap.bid.n_markouts_4h = 30;
            snap.ask.n_markouts_4h = 30;
            snap.bid.hit_pct_1s = 20.0;
            snap.ask.hit_pct_1s = 20.0;
            snap.has_data = true;
            snap.updated_ms = r.first_ts;
            mm_->set_regime_snapshot(snap);
            std::cout << "[BT] FORCED regime: Toxic/Toxic\n";
        } else if (cfg_.force_regime_illiquid) {
            mm::SymModeSnapshot snap;
            snap.bid.mode = mm::SymMode::Illiquid;
            snap.ask.mode = mm::SymMode::Illiquid;
            snap.has_data = true;
            snap.updated_ms = r.first_ts;
            mm_->set_regime_snapshot(snap);
            std::cout << "[BT] FORCED regime: Illiquid/Illiquid\n";
        }

        // Переопределяем fill callback чтобы считать fills в r
        exchange_->set_fill_callback([this, &r](const BtFill& f) {
            ++r.fills;
            if (f.is_full) ++r.full_fills;
            r.total_volume += f.qty;
            if (mm_) {
                mm_->on_order_filled(f.order_id, f.timestamp_ms,
                                     f.price, f.qty, f.side, f.is_full);
            }
        });

        int tick_counter = 0;
        uint64_t last_prune_ts = 0;
        uint64_t last_tick_ts = 0;
        constexpr uint64_t kTickIntervalMs = 100;

        for (const auto& ev : events) {
            exchange_->set_current_ts(ev.timestamp_ms);
            if (mm_) mm_->set_current_time_ms(ev.timestamp_ms);

            if (ev.type == TimedEvent::BOOK) {
                ++r.book_events;
                std::string err;
                if (!BookRebuilder::apply(book_, ev.book, &err)) {
                    exchange_->process_pending_orders();
                    continue;
                }

                double bb = book_.get_best_bid();
                double ba = book_.get_best_ask();
                if (bb <= 0.0 || ba <= 0.0 || ba < bb) {
                    static int sanity_warn = 0;
                    if (sanity_warn < 5) {
                        std::cerr << "[BT-SANITY] invalid book ts=" << ev.timestamp_ms
                                  << " bb=" << bb << " ba=" << ba << "\n";
                        sanity_warn++;
                    }
                    continue;
                }
                constexpr uint64_t kBookStaleMs = 10'000;
                uint64_t book_age = (ev.timestamp_ms > book_.get_last_update_ms())
                                    ? (ev.timestamp_ms - book_.get_last_update_ms())
                                    : 0;

                if (book_age > kBookStaleMs) {
                    static int stale_warn = 0;
                    if (stale_warn < 5) {
                        std::cerr << "[BT-STALE] book age=" << book_age
                                  << "ms > 10s, skip tick at ts=" << ev.timestamp_ms << "\n";
                        stale_warn++;
                    }
                    // process_pending всё равно нужен — иначе ордера застрянут
                    continue;
                }

                bool should_tick = (last_tick_ts == 0)
                                   || (ev.timestamp_ms - last_tick_ts >= kTickIntervalMs);

                if (mm_ && should_tick) {
                    try {
                        mm_->tick();
                        last_tick_ts = ev.timestamp_ms;

                        auto finalized = mm_->drain_finalized_markouts();
                        for (auto& m : finalized) {
                            r.markouts.push_back(m);
                            all_markouts_.push_back(m);
                        }
                        if (!cfg_.force_regime_toxic && !cfg_.force_regime_illiquid) {
                            if (last_regime_update_ms_ == 0
                                || ev.timestamp_ms - last_regime_update_ms_ >= 300'000) {
                                update_regime_inmemory(ev.timestamp_ms);
                                last_regime_update_ms_ = ev.timestamp_ms;
                            }
                        }
                    } catch (const std::exception& e) {
                        std::cerr << "[MM-EXC] " << e.what() << "\n";
                    }
                }

                ++tick_counter;

                // Диагностика статуса
                if (tick_counter % 20000 == 0) {
                    std::cerr << "[BT-STATUS] tick=" << tick_counter
                              << " open_orders=" << engine_.open_order_count()
                              << " fills_so_far=" << r.fills << "\n";
                }
            } else {
                ++r.trade_events;
                exchange_->process_pending_orders();
                cluster_.add_historical_trade(
                        ev.timestamp_ms, ev.trade.price, ev.trade.volume,
                        ev.trade.is_buyer_maker);

                // ── MM toxic detection ──
                if (mm_) {
                    if (!ev.trade.is_buyer_maker)
                        mm_->on_trade_through_ask(ev.trade.price, ev.trade.volume);
                    else
                        mm_->on_trade_through_bid(ev.trade.price, ev.trade.volume);
                }
                exchange_->on_trade(ev.trade);
            }

            if (ev.timestamp_ms - last_prune_ts > 60000) {
                engine_.prune_completed(ev.timestamp_ms, 120000);
                last_prune_ts = ev.timestamp_ms;
            }
        }
        if (mm_) {
            std::cout << "[BT] force-finalizing markouts at ts="
                      << (r.last_ts + 200'000) << "\n";
            mm_->set_current_time_ms(r.last_ts + 200'000);
            try {
                mm_->tick();   // update_pending_markouts (после Fix 1) forced-finalize всех pending
                auto finalized = mm_->drain_finalized_markouts();
                for (auto& m : finalized) {
                    r.markouts.push_back(m);
                    all_markouts_.push_back(m);
                }
                std::cout << "[BT] +" << finalized.size()
                          << " markouts from force-finalize\n";
            } catch (const std::exception& e) {
                std::cerr << "[BT-FORCE] " << e.what() << "\n";
            }
        }

// Закрываем orphan position market-ом в конце
        {
            auto pos = exchange_->get_position_info(cfg_.symbol);
            if (std::abs(pos.signed_qty) > 1e-6) {
                std::string close_side = (pos.signed_qty > 0) ? "Sell" : "Buy";
                std::cout << "[BT] Closing final position: " << close_side
                          << " " << std::abs(pos.signed_qty) << "\n";
                exchange_->place_market_order(cfg_.symbol, close_side, std::abs(pos.signed_qty));
            }
        }
        auto pos = exchange_->get_position_info(cfg_.symbol);
        r.final_position_qty = pos.signed_qty;
        r.final_position_avg = pos.avg_price;
        r.total_fees = exchange_->fees_paid();
        r.realized_pnl = exchange_->realized_pnl();
        r.net_pnl = r.realized_pnl - r.total_fees;

        // ── В конце BacktestRunner::run() перед return r ──
        {
            // Классифицируем каждый fill как WIN/LOSS по mk_5s
            int wins = 0, losses = 0, scratches = 0;
            double sum_win = 0.0, sum_loss = 0.0;

            for (const auto& m : all_markouts_) {
                if (!m.finalized || m.is_close) continue;
                if (m.mk_5s_bps == 0.0 && m.mk_1s_bps == 0.0) continue; // нет данных

                if (m.mk_5s_bps > 2.0) {       // WIN: >2 bps через 5s
                    wins++;
                    sum_win += m.mk_5s_bps;
                } else if (m.mk_5s_bps < -2.0) { // LOSS: <-2 bps через 5s
                    losses++;
                    sum_loss += m.mk_5s_bps;
                } else {
                    scratches++;
                }
            }

            int total = wins + losses + scratches;
            if (total > 0) {
                double win_rate = 100.0 * wins / total;
                double profit_factor = (sum_win > 0 && std::abs(sum_loss) > 0.01)
                                       ? sum_win / std::abs(sum_loss)
                                       : 0.0;

                std::cout << "\n=== EDGE ANALYSIS ===\n";
                std::cout << "Total fills:  " << total << "\n";
                std::cout << "Wins:         " << wins << " (" << win_rate << "%)\n";
                std::cout << "Losses:       " << losses << "\n";
                std::cout << "Scratches:    " << scratches << "\n";
                std::cout << "Profit Factor:" << std::fixed << std::setprecision(2)
                          << profit_factor << "\n";
                std::cout << "Avg Win:      " << (wins > 0 ? sum_win/wins : 0) << " bps\n";
                std::cout << "Avg Loss:     " << (losses > 0 ? sum_loss/losses : 0) << " bps\n";
            }
        }

        return r;
    }

    void BacktestRunner::update_regime_inmemory(uint64_t now_ms) {
        if (!mm_) return;

        constexpr uint64_t kWindowMs = 4 * 3600 * 1000;
        uint64_t cutoff = (now_ms > kWindowMs) ? (now_ms - kWindowMs) : 0;

        struct SideAcc {
            int    n         = 0;
            double sum_mk_1s = 0.0;
            double sum_mk_30s= 0.0;
            int    n_hit_1s  = 0;
        };
        SideAcc buy_acc, sell_acc;

        for (const auto& m : all_markouts_) {
            if (!m.finalized) continue;
            if (m.fill_time_ms < cutoff) continue;

            SideAcc* acc = (m.side == "Buy") ? &buy_acc : &sell_acc;
            acc->n++;
            acc->sum_mk_1s  += m.mk_1s_bps;
            acc->sum_mk_30s += m.mk_30s_bps;
            if (m.mk_1s_bps > 0.0) acc->n_hit_1s++;
        }

        // Market-derived
        double depth_usdt = 0.0, spread_frac = 0.0, vpin = 0.0;
        {
            double bb = book_.get_best_bid();
            double ba = book_.get_best_ask();
            double bs = book_.get_best_bid_size();
            double as = book_.get_best_ask_size();
            if (bb > 0 && ba > 0) {
                double mid = (bb + ba) / 2.0;
                depth_usdt  = std::min(bs * bb, as * ba) * 5.0;
                spread_frac = (ba - bb) / mid;
            }
            vpin = cluster_.get_vpin(20);
        }

        mm::SymModeSnapshot snap;

        auto fill_side = [](mm::SymModeSide& s, const SideAcc& a) {
            s.n_markouts_4h = a.n;
            if (a.n > 0) {
                s.med_mk_1s  = a.sum_mk_1s  / a.n;
                s.med_mk_30s = a.sum_mk_30s / a.n;
                s.hit_pct_1s = 100.0 * a.n_hit_1s / a.n;
            }
        };
        fill_side(snap.bid, buy_acc);
        fill_side(snap.ask, sell_acc);

        snap.bid.mode = mm::classify_side(snap.bid, depth_usdt, spread_frac, vpin);
        snap.ask.mode = mm::classify_side(snap.ask, depth_usdt, spread_frac, vpin);

        const auto& prev = mm_->regime();
        snap.bid = mm::apply_hysteresis(prev.bid, snap.bid, now_ms);
        snap.ask = mm::apply_hysteresis(prev.ask, snap.ask, now_ms);

        snap.depth_usdt  = depth_usdt;
        snap.spread_frac = spread_frac;
        snap.vpin_now    = vpin;
        snap.updated_ms  = now_ms;
        snap.has_data    = (buy_acc.n >= 20) || (sell_acc.n >= 20);

        mm_->set_regime_snapshot(snap);

        std::cout << "[BT-REGIME] " << cfg_.symbol
                  << " bid:" << mm::to_str(snap.bid.mode)
                  << "(n=" << snap.bid.n_markouts_4h
                  << ",hit=" << std::fixed << std::setprecision(1)
                  << snap.bid.hit_pct_1s << "%)"
                  << " ask:" << mm::to_str(snap.ask.mode)
                  << "(n=" << snap.ask.n_markouts_4h
                  << ",hit=" << snap.ask.hit_pct_1s << "%)"
                  << " vpin=" << std::setprecision(2) << vpin
                  << std::defaultfloat << "\n";
    }

    void BacktestRunner::set_tp_maker_bps(double bps) {
        std::cout << "[RUNNER] Setting tp_maker_bps to: " << bps << "\n";
        if (mm_) {
            mm_->set_tp_maker_bps(bps);
        }
    }

}  // namespace bt