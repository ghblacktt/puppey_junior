#include "screener.h"
#include "time_decay_stats.h" // <-- Подключаем наш новый класс
#include <pqxx/pqxx>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <sstream>

namespace {
    inline uint64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
    }
}

Screener::Screener(boost::asio::io_context& ioc, const std::string& conn_str)
        : ioc_(ioc),
          conn_str_(conn_str),
          snapshot_timer_(ioc),
          promote_timer_(ioc),
          pg_reval_timer_(ioc) {}

ScreenerGlobalConfig Screener::config() const {
    std::lock_guard lk(mtx_);
    return cfg_;
}

void Screener::load(BybitApiClient& api) {
    try {
        pqxx::connection conn(conn_str_);
        pqxx::work txn(conn);
        auto r = txn.exec(
                "SELECT enabled, scan_top_n, target_half_bps, window_sec, "
                "       snapshot_interval_sec, promote_interval_sec, "
                "       min_shadow_fills, min_net_edge_bps, min_update_rate_hz, "
                "       min_bbo_notional_usd, max_spread_p75_bps, auto_promote, "
                "       use_per_symbol_params, default_max_hold_sec, shadow_touch_volume_usd "
                "FROM mm_screener_config WHERE id = 1");
        if (!r.empty()) {
            cfg_.enabled               = r[0][0].as<bool>();
            cfg_.scan_top_n            = r[0][1].as<int>();
            cfg_.target_half_bps       = r[0][2].as<double>();
            cfg_.window_sec            = r[0][3].as<int>();
            cfg_.snapshot_interval_sec = r[0][4].as<int>();
            cfg_.promote_interval_sec  = r[0][5].as<int>();
            cfg_.min_shadow_fills      = r[0][6].as<int>();
            cfg_.min_net_edge_bps      = r[0][7].as<double>();
            cfg_.min_update_rate_hz    = r[0][8].as<double>();
            cfg_.min_bbo_notional_usd  = r[0][9].as<double>();
            cfg_.max_spread_p75_bps    = r[0][10].as<double>();
            cfg_.auto_promote          = r[0][11].as<bool>();
            if (!r[0][12].is_null()) cfg_.use_per_symbol_params  = r[0][12].as<bool>();
            if (!r[0][13].is_null()) cfg_.default_max_hold_sec   = r[0][13].as<int>();
            if (!r[0][14].is_null()) cfg_.shadow_touch_volume_usd = r[0][14].as<double>();
        }
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[Screener] config load failed: " << e.what() << " — using defaults\n";
    }

    auto syms = api.get_top_liquid_symbols(cfg_.scan_top_n);
    try {
        pqxx::connection conn(conn_str_);
        pqxx::work txn(conn);
        auto r = txn.exec("SELECT symbol FROM mm_symbols WHERE enabled = TRUE");
        for (auto row : r) {
            syms.push_back(row[0].as<std::string>());
        }
        txn.commit();
    } catch (...) {}

    std::sort(syms.begin(), syms.end());
    syms.erase(std::unique(syms.begin(), syms.end()), syms.end());

    std::lock_guard lk(mtx_);
    symbols_ = syms;
    for (const auto& s : syms) {
        auto& st = stats_[s];
        st.symbol = s;
    }
    std::cout << "[Screener] loaded " << symbols_.size()
              << " symbols (top-" << cfg_.scan_top_n << "), target_half="
              << cfg_.target_half_bps << " bps\n";
}

// ── Вычисление метрик (оставлены как fallback, если понадобятся) ──
double Screener::median_of(std::deque<double> buf) {
    if (buf.empty()) return 0.0;
    std::sort(buf.begin(), buf.end());
    size_t n = buf.size();
    return (n % 2 == 0) ? (buf[n/2 - 1] + buf[n/2]) / 2.0 : buf[n/2];
}

