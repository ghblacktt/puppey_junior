#include "bybit/bybit_ws.h"
#include "util/console.h"
#include <math.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <set>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <deque>
#include <iomanip>
#include <algorithm>
#include <map>
#include <tuple>
#include <unordered_map>
#include "cross_venue_signal.h"
using arb::IExchange;
// ============================================================
// Constructor — без изменений
// ============================================================
BybitWSClient::BybitWSClient(net::io_context& ioc, ssl::context& ctx,
                             const std::vector<std::string>& symbols,
                             OrderExecutor* executor,
                             BybitApiClient& api,
                             const std::string& ws_path,
                             bool auxiliary,
                             const std::string& pg_conn_str)
        : ioc_(ioc), ctx_(ctx), resolver_(net::make_strand(ioc)),
          ping_timer_(net::make_strand(ioc)), symbols_(symbols),
          order_executor_(executor), api_(api),
          ws_path_(ws_path), auxiliary_(auxiliary),
          pg_conn_str_(pg_conn_str),
          execution_poll_timer_(net::make_strand(ioc)),
          balance_timer_(net::make_strand(ioc)),
          stats_timer_(net::make_strand(ioc)),
          position_update_timer_(net::make_strand(ioc)),
          mm_fill_poll_timer_(net::make_strand(ioc)),
          inv_refresh_timer_(net::make_strand(ioc)),
          pos_manage_timer_(net::make_strand(ioc)),
          reconnect_timer_(net::make_strand(ioc)),
          subscribe_delay_timer_(net::make_strand(ioc)),
          regime_update_timer_(net::make_strand(ioc))
{
    for (const auto& symbol : symbols) {
        tickers_[symbol] = std::make_unique<TickerState>(symbol, api_, 1);
    }

    if (!auxiliary_) {
        balance_timer_.expires_after(std::chrono::seconds(60));
        balance_timer_.async_wait([this](beast::error_code ec) { if (!ec) print_balance(); });

        signal_logger_ = std::make_unique<SignalLogger>(pg_conn_str_);
        trade_tracker_ = std::make_unique<TradeTracker>(
                pg_conn_str_,
                [this](const std::string& symbol) { return get_current_price(symbol); },
                [this](const std::string& symbol) { return get_symbol_liquidity(symbol); },
                [this](const std::string& symbol) {
                    auto it = tickers_.find(symbol);
                    if (it != tickers_.end()) return it->second->vwma.get_vwma();
                    return 0.0;
                },
                order_executor_);
        trade_tracker_->start();
        initial_balance_ = api_.get_balance("USDT");
        schedule_stats_output();

        mm_db_conn_ = std::make_shared<pqxx::connection>(pg_conn_str_);   // ← было хардкод

        start_mm_fill_polling();
        start_mm_tick();
    }

    create_websocket();
}

void BybitWSClient::start_mm_tick() {
    std::cout << clr::bold << clr::cyan
              << "\n" << pad_r("TIME", 12)
              << "  " << pad_r("", 3)
              << pad_r("SYMBOL", 14)
              << pad_l("BID", 10)
              << pad_l("ASK", 10)
              << pad_l("SPREAD", 8)
              << pad_l("σ", 5)
              << pad_l("INV$", 6)
              << pad_l("PNL%", 8)
              << "  STATUS"
              << clr::reset << "\n";
    std::cout << clr::gray << std::string(95, '-') << clr::reset << "\n";
    start_pos_manage();
    start_regime_update();
}

