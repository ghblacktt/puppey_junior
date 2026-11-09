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

        // Создаём MM
        ClusterEngine dummy_cluster;
        (void)dummy_cluster;  // MM требует cluster - создаём пустой

        // TODO: передать реальный cluster если нужно для VPIN
        // Пока создаём заглушку
        static ClusterEngine stub_cluster;
        mm_ = std::make_unique<MarketMakingStrategy>(
                book_, stub_cluster, *exchange_,
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
        mm_->set_vpin_thresholds(0.85, 0.60);
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

        for (const auto& ev : events) {
            exchange_->set_current_ts(ev.timestamp_ms);
            if (mm_) mm_->set_current_time_ms(ev.timestamp_ms);

            if (ev.type == TimedEvent::BOOK) {
                ++r.book_events;
                std::string err;
                if (!BookRebuilder::apply(book_, ev.book, &err)) continue;

                // Sanity: книга валидная?
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
                if (ba > bb * 1.10) {
                    static int spread_warn = 0;
                    if (spread_warn < 5) {
                        std::cerr << "[BT-SANITY] wide spread ts=" << ev.timestamp_ms
                                  << " bb=" << bb << " ba=" << ba
                                  << " spread=" << ((ba - bb) / bb * 100.0) << "%\n";
                        spread_warn++;
                    }
                    continue;
                }

                // Активируем pending orders (submit + latency)
                exchange_->process_pending_orders();

                // MM тик — ставит/переставляет ордера
                if (mm_) {
                    try {
                        mm_->tick();
                        auto finalized = mm_->drain_finalized_markouts();
                        for (auto& m : finalized) {
                            r.markouts.push_back(m);
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
                // ═══ MM toxic detection — как в live bybit_ws.cpp ═══
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

        return r;
    }

}  // namespace bt