void Screener::on_book_update(const std::string& symbol,
                              double best_bid, double best_ask,
                              double bid_size, double ask_size) {

    static std::atomic<uint64_t> _entry_count{0};
    uint64_t n = ++_entry_count;
    if (n <= 10 || n % 1000 == 0) {
        std::cout << "[SCREENER-ENTRY] n=" << n << " sym=" << symbol << "\n";
    }

    if (best_bid <= 0.0 || best_ask <= 0.0 || best_ask <= best_bid) return;

    std::lock_guard lk(mtx_);
    auto it = stats_.find(symbol);
    if (it == stats_.end()) return;
    auto& s = it->second;

    const double mid         = (best_bid + best_ask) / 2.0;
    const double spread_bps  = (best_ask - best_bid) / mid * 10000.0;
    const double bbo_notional = std::min(bid_size * best_bid, ask_size * best_ask);
    const uint64_t t = now_ms();

    s.price = mid;
    s.last_update_ms = t;
    if (s.first_update_ms == 0) s.first_update_ms = t;

    // ═══════════════════════════════════════════════════════════
    // FIX: Используем TimeDecayStats вместо ручных std::deque
    // ═══════════════════════════════════════════════════════════
    s.spread_stats->add(spread_bps, t);
    s.bbo_notional_stats->add(bbo_notional, t);
    s.update_rate_stats->add(1.0, t); // Каждое обновление = 1 событие

    // ═══ Through detection ═══
    if (s.last_mid_for_through > 0) {
        const double move_bps = std::abs(mid - s.last_mid_for_through) / s.last_mid_for_through * 10000.0;
        if (move_bps > 3.0) {
            s.through_stats->add(1.0, t); // Добавляем событие прошивки
        }
    }
    s.last_mid_for_through = mid;

    // ═══ Shadow Order Logic ═══
    double shadow_min_hs_bps;
    if (registry_ && cfg_.use_per_symbol_params) {
        shadow_min_hs_bps = registry_->effective_min_hs_bps(symbol, mid);
    } else {
        double current_med = s.spread_stats->weighted_percentile(0.5, t);
        shadow_min_hs_bps = std::clamp(current_med * 0.5, 2.0, 12.0);
    }

    const double H = shadow_min_hs_bps / 10000.0;
    const double shadow_bid = mid * (1.0 - H);
    const double shadow_ask = mid * (1.0 + H);

    constexpr uint64_t SHADOW_TIMEOUT_MS = 30'000;
    if (s.shadow_bid_pending && s.shadow_bid_placed_ms > 0 && (t - s.shadow_bid_placed_ms) > SHADOW_TIMEOUT_MS) {
        shadow_fm_.cancel(s.shadow_bid_oid_);
        s.shadow_bid_pending = false; s.shadow_bid_placed_ms = 0; s.shadow_bid_oid_.clear();
    }
    if (s.shadow_ask_pending && s.shadow_ask_placed_ms > 0 && (t - s.shadow_ask_placed_ms) > SHADOW_TIMEOUT_MS) {
        shadow_fm_.cancel(s.shadow_bid_oid_);
        s.shadow_ask_pending = false; s.shadow_ask_placed_ms = 0; s.shadow_bid_oid_.clear();
    }

    static std::atomic<uint64_t> g_shadow_seq{0};
    auto register_shadow = [&](const std::string& tag, const std::string& side, double px, bool& pending, uint64_t& placed_ms, std::string& oid_out, double& price_out) {
        oid_out = symbol + "_" + tag + "_" + std::to_string(g_shadow_seq.fetch_add(1, std::memory_order_relaxed));
        mm::FillModel::Order o;
        o.order_id = oid_out; o.symbol = symbol; o.side = side; o.price = px; o.qty = 1.0; o.placed_ms = t;
        shadow_fm_.register_order(std::move(o), best_bid, best_ask, bid_size, ask_size);
        pending = true; placed_ms = t; price_out = px;
    };

    if (!s.shadow_bid_pending && s.shadow_bid_placed_ms == 0) { // Упрощенная проверка
        register_shadow("sB", "Buy", shadow_bid, s.shadow_bid_pending, s.shadow_bid_placed_ms, s.shadow_bid_oid_, s.shadow_bid_price);
    }
    if (!s.shadow_ask_pending && s.shadow_ask_placed_ms == 0) {
        register_shadow("sA", "Sell", shadow_ask, s.shadow_ask_pending, s.shadow_ask_placed_ms, s.shadow_bid_oid_, s.shadow_ask_price);
    }

    // Проверка adverse для уже исполненных (упрощенная логика из твоего кода)
    const uint64_t ADVERSE_WINDOW_MS = 5'000;
    if (s.shadow_bid_fill_ms > 0 && (t - s.shadow_bid_fill_ms) >= ADVERSE_WINDOW_MS) {
        const double adv_bps = (s.shadow_bid_price - mid) / s.shadow_bid_price * 10000.0;
        s.adverse_stats->add(adv_bps, t); // <-- Используем TimeDecayStats
        s.shadow_bid_fill_ms = 0; // Сброс для следующего цикла
    }
    if (s.shadow_ask_fill_ms > 0 && (t - s.shadow_ask_fill_ms) >= ADVERSE_WINDOW_MS) {
        const double adv_bps = (mid - s.shadow_ask_price) / s.shadow_ask_price * 10000.0;
        s.adverse_stats->add(adv_bps, t); // <-- Используем TimeDecayStats
        s.shadow_ask_fill_ms = 0;
    }
}