// ============================================================
// init_mm_strategies — принимает IExchange&
// ============================================================
void BybitWSClient::init_mm_strategies(IExchange& exchange) {
    exchange_ = &exchange;

    if (!mm_db_conn_) {
        mm_db_conn_ = std::make_shared<pqxx::connection>(pg_conn_str_);
    }


    int initialized = 0;
    for (const auto& symbol : symbols_) {
        auto it = tickers_.find(symbol);
        if (it == tickers_.end()) continue;
        if (!mm_registry_ || !mm_registry_->is_enabled(symbol)) continue;

        try {
            size_t before = mm_strategies_.size();
            create_mm_for_symbol(symbol);
            size_t after = mm_strategies_.size();
            if (after > before) {
                ++initialized;
            } else {
                std::cerr << "[MM] " << symbol
                          << " NOT created (check earlier WS logs)\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "[MM] " << symbol << " creation failed: " << e.what() << "\n";
        }
    }

    std::cout << "[MM] Strategies initialized for "
              << initialized << " symbols\n";
    mm_initialized_.store(true, std::memory_order_release);
    std::cout << "[MM] mm_initialized_ = true (tick will start iterating)\n";
}

void BybitWSClient::create_mm_for_symbol(const std::string& symbol) {
    if (!exchange_) {
        return;
    }
    auto it = tickers_.find(symbol);
    if (it == tickers_.end()) {
        return;
    }
    if (mm_strategies_.count(symbol) > 0) return;

    double px_hint = it->second->orderbook.get_mid_price();
    if (px_hint <= 0.0) {
        return;
    }

    MmGlobalConfig g;
    if (mm_registry_) g = mm_registry_->global();
    MmSymbolConfig sym_cfg;
    if (mm_registry_) sym_cfg = mm_registry_->get(symbol);

    auto inst = api_.get_instrument_info(symbol, "linear");
    const double default_max_pos = (g.max_position_default_usd > 0.0)
                                   ? g.max_position_default_usd : 50.0;
    double max_position_usd = (sym_cfg.max_position_usd > 0.0)
                              ? sym_cfg.max_position_usd
                              : default_max_pos;
    double step_notional = inst.step_size * px_hint;
    double min_qty_safe  = std::max(5.5, step_notional * 1.5);
    double max_qty_safe  = max_position_usd * 0.30;

    double qty_usd;
    if (sym_cfg.qty_usd > 0.0) {
        qty_usd = std::clamp(sym_cfg.qty_usd, 5.5, max_qty_safe);
    } else {
        qty_usd = std::clamp(min_qty_safe, 5.5, max_qty_safe);
    }

    // ── min_hs ──
    double min_hs;
    if (sym_cfg.min_hs_bps > 0.0) {
        min_hs = sym_cfg.min_hs_bps / 10000.0;
    } else if (px_hint > 100.0) {
        min_hs = 0.0005;
    } else if (px_hint > 1.0) {
        min_hs = 0.0010;
    } else {
        min_hs = 0.0020;
    }

    double base_spread = (sym_cfg.base_spread_bps > 0.0)
                         ? sym_cfg.base_spread_bps / 10000.0
                         : min_hs * 2.0;

    // ── Создание MM ──
    auto mm = std::make_shared<MarketMakingStrategy>(
            it->second->orderbook, it->second->cluster, *exchange_,
            symbol, base_spread, qty_usd, max_position_usd);
    if (signal_engine_) {
        mm->set_signal_engine(signal_engine_.get());
    }
// Определи, торгуется ли символ на Bitunix
    if (is_bitunix_symbol(symbol)) {
        mm->set_is_bitunix_symbol(true);
    }
    mm->set_executor(pos_manage_timer_.get_executor());
    mm->set_maker_fee_bps(g.maker_fee_bps);
    mm->set_use_adaptive(true);

    AdaptiveController::Config cfg;
    cfg.min_half_spread_bps = min_hs * 10000.0;
    cfg.max_half_spread_bps = g.max_half_spread_bps;
    cfg.min_qty_usd         = std::max(g.min_qty_floor_usd, min_qty_safe * 0.9);
    cfg.max_qty_usd         = std::max(10.0, qty_usd * g.max_qty_multiplier);
    cfg.kill_adverse_bps    = g.kill_adverse_bps;
    mm->set_adaptive_config(cfg);

    mm->set_half_spread_bounds(min_hs, g.half_spread_upper_pct);
    mm->set_hard_cap_ratio(g.hard_cap_ratio);
    mm->set_block_ratio(g.block_ratio);
    mm->set_gamma(g.gamma);
    mm->set_vpin_thresholds(g.vpin_pause_threshold, g.vpin_resume_threshold);
    mm->set_soft_exit_pnl_pct(g.soft_exit_pnl_pct);
    mm->set_soft_exit_ratio(g.soft_exit_ratio);
    mm->set_regime_filter(&it->second->regime_filter);
    mm->set_db_connection(mm_db_conn_);

    if (paper_exchange_ != nullptr) {
        mm->set_run_tag(paper_exchange_->run_tag());
    }
    mm->set_momentum_weight(sym_cfg.momentum_weight > 0 ? sym_cfg.momentum_weight : 1.0);

    if (sym_cfg.max_hold_sec > 0) {
        mm->set_max_hold_sec(sym_cfg.max_hold_sec);
    }

    std::cout << clr::green << "  ✓ " << clr::reset
              << pad_r(symbol, 14)
              << "$" << num(px_hint, 4)
              << "  half=" << num(min_hs * 10000, 1) << "bps"
              << "  qty=$" << num(qty_usd, 0)
              << "\n";

    mm_strategies_[symbol] = std::move(mm);
}

// ============================================================
// start_mm_fill_polling — пропускаем в paper mode
// (fills приходят через callback от PaperExchange)
// ============================================================
void BybitWSClient::start_mm_fill_polling() {
    if (paper_exchange_ != nullptr) {
        std::cout << "[MM] paper mode — skipping fill polling (callback driven)\n";
        return;
    }
    mm_fill_poll_timer_.expires_after(std::chrono::seconds(15));
    mm_fill_poll_timer_.async_wait([this](beast::error_code ec) {
        if (!ec) {
            poll_mm_fills();
            start_mm_fill_polling();
        }
    });
}

void BybitWSClient::start_inv_refresh() {
    inv_refresh_timer_.expires_after(std::chrono::seconds(30));
    inv_refresh_timer_.async_wait([this](beast::error_code ec) {
        if (!ec) start_inv_refresh();
    });
}

void BybitWSClient::start_pos_manage() {
    if (!mm_initialized_.load(std::memory_order_acquire)) {
        pos_manage_timer_.expires_after(std::chrono::milliseconds(100));
        pos_manage_timer_.async_wait([this](beast::error_code ec) {
            if (!ec) start_pos_manage();
        });
        return;
    }

    pos_manage_timer_.expires_after(std::chrono::milliseconds(100));
    pos_manage_timer_.async_wait([this](beast::error_code ec) {
        if (ec) return;
        if (signal_engine_) {
            uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
            signal_engine_->drain(now_ms);
        }
        for (auto& [sym, mm] : mm_strategies_) {
            if (!mm) continue;
            try { mm->tick(); }
            catch (const std::exception& e) {
                std::cerr << "[MM-TICK] " << sym << " exception: " << e.what() << "\n";
            } catch (...) {
                std::cerr << "[MM-TICK] " << sym << " unknown exception\n";
            }
        }
        start_pos_manage();
    });
}

void BybitWSClient::poll_mm_fills() {
    constexpr uint64_t WINDOW_MS = 5 * 60 * 1000;
    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    uint64_t start_time = now_ms - WINDOW_MS;

    for (auto it = processed_fills_.begin(); it != processed_fills_.end();) {
        if (it->second < start_time) it = processed_fills_.erase(it);
        else                         ++it;
    }

    for (const auto& symbol : symbols_) {
        auto mm_it = mm_strategies_.find(symbol);
        if (mm_it == mm_strategies_.end() || !mm_it->second) continue;

        auto execs = api_.get_executions(symbol, start_time, 50);
        for (const auto& e : execs) {
            if (!e.is_valid) continue;
            if (processed_fills_.count(e.exec_id)) continue;
            processed_fills_[e.exec_id] = e.exec_time;

            BybitOrder ord = api_.get_order(symbol, e.order_id);
            bool fully_filled = (ord.order_status == "Filled");
            std::string new_status = fully_filled ? "FILLED" : "PARTIALLY_FILLED";

            if (mm_db_conn_ && mm_db_conn_->is_open()) {
                try {
                    pqxx::work txn(*mm_db_conn_);
                    auto cur = txn.exec(
                            "SELECT fill_qty, fill_price FROM mm_orders "
                            "WHERE order_id = $1 AND status IN ('OPEN','PARTIALLY_FILLED') "
                            "FOR UPDATE",
                            pqxx::params{e.order_id});

                    if (cur.empty()) {
                        txn.abort();
                    } else {
                        double old_qty = cur[0]["fill_qty"].is_null() ? 0.0 : cur[0]["fill_qty"].as<double>();
                        double old_price = cur[0]["fill_price"].is_null() ? 0.0 : cur[0]["fill_price"].as<double>();
                        double new_qty = old_qty + e.exec_qty;
                        double new_price = (old_qty > 0 && old_price > 0)
                                           ? (old_price * old_qty + e.exec_price * e.exec_qty) / new_qty
                                           : e.exec_price;

                        txn.exec(
                                "UPDATE mm_orders SET "
                                " status = $1, "
                                " filled_time = COALESCE(filled_time, $2), "
                                " fill_price = $3, fill_qty = $4, "
                                " fee = COALESCE(fee, 0) + $5, "
                                " exec_id = $6, exec_type = $7, is_partial = $8, "
                                " order_age_ms = $2 - placed_time "
                                "WHERE order_id = $9 AND status IN ('OPEN','PARTIALLY_FILLED')",
                                pqxx::params{
                                        new_status, e.exec_time, new_price, new_qty, e.exec_fee,
                                        e.exec_id, e.exec_type, !fully_filled, e.order_id});
                        txn.commit();
                    }
                } catch (const std::exception& ex) {
                    std::cerr << "[MM-DB] Fill update error: " << ex.what() << "\n";
                }
            }

            mm_it->second->on_order_filled(e.order_id, e.exec_time, e.exec_price,
                                           e.exec_qty, e.side);

            std::cout << "[FILL] " << symbol << " | " << e.side
                      << " | " << e.exec_type
                      << " | price=" << e.exec_price
                      << " | qty=" << e.exec_qty
                      << " | fee=" << e.exec_fee << " " << e.fee_currency
                      << " | " << new_status
                      << " | order=" << e.order_id << "\n";
        }
    }
}

// ============================================================
// Остальные методы — без изменений, кроме process_message
// ============================================================

bool BybitWSClient::can_open_new_position(double current_balance) {
    if (kill_switch_active_) return false;
    if (initial_balance_ > 0) {
        double loss_pct = (initial_balance_ - current_balance) / initial_balance_;
        if (loss_pct >= max_daily_loss_pct_) {
            std::cerr << "[RISK] Daily loss limit reached! Loss: " << loss_pct * 100 << "%\n";
            kill_switch_active_ = true;
            return false;
        }
    }
    static double peak_balance = initial_balance_;
    if (current_balance > peak_balance) peak_balance = current_balance;
    if (peak_balance > 0) {
        double drawdown = (peak_balance - current_balance) / peak_balance;
        if (drawdown >= max_drawdown_pct_) {
            std::cerr << "[RISK] Max drawdown reached! Drawdown: " << drawdown * 100 << "%\n";
            kill_switch_active_ = true;
            return false;
        }
    }
    return true;
}

void BybitWSClient::schedule_stats_output() {
    stats_timer_.expires_after(std::chrono::seconds(600));
    stats_timer_.async_wait([this](beast::error_code ec) {
        if (!ec) {
            if (trade_tracker_) trade_tracker_->print_performance_summary();
            schedule_stats_output();
        }
    });
}

void BybitWSClient::print_balance() {
    double balance;
    if (paper_exchange_ != nullptr) {
        balance = paper_exchange_->balance();
    } else {
        balance = api_.get_balance("USDT");
    }
    std::cout << "[BALANCE] " << balance << " USDT"
              << (paper_exchange_ ? " (PAPER)" : "") << "\n";
    if (paper_exchange_) paper_exchange_->print_snapshot();

    // ── MM metrics из БД ──
    if (mm_db_conn_ && mm_db_conn_->is_open()) {
        try {
            pqxx::work txn(*mm_db_conn_);

            // Fill rate / OTR / CTR за 24 часа
            auto r = txn.exec(
                    "SELECT "
                    "  COUNT(*) FILTER (WHERE status='FILLED') AS fills, "
                    "  COUNT(*) FILTER (WHERE status='CANCELLED') AS cancels, "
                    "  COUNT(*) FILTER (WHERE status='REJECTED') AS rejects, "
                    "  COUNT(*) AS total "
                    "FROM mm_orders "
                    "WHERE placed_time > (EXTRACT(EPOCH FROM NOW())*1000 - 86400000)::bigint");

            if (!r.empty()) {
                int fills   = r[0][0].as<int>();
                int cancels = r[0][1].as<int>();
                int rejects = r[0][2].as<int>();
                int total   = r[0][3].as<int>();

                double fill_rate = (total > 0) ? 100.0 * fills / total : 0.0;
                double otr       = (fills > 0) ? (double)total   / fills : 0.0;
                double ctr       = (fills > 0) ? (double)cancels / fills : 0.0;

                std::cout << "[MM-METRICS] 24h: fills=" << fills
                          << " cancels=" << cancels
                          << " rejects=" << rejects
                          << " placed=" << total
                          << " fill_rate=" << std::fixed << std::setprecision(1)
                          << fill_rate << "%"
                          << " OTR=" << std::setprecision(1) << otr
                          << " CTR=" << std::setprecision(1) << ctr
                          << std::defaultfloat << "\n";
            }

            // Markout hit rate + avg из mm_markouts
            auto mk = txn.exec(
                    "SELECT "
                    "  COUNT(*) FILTER (WHERE mk_1s_bps > 0)::float "
                    "    / NULLIF(COUNT(*),0) * 100 AS hr_1s, "
                    "  COUNT(*) FILTER (WHERE mk_30s_bps > 0)::float "
                    "    / NULLIF(COUNT(*),0) * 100 AS hr_30s, "
                    "  AVG(mk_1s_bps)  AS avg_1s, "
                    "  AVG(mk_30s_bps) AS avg_30s, "
                    "  COUNT(*)        AS n "
                    "FROM mm_markouts "
                    "WHERE finalized = TRUE "
                    "  AND created_at > NOW() - INTERVAL '24 hours'");

            if (!mk.empty() && !mk[0][4].is_null() && mk[0][4].as<int>() > 0) {
                double hr_1s  = mk[0][0].is_null() ? 0.0 : mk[0][0].as<double>();
                double hr_30s = mk[0][1].is_null() ? 0.0 : mk[0][1].as<double>();
                double a_1s   = mk[0][2].is_null() ? 0.0 : mk[0][2].as<double>();
                double a_30s  = mk[0][3].is_null() ? 0.0 : mk[0][3].as<double>();
                int    n      = mk[0][4].as<int>();

                std::cout << "[MM-MARKOUT] 24h: n=" << n
                          << " hit_1s=" << std::fixed << std::setprecision(1)
                          << hr_1s << "%"
                          << " hit_30s=" << hr_30s << "%"
                          << " avg_mk_1s=" << std::setprecision(2) << a_1s << "bps"
                          << " avg_mk_30s=" << a_30s << "bps"
                          << std::defaultfloat << "\n";
            }

            txn.commit();
        } catch (const std::exception& e) {
            std::cerr << "[MM-METRICS] err: " << e.what() << "\n";
        }
    }

    balance_timer_.expires_after(std::chrono::seconds(60));
    balance_timer_.async_wait([this](beast::error_code ec) {
        if (!ec) print_balance();
    });
}

OrderBook& BybitWSClient::get_orderbook(const std::string& symbol) {
    return tickers_.at(symbol)->orderbook;
}

void BybitWSClient::preload_history(const std::vector<std::string>& symbols, BybitApiClient& api) {
    std::cout << "[PRELOAD] Loading historical data for " << symbols.size() << " symbols...\n";
    int count = 0;
    for (const auto& symbol : symbols) {
        ++count;
        std::cout << "[PRELOAD] (" << count << "/" << symbols.size() << ") " << symbol << std::endl;
        auto it = tickers_.find(symbol);
        if (it == tickers_.end()) continue;
        TickerState& state = *it->second;
        try {
            auto klines = api.get_kline(symbol, "1", 200);
            for (const auto& k : klines) {
                Bar bar;
                bar.timestamp = k.open_time;
                bar.open = k.open; bar.high = k.high; bar.low = k.low;
                bar.close = k.close; bar.volume = k.volume;
                if (state.strategy && state.strategy->bar_aggregator_)
                    state.strategy->bar_aggregator_->add_historical_bar(bar);
                if (state.strategy && state.strategy->regime_filter_)
                    state.strategy->regime_filter_->on_bar_closed(bar);
                state.regime_filter.on_bar_closed(bar);
                state.bb.add_price(bar.close, bar.timestamp);
            }
        } catch (const std::exception& e) { std::cerr << "[PRELOAD] klines " << symbol << ": " << e.what() << "\n"; }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        try {
            auto trades = api.get_public_trades(symbol, 1000);
            for (const auto& t : trades) {
                state.cluster.add_historical_trade(t.timestamp, t.price, t.volume, t.is_buyer_maker);
                state.vwma.add_tick(t.price, t.volume, t.timestamp);
            }
        } catch (const std::exception& e) { std::cerr << "[PRELOAD] trades " << symbol << ": " << e.what() << "\n"; }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        try {
            auto snap = api.get_orderbook_snapshot(symbol, 50);
            state.orderbook.initialize(snap);
        } catch (const std::exception& e) { std::cerr << "[PRELOAD] orderbook " << symbol << ": " << e.what() << "\n"; }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    std::cout << "[PRELOAD] Finished loading history.\n";
}

LiquidityInfo BybitWSClient::get_symbol_liquidity(const std::string& symbol) {
    LiquidityInfo info;
    info.is_valid = false;

    auto it = tickers_.find(symbol);
    if (it == tickers_.end()) return info;

    TickerState& state = *it->second;
    auto bbo = state.orderbook.get_bbo();   // один lock — bid/ask из одного состояния
    if (!bbo.is_valid()) return info;

    const double mid = (bbo.bid + bbo.ask) / 2.0;
    info.spread_pct = (bbo.ask - bbo.bid) / mid * 100.0;
    info.depth_usdt = std::min(bbo.bid_size * bbo.bid, bbo.ask_size * bbo.ask) * 5.0;

    double volume_coins = state.cluster.get_total_volume(60);
    info.volume_60s = volume_coins * mid;

    // avg_trade_size корректно считается только если ClusterEngine
    // умеет отдавать число трейдов. Пока оставляем «средний объём на
    // 60-секундное окно» — это оценка сверху, а не средний размер трейда.
    // TODO: заменить на cluster.get_trade_count(60).
    info.avg_trade_size = volume_coins > 0.0 ? (volume_coins * mid) : 0.0;

    info.is_valid = (info.depth_usdt > 0.0 &&
                     info.spread_pct > 0.0 &&
                     info.spread_pct < 10.0);
    return info;
}

BybitWSClient::~BybitWSClient() {
    balance_timer_.cancel(); stats_timer_.cancel();
    position_update_timer_.cancel(); mm_fill_poll_timer_.cancel();
    inv_refresh_timer_.cancel(); pos_manage_timer_.cancel();
    ping_timer_.cancel(); reconnect_timer_.cancel();
    subscribe_delay_timer_.cancel();
    if (trade_tracker_) trade_tracker_->stop();
    if (trade_ws_) trade_ws_->stop();
    if (private_ws_) private_ws_->stop();
    regime_update_timer_.cancel();
}

void BybitWSClient::create_websocket() {
    ws_ = std::make_unique<websocket::stream<beast::ssl_stream<beast::tcp_stream>>>(
            net::make_strand(ioc_), ctx_);
    ws_->set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
    ws_->set_option(websocket::stream_base::decorator(
            [](websocket::request_type& req) {
                req.set(boost::beast::http::field::user_agent, "HFT-Cpp-Client");
            }));
}

void BybitWSClient::run(const std::string& host, const std::string& port, const std::string& target) {
    host_ = host;
    if (!target.empty()) ws_path_ = target;
    resolver_.async_resolve(host, port,
                            beast::bind_front_handler(&BybitWSClient::on_resolve, shared_from_this()));
}

// ... on_resolve/on_connect/on_ssl_handshake/on_handshake/on_write/on_read
//     — БЕЗ ИЗМЕНЕНИЙ, копируйте из вашего текущего файла ...

void BybitWSClient::send_subscribe_batch(size_t start_idx, size_t batch_size,
                                         size_t batch_num, size_t total_batches) {
    if (!ws_) return;
    if (start_idx >= symbols_.size()) {
        std::cout << "[SUBSCRIBE] All batches sent\n";
        return;
    }
    std::ostringstream args;
    args << "[";
    bool first = true;
    size_t end = std::min(start_idx + batch_size, symbols_.size());
    bool is_spot = (ws_path_.find("/spot") != std::string::npos);
    for (size_t j = start_idx; j < end; ++j) {
        if (!first) args << ",";
        args << "\"orderbook.50." << symbols_[j] << "\"";
        if (!is_spot) args << ",\"publicTrade." << symbols_[j] << "\"";
        first = false;
    }
    args << "]";
    std::string req_id = "sub_" + std::to_string(batch_num);
    std::string sub_msg = R"({"req_id": ")" + req_id + R"(", "op": "subscribe", "args":)" + args.str() + "}";
    ws_->text(true);
    ws_->async_write(net::buffer(sub_msg),
                     [self = shared_from_this(), start_idx, batch_size, batch_num, total_batches, end]
                             (beast::error_code ec, std::size_t) {
                         if (ec) { std::cerr << "Subscribe write error: " << ec.message() << "\n"; return; }
                         if (!self->ws_ || self->reconnecting_) return;
                         if (end < self->symbols_.size()) {
                             self->subscribe_delay_timer_.expires_after(std::chrono::seconds(1));
                             self->subscribe_delay_timer_.async_wait(
                                     [self, start_idx, batch_size, batch_num, total_batches](beast::error_code ec) {
                                         if (!ec && self->ws_ && !self->reconnecting_)
                                             self->send_subscribe_batch(start_idx + batch_size,
                                                                        batch_size, batch_num + 1, total_batches);
                                     });
                         } else {
                             std::cout << "[SUBSCRIBE] All batches sent\n";
                             self->send_ping();
                         }
                     });
}

void BybitWSClient::send_ping() {
    if (!ws_ || reconnecting_) return;
    std::string ping_msg = R"({"req_id": "ping_1", "op": "ping"})";
    ws_->text(true);
    ws_->async_write(net::buffer(ping_msg),
                     [self = shared_from_this()](beast::error_code ec, std::size_t) {
                         if (ec) return;
                         if (!self->ws_ || self->reconnecting_) return;
                         self->ping_timer_.expires_after(std::chrono::seconds(20));
                         self->ping_timer_.async_wait([self](beast::error_code ec) {
                             if (!ec && self->ws_ && !self->reconnecting_) self->send_ping();
                         });
                     });
}

auto BybitWSClient::extract_symbol(std::string_view topic) const -> std::string {
    auto last_dot = topic.find_last_of('.');
    if (last_dot == std::string_view::npos) return "";
    return std::string(topic.substr(last_dot + 1));
}

// ============================================================
// process_message — с paper hooks
// ============================================================
void BybitWSClient::process_message(const char* data, std::size_t size) {
    if (size + simdjson::SIMDJSON_PADDING > json_buffer_.capacity())
        json_buffer_.resize(size + simdjson::SIMDJSON_PADDING);
    std::memcpy(json_buffer_.data(), data, size);

    simdjson::dom::parser parser;
    simdjson::dom::element doc;
    if (parser.parse(json_buffer_.data(), size, size).get(doc) != simdjson::SUCCESS) return;

    std::string_view topic;
    if (doc["topic"].get_string().get(topic) != simdjson::SUCCESS) return;

    std::string symbol = extract_symbol(topic);
    {
        static std::unordered_map<std::string, uint64_t> msg_count;
        static uint64_t last_dump = 0;
        static uint64_t total_msgs = 0;
        msg_count[symbol]++;
        total_msgs++;
        uint64_t now_dbg = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        if (now_dbg - last_dump > 60'000) {
            // Aggregate
            uint64_t min_c = UINT64_MAX, max_c = 0;
            for (auto& [s, c] : msg_count) {
                min_c = std::min(min_c, c);
                max_c = std::max(max_c, c);
            }
            std::cout << clr::gray << fmt_ts() << clr::reset << "  "
                      << clr::cyan << "WS" << clr::reset << "  "
                      << "symbols=" << msg_count.size()
                      << "  msgs=" << total_msgs
                      << "  per_sym: " << (msg_count.empty() ? 0 : total_msgs / msg_count.size())
                      << "  min=" << (min_c == UINT64_MAX ? 0 : min_c)
                      << "  max=" << max_c
                      << "\n";
            msg_count.clear();
            total_msgs = 0;
            last_dump = now_dbg;
        }
    }
    if (symbol.empty()) return;
    auto it = tickers_.find(symbol);
    if (it == tickers_.end()) return;
    TickerState& state = *it->second;

    if (topic.find("orderbook") != std::string_view::npos) {
        simdjson::dom::element data_field;
        if (doc["data"].get(data_field) == simdjson::SUCCESS) {
            std::string type_str;
            std::string_view type_sv;
            if (doc["type"].get_string().get(type_sv) == simdjson::SUCCESS)
                type_str = std::string(type_sv);
            state.orderbook.update(data_field, type_str);

            double _bb = state.orderbook.get_best_bid();
            double _ba = state.orderbook.get_best_ask();
            if (_bb <= 0.0 || _ba <= 0.0 || _bb >= _ba) {
                static std::unordered_map<std::string, uint64_t> _bad_log;
                static std::unordered_map<std::string, uint64_t> _bad_count;
                _bad_count[symbol]++;
                uint64_t _now_dbg = ch::now_ms();
                if (_now_dbg - _bad_log[symbol] > 30'000) {
                    std::cerr << "[WS-BAD] " << symbol
                              << " type=" << type_str
                              << " bb=" << _bb << " ba=" << _ba
                              << " bad_total=" << _bad_count[symbol] << "\n";
                    _bad_log[symbol] = _now_dbg;
                }
                return;
            }

            if (signal_engine_) {
                signal_engine_->on_bybit_book(
                        symbol, _bb, _ba,
                        state.orderbook.get_best_bid_size(),
                        state.orderbook.get_best_ask_size());
            }

            if (on_book_update_cb_) on_book_update_cb_(symbol);

            if (paper_exchange_) {
                paper_exchange_->on_book_update(
                        symbol, _bb, _ba,
                        state.orderbook.get_best_bid_size(),
                        state.orderbook.get_best_ask_size());
            }

            if (ch_writer_) {
                uint64_t ts_event = 0;
                (void)doc["ts"].get_uint64().get(ts_event);
                ch_writer_->push_book(ch::VenueBybit, symbol,
                                      ch::now_ms(), ts_event,
                                      _bb, _ba,
                                      state.orderbook.get_best_bid_size(),
                                      state.orderbook.get_best_ask_size());
            }
        }
    }
    else if (topic.find("publicTrade") != std::string_view::npos) {
        simdjson::dom::element data_field;
        if (doc["data"].get(data_field) == simdjson::SUCCESS) {

            state.cluster.process_trades(data_field);
            simdjson::dom::array trades_arr;
            if (data_field.get_array().get(trades_arr) == simdjson::SUCCESS) {
                for (auto trade_elem : trades_arr) {
                    std::string_view price_str, size_str;
                    uint64_t timestamp = 0;
                    if (trade_elem["p"].get_string().get(price_str) != simdjson::SUCCESS) continue;
                    if (trade_elem["v"].get_string().get(size_str) != simdjson::SUCCESS) continue;
                    if (trade_elem["T"].get_uint64().get(timestamp) != simdjson::SUCCESS) continue;

                    double price = std::strtod(price_str.data(), nullptr);
                    double sz    = std::strtod(size_str.data(), nullptr);
                    if (price <= 0 || sz <= 0) continue;

                    std::string_view side_str;
                    bool is_buyer_maker = false;
                    if (trade_elem["S"].get_string().get(side_str) == simdjson::SUCCESS)
                        is_buyer_maker = (side_str == "Sell");
// trade_id из поля "i"
                    std::string_view tid_sv;
                    uint64_t tid_hash;
                    if (trade_elem["i"].get_string().get(tid_sv) == simdjson::SUCCESS) {
                        tid_hash = ch::fnv1a("byb|" + std::string(tid_sv));
                    } else {
                        tid_hash = ch::fnv1a("byb|" + symbol + "|" + std::to_string(timestamp)
                                             + "|" + std::to_string(price) + "|" + std::to_string(sz));
                    }

// is_through
                    double bbo_bid = state.orderbook.get_best_bid();
                    double bbo_ask = state.orderbook.get_best_ask();
                    bool is_through = false;
                    if (bbo_bid > 0 && bbo_ask > 0) {
                        if (!is_buyer_maker && price > bbo_ask) is_through = true;
                        if ( is_buyer_maker && price < bbo_bid) is_through = true;
                    }

                    if (ch_writer_) {
                        ch_writer_->push_trade(ch::VenueBybit, symbol,
                                               ch::now_ms(), timestamp,
                                               price, sz,
                                               is_buyer_maker ? -1 : +1,
                                               is_through ? 1 : 0,
                                               tid_hash,
                                               bbo_bid, bbo_ask);
                    }
                    state.vwma.add_tick(price, sz, timestamp);
                    state.bb.add_tick(price, sz, timestamp);

                    // ── cross-venue signal: Bybit trade flow ──
                    if (signal_engine_) {
                        signal_engine_->on_bybit_trade(symbol, price, sz, is_buyer_maker);
                    }

                    // ── paper exchange trade handler ──
                    if (paper_exchange_) {
                        paper_exchange_->on_trade(symbol, price, sz, is_buyer_maker);
                    }

                    // ── toxic detection ──
                    auto mm_it = mm_strategies_.find(symbol);
                    if (mm_it != mm_strategies_.end() && mm_it->second) {
                        if (!is_buyer_maker) mm_it->second->on_trade_through_ask(price, sz);
                        else                 mm_it->second->on_trade_through_bid(price, sz);
                    }
                }
            }
        }
    }
}

auto BybitWSClient::get_current_price(const std::string& symbol) -> double {
    auto it = tickers_.find(symbol);
    if (it == tickers_.end()) return 0.0;
    return it->second->orderbook.get_mid_price();
}

void BybitWSClient::subscribe_topics() {
    const size_t batch_size = 10;
    size_t total_batches = (symbols_.size() + batch_size - 1) / batch_size;
    send_subscribe_batch(0, batch_size, 1, total_batches);
}

void BybitWSClient::reconnect() {
    if (reconnecting_) return;
    reconnecting_ = true;
    reconnect_attempts_++;
    if (reconnect_attempts_ > MAX_RECONNECT_ATTEMPTS) {
        std::cerr << "Max reconnect attempts reached. Giving up.\n";
        return;
    }
    std::cout << "[RECONNECT] Attempt " << reconnect_attempts_ << "...\n";
    ping_timer_.cancel();
    subscribe_delay_timer_.cancel();
    if (ws_) {
        beast::error_code ec;
        if (ws_->is_open()) ws_->close(websocket::close_code::normal, ec);
        ws_.reset();
    }
    buffer_.clear();
    reconnect_timer_.expires_after(std::chrono::seconds(RECONNECT_DELAY_SEC));
    reconnect_timer_.async_wait(
            beast::bind_front_handler(&BybitWSClient::on_reconnect_timer, shared_from_this()));
}

void BybitWSClient::on_reconnect_timer(beast::error_code ec) {
    if (ec) return;
    std::cout << "[RECONNECT] Reconnecting...\n";
    reconnecting_ = false;
    create_websocket();
    resolver_.async_resolve(host_, "443",
                            beast::bind_front_handler(&BybitWSClient::on_resolve, shared_from_this()));
}

void BybitWSClient::on_paper_fill(const arb::ExchangeExecution& e) {
    auto it = mm_strategies_.find(e.symbol);
    if (it == mm_strategies_.end() || !it->second) return;

    // 1) MM.on_order_filled (обновляет inventory)
    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    it->second->on_order_filled(e.order_id, now_ms, e.exec_price,
                                e.exec_qty, e.side, e.is_full_fill);

    // 2) DB log (тот же путь что и live)
    if (mm_db_conn_ && mm_db_conn_->is_open()) {
        try {
            pqxx::work txn(*mm_db_conn_);
            auto cur = txn.exec(
                    "SELECT fill_qty, fill_price FROM mm_orders "
                    "WHERE order_id = $1 AND status IN ('OPEN','PARTIALLY_FILLED') "
                    "FOR UPDATE",
                    pqxx::params{e.order_id});
            if (!cur.empty()) {
                double old_qty   = cur[0]["fill_qty"].is_null() ? 0.0 : cur[0]["fill_qty"].as<double>();
                double old_price = cur[0]["fill_price"].is_null() ? 0.0 : cur[0]["fill_price"].as<double>();
                double new_qty   = old_qty + e.exec_qty;
                double new_price = (old_qty > 0 && old_price > 0)
                                   ? (old_price*old_qty + e.exec_price*e.exec_qty) / new_qty
                                   : e.exec_price;
                const char* status_str = e.is_full_fill ? "FILLED" : "PARTIALLY_FILLED";
                txn.exec(
                        "UPDATE mm_orders SET "
                        " status=$6, filled_time=$1, fill_price=$2, "
                        " fill_qty=$3, fee=COALESCE(fee,0)+$4, exec_type='Maker', "
                        " is_partial=$7, order_age_ms=$1 - placed_time "
                        "WHERE order_id=$5",
                        pqxx::params{now_ms, new_price, new_qty, e.exec_fee, e.order_id,
                                     status_str, !e.is_full_fill});
                txn.commit();
            }
        } catch (const std::exception& ex) {
            std::cerr << "[MM-DB] paper fill err: " << ex.what() << "\n";
        }
    }
}

// ============================================================
// Async WS chain — эти 6 методов должны быть в файле
// ============================================================
void BybitWSClient::on_resolve(beast::error_code ec,
                               const tcp::resolver::results_type& results) {
    if (ec) { std::cerr << clr::red << "[WS] resolve failed: " << ec.message() << clr::reset << "\n"; reconnect(); return; }
    if (ec) { reconnect(); return; }
    auto& tcp_stream = ws_->next_layer().next_layer();
    net::async_connect(tcp_stream.socket(), results,
                       beast::bind_front_handler(&BybitWSClient::on_connect, shared_from_this()));
}

void BybitWSClient::on_connect(beast::error_code ec,
                               const tcp::resolver::results_type::endpoint_type&) {
    if (ec) { std::cerr << clr::red << "[WS] connect failed: " << ec.message() << clr::reset << "\n"; reconnect(); return; }
    if (!SSL_set_tlsext_host_name(ws_->next_layer().native_handle(), host_.c_str())) {
        beast::error_code sni_ec{static_cast<int>(::ERR_get_error()),
                                 boost::asio::error::get_ssl_category()};
        std::cerr << "SNI setup error: " << sni_ec.message() << "\n";
        reconnect();
        return;
    }
    ws_->next_layer().async_handshake(
            ssl::stream_base::client,
            beast::bind_front_handler(&BybitWSClient::on_ssl_handshake, shared_from_this()));
}

void BybitWSClient::on_ssl_handshake(beast::error_code ec) {
    if (ec) { std::cerr << clr::red << "[WS] ssl failed: " << ec.message() << clr::reset << "\n"; reconnect(); return; }
    std::cout << clr::bright_green << "✓ Bybit WS connected"
              << " (" << symbols_.size() << " symbols)" << clr::reset << "\n";
    ws_->async_handshake(host_, ws_path_,
                         beast::bind_front_handler(&BybitWSClient::on_handshake,
                                                   shared_from_this()));
}

void BybitWSClient::on_handshake(beast::error_code ec) {
    if (ec) {
        std::cerr << "WS Handshake error: " << ec.message() << "\n";
        reconnect();
        return;
    }
    reconnect_attempts_ = 0;

    if (!auxiliary_) {
        for (auto& [sym, mm] : mm_strategies_) {
            if (mm) mm->reset_vol_estimator();
        }
    }

    std::cout << "Connected to Bybit! Subscribing to " << symbols_.size() << " symbols...\n";

    ws_->async_read(buffer_,
                    beast::bind_front_handler(&BybitWSClient::on_read, shared_from_this()));
    subscribe_topics();
}

void BybitWSClient::on_write(beast::error_code ec, std::size_t /*bytes_transferred*/) {
    if (ec) std::cerr << "[WS] Write error: " << ec.message() << "\n";
}

void BybitWSClient::on_read(beast::error_code ec, std::size_t bytes_transferred) {
    if (ec) {
        std::cerr << "Read error: " << ec.message() << "\n";
        reconnect();
        return;
    }
    if (!ws_) return;

    auto buffers = buffer_.data();
    if (buffers.size() > 0) {
        const char* data = static_cast<const char*>(buffers.data());
        process_message(data, buffers.size());
    }
    buffer_.consume(bytes_transferred);
    ws_->async_read(buffer_,
                    beast::bind_front_handler(&BybitWSClient::on_read, shared_from_this()));
}

void BybitWSClient::init_trade_ws(const std::string& host) {
    trade_ws_ = std::make_shared<BybitTradeWSClient>(
            ioc_, ctx_, api_,
            [this](const BybitTradeWSClient::Ack& ack) { on_trade_ack(ack); });
    trade_ws_->run(host);
}

void BybitWSClient::init_private_ws(const std::string& host) {
    private_ws_ = std::make_shared<BybitPrivateWSClient>(
            ioc_, ctx_,
            api_.get_api_key(), api_.get_api_secret(),
            [this](const std::string& /*sym*/, const std::string& /*side*/,
                   double /*sz*/, double /*avg*/, double /*pnl*/) {
                // position update — при желании обновляйте локальный кэш
            },
            [this](const std::string& exec_id, const std::string& order_id,
                   const std::string& symbol, const std::string& side,
                   double price, double qty, double fee,
                   const std::string& fee_ccy, uint64_t exec_time, bool is_maker) {
                on_private_execution(exec_id, order_id, symbol, side,
                                     price, qty, fee, fee_ccy, exec_time, is_maker);
            },
            [this](const std::string& oid, const std::string& link_id,
                   const std::string& sym, const std::string& status,
                   const std::string& reject) {
                on_private_order(oid, link_id, sym, status, reject);
            },
            []() { std::cout << "[PrivWS] subscribed\n"; });
    private_ws_->run(host);
}

void BybitWSClient::on_trade_ack(const BybitTradeWSClient::Ack& ack) {
    if (ack.ret_code != 0) {
        std::cerr << "[TradeWS] order rejected: code=" << ack.ret_code
                  << " msg=" << ack.ret_msg
                  << " reqId=" << ack.req_id << "\n";
        return;
    }
    // Можно логировать orderLinkId → orderId, если используете корреляцию
}


void BybitWSClient::on_private_execution(
        const std::string& exec_id, const std::string& order_id,
        const std::string& symbol, const std::string& side,
        double price, double qty, double fee,
        const std::string& fee_currency, uint64_t exec_time, bool is_maker) {

// NOTE: paper-fallback вызывает это из WS-треда. Live идёт через on_private_execution
// на strand'е pos_manage_timer_. Для paper TODO: сериализовать через post(exec_).
    // dedup (WS может переотправить при реконнекте)
    if (processed_execution_ids_.count(exec_id)) return;
    processed_execution_ids_.insert(exec_id);
    if (processed_execution_ids_.size() > 50000)
        processed_execution_ids_.clear();   // простой prune; при желании — по времени

    auto mm_it = mm_strategies_.find(symbol);
    if (mm_it == mm_strategies_.end() || !mm_it->second) return;

    // статус ордера: если это последний филл, order-топик пришлёт Filled;
    // здесь мы не дергаем REST, чтобы не блокировать event loop.
    // Для БД пишем PARTIALLY_FILLED и позволим order-топику обновить на FILLED.
    std::string new_status = "PARTIALLY_FILLED";

    if (mm_db_conn_ && mm_db_conn_->is_open()) {
        try {
            pqxx::work txn(*mm_db_conn_);
            auto cur = txn.exec(
                    "SELECT fill_qty, fill_price FROM mm_orders "
                    "WHERE order_id = $1 AND status IN ('OPEN','PARTIALLY_FILLED') FOR UPDATE",
                    pqxx::params{order_id});

            if (cur.empty()) {
                txn.abort();
            } else {
                double old_qty   = cur[0]["fill_qty"].is_null() ? 0.0 : cur[0]["fill_qty"].as<double>();
                double old_price = cur[0]["fill_price"].is_null() ? 0.0 : cur[0]["fill_price"].as<double>();
                double new_qty   = old_qty + qty;
                double new_price = (old_qty > 0 && old_price > 0)
                                   ? (old_price * old_qty + price * qty) / new_qty
                                   : price;

                txn.exec(
                        "UPDATE mm_orders SET "
                        " status = $1, filled_time = COALESCE(filled_time, $2), "
                        " fill_price = $3, fill_qty = $4, fee = COALESCE(fee,0) + $5, "
                        " exec_id = $6, exec_type = $7, is_partial = true, "
                        " order_age_ms = $2 - placed_time "
                        "WHERE order_id = $8 AND status IN ('OPEN','PARTIALLY_FILLED')",
                        pqxx::params{new_status, exec_time, new_price, new_qty, fee,
                                     exec_id, is_maker ? "Maker" : "Taker", order_id});
                txn.commit();
            }
        } catch (const std::exception& ex) {
            std::cerr << "[MM-DB] WS execution update error: " << ex.what() << "\n";
        }
    }

    // КРИТИЧНО: on_order_filled вызывается сразу, без 15-секундной задержки
    mm_it->second->on_order_filled(order_id, exec_time, price, qty, side);

    std::cout << "[FILL-WS] " << symbol << " | " << side
              << " | " << (is_maker ? "Maker" : "Taker")
              << " | price=" << price << " | qty=" << qty
              << " | fee=" << fee << " " << fee_currency
              << " | order=" << order_id << "\n";
}

void BybitWSClient::on_private_order(
        const std::string& order_id, const std::string& order_link_id,
        const std::string& symbol, const std::string& status,
        const std::string& reject_reason) {

    if (status == "Filled") {
        if (mm_db_conn_ && mm_db_conn_->is_open()) {
            try {
                pqxx::work txn(*mm_db_conn_);
                txn.exec(
                        "UPDATE mm_orders SET status='FILLED', is_partial=false "
                        "WHERE order_id=$1 AND status IN ('OPEN','PARTIALLY_FILLED')",
                        pqxx::params{order_id});
                txn.commit();
            } catch (const std::exception& ex) {
                std::cerr << "[MM-DB] order Filled update: " << ex.what() << "\n";
            }
        }
        auto it = mm_strategies_.find(symbol);
        if (it != mm_strategies_.end() && it->second)
            it->second->on_order_filled(order_id, 0, 0, 0, "");  // 0 — no-op fill
    }
    else if (status == "Cancelled" || status == "Rejected") {
        if (mm_db_conn_ && mm_db_conn_->is_open()) {
            try {
                pqxx::work txn(*mm_db_conn_);
                txn.exec(
                        "UPDATE mm_orders SET status=$1, reject_reason=$2 "
                        "WHERE order_id=$3 AND status IN ('OPEN','PARTIALLY_FILLED')",
                        pqxx::params{status, reject_reason, order_id});
                txn.commit();
            } catch (const std::exception& ex) {
                std::cerr << "[MM-DB] order cancel/reject update: " << ex.what() << "\n";
            }
        }
        std::cout << "[ORDER-WS] " << symbol << " " << status
                  << " order=" << order_id << " reason=" << reject_reason << "\n";
    }
}

int BybitWSClient::count_symbols_with_book() const {
    int n = 0;
    for (const auto& [sym, state] : tickers_) {
        if (state->orderbook.get_mid_price() > 0.0) ++n;
    }
    return n;
}

void BybitWSClient::disable_symbol(const std::string& symbol) {
    auto it = mm_strategies_.find(symbol);
    if (it == mm_strategies_.end()) {
        // MM уже нет — ничего не делаем (норма если дважды disable)
        return;
    }

    if (it->second) {
        it->second->shutdown_gracefully("HOT_DISABLE");
    }

    mm_strategies_.erase(it);
    std::cout << "[WS] DISABLED " << symbol
              << " (hot-reload, MM count=" << mm_strategies_.size() << ")\n";
}

void BybitWSClient::enable_symbol(const std::string& symbol) {
    if (has_mm(symbol)) return;

    // 1) Есть ли WS-подписка? (tickers_ создан в конструкторе из symbols_)
    auto tit = tickers_.find(symbol);
    if (tit == tickers_.end()) {
        std::cerr << "[WS] enable " << symbol
                  << " FAILED: ticker not subscribed (not in symbols_ at startup). "
                     "Требуется рестарт с добавлением в top-N.\n";
        return;
    }

    // 2) Есть ли registry?
    if (!mm_registry_) {
        std::cerr << "[WS] enable " << symbol
                  << " FAILED: mm_registry_ is null\n";
        return;
    }

    // 3) Разрешён ли в registry (кэш может быть устаревшим — reload уже был)
    if (!mm_registry_->is_enabled(symbol)) {
        std::cerr << "[WS] enable " << symbol
                  << " FAILED: registry says disabled "
                     "(race: reload увидел enabled, но cache уже откатился?)\n";
        return;
    }

    // 4) Есть ли exchange_
    if (!exchange_) {
        std::cerr << "[WS] enable " << symbol
                  << " FAILED: exchange_ not set (init_mm_strategies not called)\n";
        return;
    }

    try {
        size_t before = mm_strategies_.size();
        create_mm_for_symbol(symbol);
        size_t after = mm_strategies_.size();
        if (after > before) {
            std::cout << "[WS] ENABLED " << symbol
                      << " (hot-reload, MM count=" << after << ")\n";
        } else {
            std::cerr << "[WS] enable " << symbol
                      << " returned WITHOUT creating MM (count stayed "
                      << before << "). Check earlier logs.\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "[WS] enable " << symbol
                  << " FAILED (exception): " << e.what() << "\n";
    }
}

std::vector<std::string> BybitWSClient::currently_trading_symbols() const {
    std::vector<std::string> out;
    out.reserve(mm_strategies_.size());
    for (const auto& [sym, mm] : mm_strategies_) {
        out.push_back(sym);
    }
    return out;
}

void BybitWSClient::start_regime_update() {
    regime_update_timer_.expires_after(std::chrono::seconds(30));   // первый раз через 30с
    regime_update_timer_.async_wait([this](beast::error_code ec) {
        if (ec) return;
        do_regime_update();
        regime_update_timer_.expires_after(std::chrono::minutes(5));
        regime_update_timer_.async_wait([this](beast::error_code ec2) {
            if (ec2) return;
            do_regime_update();
            start_regime_update();   // рекурсия для следующего цикла
        });
    });
}

void BybitWSClient::do_regime_update() {
    if (!mm_db_conn_ || !mm_db_conn_->is_open()) return;

    struct SideStats {
        double med_mk_30s   = 0.0;
        double med_mk_1s    = 0.0;
        double hit_pct_1s   = 0.0;
        int    n            = 0;
    };
    // key: symbol → [Buy, Sell]
    std::unordered_map<std::string, std::array<SideStats, 2>> db;

    // ─── один запрос на всё ───
    try {
        pqxx::work txn(*mm_db_conn_);
        auto r = txn.exec(
                "SELECT symbol, side,"
                "  COALESCE(AVG(mk_30s_bps), 0) AS med_mk_30s,"
                "  COALESCE(AVG(mk_1s_bps),  0) AS med_mk_1s,"
                "  COALESCE(100.0 * COUNT(*) FILTER (WHERE mk_1s_bps > 0)"
                "           / NULLIF(COUNT(*), 0), 0) AS hit_pct,"
                "  COUNT(*) AS n"
                " FROM mm_markouts"
                " WHERE finalized = TRUE"
                "   AND created_at > NOW() - INTERVAL '4 hours'"
                " GROUP BY symbol, side");

        for (const auto& row : r) {
            std::string sym  = row["symbol"].as<std::string>();
            std::string side = row["side"].as<std::string>();
            int idx = (side == "Buy") ? 0 : 1;
            auto& x = db[sym][idx];
            x.med_mk_30s = row["med_mk_30s"].as<double>();
            x.med_mk_1s  = row["med_mk_1s"].as<double>();
            x.hit_pct_1s = row["hit_pct"].as<double>();
            x.n          = row["n"].as<int>();
        }
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[REGIME] DB query error: " << e.what() << "\n";
        return;
    }

    // ─── обновить каждый MM ───
    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

    int bid_modes[4] = {0, 0, 0, 0};
    int ask_modes[4] = {0, 0, 0, 0};

    for (auto& [sym, mm_ptr] : mm_strategies_) {
        if (!mm_ptr) continue;

        // Market-derived данные
        double depth_usdt = 0.0, spread_frac = 0.0, vpin = 0.0;
        auto tit = tickers_.find(sym);
        if (tit != tickers_.end()) {
            vpin = tit->second->cluster.get_vpin(20);
            auto liq = get_symbol_liquidity(sym);
            if (liq.is_valid) {
                depth_usdt  = liq.depth_usdt;
                spread_frac = liq.spread_pct / 100.0;
            }
        }

        // Собрать кандидатов per side
        mm::SymModeSide cand_bid, cand_ask;
        auto it = db.find(sym);
        if (it != db.end()) {
            const auto& buy_s  = it->second[0];
            const auto& sell_s = it->second[1];

            cand_bid.med_mk_30s    = buy_s.med_mk_30s;
            cand_bid.med_mk_1s     = buy_s.med_mk_1s;
            cand_bid.hit_pct_1s    = buy_s.hit_pct_1s;
            cand_bid.n_markouts_4h = buy_s.n;

            cand_ask.med_mk_30s    = sell_s.med_mk_30s;
            cand_ask.med_mk_1s     = sell_s.med_mk_1s;
            cand_ask.hit_pct_1s    = sell_s.hit_pct_1s;
            cand_ask.n_markouts_4h = sell_s.n;
        }
        cand_bid.mode = mm::classify_side(cand_bid, depth_usdt, spread_frac, vpin);
        cand_ask.mode = mm::classify_side(cand_ask, depth_usdt, spread_frac, vpin);

        // Гистерезис
        const auto& prev = mm_ptr->regime();
        cand_bid = mm::apply_hysteresis(prev.bid, cand_bid, now_ms);
        cand_ask = mm::apply_hysteresis(prev.ask, cand_ask, now_ms);

        mm::SymModeSnapshot snap;
        snap.bid          = cand_bid;
        snap.ask          = cand_ask;
        snap.depth_usdt   = depth_usdt;
        snap.spread_frac  = spread_frac;
        snap.vpin_now     = vpin;
        snap.updated_ms   = now_ms;
        snap.has_data     = (cand_bid.n_markouts_4h >= 20) || (cand_ask.n_markouts_4h >= 20);
        // fill_rate_1h — опционально, не влияет на classify_side

        mm_ptr->set_regime_snapshot(snap);

        bid_modes[static_cast<int>(snap.bid.mode)]++;
        ask_modes[static_cast<int>(snap.ask.mode)]++;
    }

    std::cout << "[REGIME] BID:"
              << " Normal="   << bid_modes[0]
              << " Tier1="    << bid_modes[1]
              << " Toxic="    << bid_modes[2]
              << " Illiquid=" << bid_modes[3]
              << " | ASK:"
              << " Normal="   << ask_modes[0]
              << " Tier1="    << ask_modes[1]
              << " Toxic="    << ask_modes[2]
              << " Illiquid=" << ask_modes[3]
              << "\n";
}

bool BybitWSClient::has_mm(const std::string& symbol) const {
    return mm_strategies_.count(symbol) > 0;
}