void Screener::on_trade(const std::string& symbol, double px, double qty, bool is_buyer_maker, uint64_t ts_ms) {
    if (px <= 0.0 || qty <= 0.0) return;

    std::lock_guard lk(mtx_);
    shadow_fm_.on_trade(symbol, px, qty, is_buyer_maker, ts_ms);
    auto fills = shadow_fm_.drain_fills();
    if (fills.empty()) return;

    auto it = stats_.find(symbol);
    if (it == stats_.end()) return;
    auto& s = it->second;

    for (const auto& f : fills) {
        const bool is_bid = (f.side == "Buy");
        if (is_bid && s.shadow_bid_placed_ms > 0) {
            s.shadow_bid_fills++;
            s.shadow_bid_fill_ms = f.exec_ms; // Запоминаем для последующего расчета adverse
            s.shadow_bid_pending = false;
            s.shadow_bid_oid_.clear();
            s.shadow_fills_stats->add(1.0, f.exec_ms); // <-- Фиксируем филл
            shadow_fm_.cancel(f.order_id);
        } else if (!is_bid && s.shadow_ask_placed_ms > 0) {
            s.shadow_ask_fills++;
            s.shadow_ask_fill_ms = f.exec_ms;
            s.shadow_ask_pending = false;
            s.shadow_bid_oid_.clear();
            s.shadow_fills_stats->add(1.0, f.exec_ms); // <-- Фиксируем филл
            shadow_fm_.cancel(f.order_id);
        }
    }
}

// ── Compute snapshot ──
void Screener::compute_snapshot_metrics(SymbolScreenerStats& s) const {
    const uint64_t now = now_ms();

    // ═══════════════════════════════════════════════════════════
    // 1. Извлекаем метрики из TimeDecayStats (автоматическое затухание)
    // ═══════════════════════════════════════════════════════════
    s.last_spread_median = s.spread_stats->weighted_percentile(0.5, now);
    s.last_spread_p25    = s.spread_stats->weighted_percentile(0.25, now);
    s.last_spread_p75    = s.spread_stats->weighted_percentile(0.75, now);

    s.last_bbo_notional  = s.bbo_notional_stats->weighted_mean(now);

    // Эффективное количество событий в окне (сумма весов)
    double window_updates = s.update_rate_stats->weighted_sum(now);
    double window_fills   = s.shadow_fills_stats->weighted_sum(now);
    double through_count  = s.through_stats->weighted_sum(now);

    s.last_shadow_fills  = static_cast<int>(std::round(window_fills));
    s.last_update_rate   = window_updates / (cfg_.window_sec); // событий в секунду

    // ═══════════════════════════════════════════════════════════
    // 2. Расчет Edge (ИСПРАВЛЕННАЯ ЛОГИКА)
    // ═══════════════════════════════════════════════════════════
    double window_adv_avg = s.adverse_stats->weighted_mean(now);

    // Чистая оценка альфы
    double alpha_estimate = -window_adv_avg;

    // FIX: Through ratio считается корректно из взвешенных сумм
    const double through_ratio = (window_updates > 0) ? std::min(1.0, through_count / window_updates) : 0.0;
    double toxic_penalty = through_ratio * 5.0;

    // FIX: SHRINKAGE применяется ТОЛЬКО к alpha_estimate, а не к комиссиям!
    constexpr double kShrinkK = 10.0;
    const double confidence = (window_fills > 0.0) ? (window_fills / (window_fills + kShrinkK)) : 0.0;
    double shrunk_alpha = alpha_estimate * confidence;

    // Штраф за тонкий стакан
    double thin_penalty = 0.0;
    if (s.last_bbo_notional < 500.0) {
        thin_penalty = ((500.0 - s.last_bbo_notional) / 500.0) * 10.0;
    }

    // Финальный скор
    s.last_net_edge = shrunk_alpha - toxic_penalty - thin_penalty;
    s.last_snapshot_ms = now;

    // ═══ EXPONENTIAL DECAY для гистерезиса (сглаживание снапшотов) ═══
    const double alpha = 0.3;
    if (s.ema_last_update_ms == 0) {
        s.ema_edge = s.last_net_edge;
    } else {
        s.ema_edge = alpha * s.last_net_edge + (1.0 - alpha) * s.ema_edge;
    }
    s.ema_last_update_ms = now;
}

void Screener::schedule_snapshot() {
    snapshot_timer_.expires_after(std::chrono::seconds(cfg_.snapshot_interval_sec));
    snapshot_timer_.async_wait([this](boost::system::error_code ec) {
        if (ec) return;
        do_snapshot();
        schedule_snapshot();
    });
}

void Screener::do_snapshot() {
    std::vector<SymbolScreenerStats> batch;
    {
        std::lock_guard lk(mtx_);
        batch.reserve(stats_.size());
        for (auto& [sym, s] : stats_) {
            compute_snapshot_metrics(s);
            batch.push_back(s);
        }
    }
    if (!batch.empty()) persist_snapshot(batch);
}

void Screener::persist_snapshot(const std::vector<SymbolScreenerStats>& batch) {
    try {
        pqxx::connection conn(conn_str_);
        pqxx::work txn(conn);
        std::string sql = "INSERT INTO mm_screening_stats (symbol, run_tag, spread_median_bps, spread_p25_bps, spread_p75_bps, sigma_bps, bbo_notional_usd, update_rate_hz, target_half_bps, shadow_fills, shadow_adverse_bps, shadow_net_edge_bps) VALUES ";
        std::ostringstream rows;
        for (size_t i = 0; i < batch.size(); ++i) {
            const auto& s = batch[i];
            if (i > 0) rows << ",";
            rows << "(" << txn.quote(s.symbol) << "," << txn.quote(run_tag_) << ","
                 << s.last_spread_median << "," << s.last_spread_p25 << "," << s.last_spread_p75 << ","
                 << s.last_sigma_bps << "," << s.last_bbo_notional << "," << s.last_update_rate << ","
                 << cfg_.target_half_bps << "," << s.last_shadow_fills << "," << s.adverse_stats->weighted_mean(s.last_snapshot_ms) << "," << s.last_net_edge << ")";
        }
        txn.exec(sql + rows.str());
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[Screener] persist snapshot failed: " << e.what() << "\n";
    }
}

void Screener::schedule_promote() {
    promote_timer_.expires_after(std::chrono::seconds(cfg_.promote_interval_sec));
    promote_timer_.async_wait([this](boost::system::error_code ec) {
        if (ec) return;
        do_promote();
        schedule_promote();
    });
}

void Screener::do_promote() {
    if (!cfg_.auto_promote) return;

    std::vector<std::pair<std::string, double>> to_promote;
    std::vector<std::pair<std::string, double>> to_demote;
    int n_active = 0, n_cand = 0, n_ready = 0, n_demote = 0;

    {
        std::lock_guard lk(mtx_);
        for (auto& [sym, s] : stats_) {
            const bool is_active = registry_ && registry_->is_whitelisted(sym);
            if (is_active) ++n_active;

            const bool liquid_ok = s.last_update_rate >= cfg_.min_update_rate_hz && s.last_bbo_notional >= cfg_.min_bbo_notional_usd;
            const bool spread_is_viable = s.last_spread_median >= 2.0;
            const bool spread_ok = spread_is_viable && s.last_spread_p75 <= cfg_.max_spread_p75_bps;
            const bool sample_ok = s.last_shadow_fills >= cfg_.min_shadow_fills;

            if (s.last_spread_median > 10.0 && s.last_bbo_notional < 3000.0) continue;

            const double entry_threshold = cfg_.min_net_edge_bps + 0.5;
            const bool edge_ok = s.ema_edge >= entry_threshold;
            const double exit_threshold = cfg_.min_net_edge_bps - 0.5;
            const bool bad = sample_ok && (s.last_net_edge < exit_threshold);

            if (edge_ok && liquid_ok && spread_ok && sample_ok) {
                s.consecutive_good_windows++;
                s.consecutive_bad_windows = 0;
            } else if (bad) {
                s.consecutive_bad_windows++;
                s.consecutive_good_windows = 0;
            }

            if (!is_active && s.consecutive_good_windows >= 3) {
                ++n_ready;
                to_promote.emplace_back(sym, s.ema_edge);
            }
            if (is_active && s.consecutive_bad_windows >= 8) {
                ++n_demote;
                to_demote.emplace_back(sym, s.last_net_edge);
            }
            if (!is_active && sample_ok) ++n_cand;
        }
    }

    std::cout << "[Screener] stats=" << stats_.size() << " active=" << n_active << " candidates=" << n_cand << " ready=" << n_ready << " demote=" << n_demote << "\n";

    if (to_promote.empty() && to_demote.empty()) return;

    try {
        pqxx::connection conn(conn_str_);
        pqxx::work txn(conn);
        for (const auto& [sym, score] : to_promote) {
            txn.exec("INSERT INTO mm_symbols (symbol, enabled, qty_usd, max_position_usd, max_hold_sec, min_hs_bps, note) "
                     "VALUES ($1, TRUE, 15.0, 30.0, 40, NULL, 'shadow-promoted: score=' || $2) "
                     "ON CONFLICT (symbol) DO UPDATE SET enabled = TRUE, qty_usd = COALESCE(mm_symbols.qty_usd, 15.0), "
                     "max_position_usd = COALESCE(mm_symbols.max_position_usd, 30.0), max_hold_sec = COALESCE(mm_symbols.max_hold_sec, 40), "
                     "min_hs_bps = NULL, note = 'shadow-promoted: score=' || $2, updated_at = NOW()", pqxx::params{sym, score});
        }
        for (const auto& [sym, score] : to_demote) {
            txn.exec("UPDATE mm_symbols SET enabled = FALSE, note = COALESCE(note,'') || ' | shadow-demoted: score=' || $2, updated_at = NOW() WHERE symbol = $1", pqxx::params{sym, score});
        }
        txn.commit();
        for (const auto& [sym, score] : to_promote) std::cout << "[SHADOW-PROMOTE] " << sym << " score=" << score << "\n";
        for (const auto& [sym, score] : to_demote) std::cout << "[SHADOW-DEMOTE] " << sym << " score=" << score << "\n";
    } catch (const std::exception& e) {
        std::cerr << "[Screener] shadow promote/demote failed: " << e.what() << "\n";
    }
}

void Screener::do_pg_revaluation() { /* Оставлено пустым, так как закомментировано в start() */ }
void Screener::schedule_pg_revaluation() {
    pg_reval_timer_.expires_after(std::chrono::seconds(300));
    pg_reval_timer_.async_wait([this](boost::system::error_code ec) {
        if (ec) return;
        do_pg_revaluation();
        schedule_pg_revaluation();
    });
}

void Screener::debug_add_symbol(const std::string& sym) {
    std::lock_guard lk(mtx_);
    auto& s = stats_[sym];
    s.symbol = sym;
}

SymbolScreenerStats Screener::debug_stats(const std::string& sym) const {
    std::lock_guard lk(mtx_);
    auto it = stats_.find(sym);
    return it == stats_.end() ? SymbolScreenerStats{} : it->second;
}

void Screener::start() {
    if (!cfg_.enabled) {
        std::cout << "[Screener] disabled in config, skipping timers\n";
        return;
    }
    schedule_snapshot();
    schedule_promote();
    std::cout << "[Screener] timers started (snapshot=" << cfg_.snapshot_interval_sec << "s, promote=" << cfg_.promote_interval_sec << "s)\n";
}