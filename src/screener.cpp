#include "screener.h"
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
    // 1) Конфиг
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
        std::cerr << "[Screener] config load failed: " << e.what()
                  << " — using defaults\n";
    }

    // 2) Список символов — top-N ликвидных
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

// ── Вычисление метрик ──
double Screener::median_of(std::deque<double> buf) {
    if (buf.empty()) return 0.0;
    std::sort(buf.begin(), buf.end());
    size_t n = buf.size();
    return (n % 2 == 0) ? (buf[n/2 - 1] + buf[n/2]) / 2.0 : buf[n/2];
}

double Screener::pctl_of(std::deque<double> buf, double p) {
    if (buf.empty()) return 0.0;
    std::sort(buf.begin(), buf.end());
    double idx = (buf.size() - 1) * p;
    size_t lo = (size_t)idx;
    size_t hi = std::min(lo + 1, buf.size() - 1);
    double frac = idx - lo;
    return buf[lo] * (1.0 - frac) + buf[hi] * frac;
}

double Screener::sigma_of(const std::deque<double>& mids) {
    if (mids.size() < 10) return 0.0;
    std::vector<double> rets;
    rets.reserve(mids.size() - 1);
    for (size_t i = 1; i < mids.size(); ++i) {
        if (mids[i-1] > 0.0)
            rets.push_back(std::log(mids[i] / mids[i-1]) * 10000.0);
    }
    if (rets.empty()) return 0.0;
    double mean = 0.0;
    for (double r : rets) mean += r;
    mean /= rets.size();
    double var = 0.0;
    for (double r : rets) var += (r - mean) * (r - mean);
    var /= std::max(1u, (unsigned)rets.size() - 1);
    return std::sqrt(var);
}

void Screener::on_book_update(const std::string& symbol,
                              double best_bid, double best_ask,
                              double bid_size, double ask_size) {

    static std::atomic<uint64_t> _entry_count{0};
    uint64_t n = ++_entry_count;
    if (n <= 10 || n % 1000 == 0) {
        std::cout << "[SCREENER-ENTRY] n=" << n
                  << " sym=" << symbol
                  << " bb=" << best_bid << " ba=" << best_ask << "\n";
        std::cout.flush();
    }

    // Убрал дублирующуюся проверку
    if (best_bid <= 0.0 || best_ask <= 0.0 || best_ask <= best_bid) return;

    std::lock_guard lk(mtx_);
    auto it = stats_.find(symbol);
    if (it == stats_.end()) return;
    auto& s = it->second;

    const double mid         = (best_bid + best_ask) / 2.0;
    const double spread_bps  = (best_ask - best_bid) / mid * 10000.0;
    const double bbo_notional = std::min(bid_size * best_bid, ask_size * best_ask);

    s.price = mid;
    s.spread_bps_buf.push_back(spread_bps);
    s.mid_buf.push_back(mid);
    s.bbo_notional_buf.push_back(bbo_notional);

    // Rolling window
    constexpr size_t MAX_BUF = 2000;
    if (s.spread_bps_buf.size() > MAX_BUF)    s.spread_bps_buf.pop_front();
    if (s.mid_buf.size() > MAX_BUF)           s.mid_buf.pop_front();
    if (s.bbo_notional_buf.size() > MAX_BUF)  s.bbo_notional_buf.pop_front();

    // Rate tracking
    const uint64_t t = now_ms();
    if (s.first_update_ms == 0) s.first_update_ms = t;
    s.last_update_ms = t;
    s.update_count++;
    s.update_events_ts_.push_back(t);

    // ═══ DBG: раз в 500 событий ═══
    static std::atomic<uint64_t> _dbg_count{0};
    if (++_dbg_count % 500 == 0) {
        std::cerr << "[SCREENER-DBG] " << symbol
                  << " upd=" << s.update_count
                  << " fills=" << (s.shadow_bid_fills + s.shadow_ask_fills)
                  << " pend_b=" << (s.shadow_bid_pending ? 1 : 0)
                  << " pend_a=" << (s.shadow_ask_pending ? 1 : 0)
                  << " fill_ms_b=" << s.shadow_bid_fill_ms
                  << " mid=" << std::fixed << std::setprecision(6) << mid
                  << " sb=" << s.shadow_bid_price
                  << " sa=" << s.shadow_ask_price
                  << std::defaultfloat << "\n";
    }

    // ═══════════════════════════════════════════════════════════
    // FIX: Реалистичный dynamic min_hs для shadow
    // Мы тестируем пару на том спреде, на котором РЕАЛЬНО будет
    // торговать бот (target_half_bps), но даем небольшую фору
    // для широких спредов, чтобы не стоять слишком далеко.
    // ═══════════════════════════════════════════════════════════
    double shadow_min_hs_bps;
    if (registry_ && cfg_.use_per_symbol_params) {
        shadow_min_hs_bps = registry_->effective_min_hs_bps(symbol, mid);
    } else {
        shadow_min_hs_bps = std::clamp(s.last_spread_median * 0.5, 2.0, 12.0);
    }

    const double H = shadow_min_hs_bps / 10000.0;
    const double shadow_bid = mid * (1.0 - H);
    const double shadow_ask = mid * (1.0 + H);

    // ═══ Timeout: pending без fill'а висит > 30 сек — отменяем ═══
    constexpr uint64_t SHADOW_TIMEOUT_MS = 30'000;
    if (s.shadow_bid_pending && s.shadow_bid_placed_ms > 0
        && (t - s.shadow_bid_placed_ms) > SHADOW_TIMEOUT_MS) {
        shadow_fm_.cancel(s.shadow_bid_oid_);
        s.shadow_bid_oid_.clear();
        s.shadow_bid_pending   = false;
        s.shadow_bid_placed_ms = 0;
    }
    if (s.shadow_ask_pending && s.shadow_ask_placed_ms > 0
        && (t - s.shadow_ask_placed_ms) > SHADOW_TIMEOUT_MS) {
        shadow_fm_.cancel(s.shadow_ask_oid_);
        s.shadow_ask_oid_.clear();
        s.shadow_ask_pending   = false;
        s.shadow_ask_placed_ms = 0;
    }

    // ═══ Регистрация: нет pending и нет активного adverse окна ═══
    static std::atomic<uint64_t> g_shadow_seq{0};
    auto register_shadow = [&](const std::string& tag, const std::string& side,
                               double px, bool& pending, uint64_t& placed_ms,
                               std::string& oid_out, double& price_out) {
        oid_out = symbol + "_" + tag + "_"
                  + std::to_string(g_shadow_seq.fetch_add(1, std::memory_order_relaxed));
        mm::FillModel::Order o;
        o.order_id = oid_out;
        o.symbol   = symbol;
        o.side     = side;
        o.price    = px;
        o.qty      = 1.0;
        o.placed_ms = t;
        shadow_fm_.register_order(std::move(o), best_bid, best_ask, bid_size, ask_size);
        pending    = true;
        placed_ms  = t;
        price_out  = px;
    };

    if (!s.shadow_bid_pending && s.shadow_bid_fill_ms == 0) {
        register_shadow("sB", "Buy", shadow_bid, s.shadow_bid_pending,
                        s.shadow_bid_placed_ms, s.shadow_bid_oid_, s.shadow_bid_price);
    }
    if (!s.shadow_ask_pending && s.shadow_ask_fill_ms == 0) {
        register_shadow("sA", "Sell", shadow_ask, s.shadow_ask_pending,
                        s.shadow_ask_placed_ms, s.shadow_ask_oid_, s.shadow_ask_price);
    }

    // Sprint 7: align with live 30s markout
    const uint64_t ADVERSE_WINDOW_MS = 5'000;

    if (s.shadow_bid_fill_ms > 0 && (t - s.shadow_bid_fill_ms) >= ADVERSE_WINDOW_MS) {
        const double adv_bps = (s.shadow_bid_price - mid) / s.shadow_bid_price * 10000.0;
        s.shadow_bid_adverse_bps += adv_bps;
        s.shadow_bid_adverse_n++;
        s.shadow_bid_fill_ms = 0;
        s.adverse_events_.emplace_back(t, adv_bps);
    }
    if (s.shadow_ask_fill_ms > 0 && (t - s.shadow_ask_fill_ms) >= ADVERSE_WINDOW_MS) {
        const double adv_bps = (mid - s.shadow_ask_price) / s.shadow_ask_price * 10000.0;
        s.shadow_ask_adverse_bps += adv_bps;
        s.shadow_ask_adverse_n++;
        s.shadow_ask_fill_ms = 0;
        s.adverse_events_.emplace_back(t, adv_bps);
    }

    // ═══ Through detection ═══
    if (s.last_mid_for_through > 0) {
        const double move_bps = std::abs(mid - s.last_mid_for_through)
                                / s.last_mid_for_through * 10000.0;
        if (move_bps > 3.0) {
            s.through_count++;
            s.through_events_ts_.push_back(t);
        }
    }
    s.last_mid_for_through = mid;
}

// ============================================================
// on_trade — трейды из ScannerClient. Идём через shadow_fm_.
// Первый fill на сессию — фиксируем fill_ms/fill_price,
// отменяем ордер (1 fill per adverse session).
// ============================================================
void Screener::on_trade(const std::string& symbol, double px, double qty,
                        bool is_buyer_maker, uint64_t ts_ms) {
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

        if (is_bid) {
            if (s.shadow_bid_fill_ms == 0 && !s.shadow_bid_oid_.empty()) {
                s.shadow_bid_fill_ms = f.exec_ms;
                s.shadow_bid_price   = f.exec_price;
                s.shadow_bid_fills++;
                s.shadow_bid_pending = false;
                s.shadow_bid_oid_.clear();
                s.fills_events_ts_.push_back(f.exec_ms);
                shadow_fm_.cancel(f.order_id);
            }
        } else {
            if (s.shadow_ask_fill_ms == 0 && !s.shadow_ask_oid_.empty()) {
                s.shadow_ask_fill_ms = f.exec_ms;
                s.shadow_ask_price   = f.exec_price;
                s.shadow_ask_fills++;
                s.shadow_ask_pending = false;
                s.shadow_ask_oid_.clear();
                s.fills_events_ts_.push_back(f.exec_ms);
                shadow_fm_.cancel(f.order_id);
            }
        }
    }
}

// ── Compute snapshot ──
// ── Compute snapshot ──
void Screener::compute_snapshot_metrics(SymbolScreenerStats& s) const {
    // ─── Спред/σ/нотионал — как было (по rolling buf) ───
    s.last_spread_median = median_of(s.spread_bps_buf);
    s.last_spread_p25    = pctl_of(s.spread_bps_buf, 0.25);
    s.last_spread_p75    = pctl_of(s.spread_bps_buf, 0.75);
    s.last_sigma_bps     = sigma_of(s.mid_buf);

    if (!s.bbo_notional_buf.empty()) {
        double sum = 0.0;
        for (double v : s.bbo_notional_buf) sum += v;
        s.last_bbo_notional = sum / s.bbo_notional_buf.size();
    } else {
        s.last_bbo_notional = 0.0;
    }

    const uint64_t WINDOW_MS = static_cast<uint64_t>(cfg_.window_sec) * 1000ULL;
    const uint64_t now = now_ms();
    const uint64_t cutoff = (now > WINDOW_MS) ? (now - WINDOW_MS) : 0;

    auto prune_ts = [cutoff](auto& dq) {
        while (!dq.empty() && dq.front() < cutoff) dq.pop_front();
    };
    prune_ts(s.fills_events_ts_);
    prune_ts(s.through_events_ts_);
    prune_ts(s.update_events_ts_);
    while (!s.adverse_events_.empty() && s.adverse_events_.front().first < cutoff) {
        s.adverse_events_.pop_front();
    }

    const int window_fills = static_cast<int>(s.fills_events_ts_.size());
    const int window_updates = static_cast<int>(s.update_events_ts_.size());
    const int window_adv_n = static_cast<int>(s.adverse_events_.size());

    double window_adv_sum = 0.0;
    for (const auto& [ts, bps] : s.adverse_events_) window_adv_sum += bps;
    const double window_adv_avg = (window_adv_n > 0) ? window_adv_sum / window_adv_n : 0.0;

    s.last_shadow_fills = window_fills;
    s.last_shadow_adv = window_adv_avg;
    s.last_update_rate = window_updates / (WINDOW_MS / 1000.0);

    // ═══════════════════════════════════════════════════════════
    // 1. Расчет "сырого" edge (RAW EDGE)
    // ═══════════════════════════════════════════════════════════
    double gross_edge = -window_adv_avg - 4.0; // -2.0 bps на комиссии

    double round_turn_ratio = (window_fills >= 4) ? 1.5 : 0.0;

    const double through_ratio = (window_updates > 0) ? std::min(1.0, (double)s.through_count / window_updates) : 0.0;
    double toxic_penalty = through_ratio * 5.0;

    double volume_bonus = 0.0;
    if (s.last_bbo_notional > 0) {
        volume_bonus = std::min(2.0, std::log2(s.last_bbo_notional / 500.0) * 0.4);
    }

    double raw_edge = gross_edge + round_turn_ratio - toxic_penalty + volume_bonus;

    // ═══════════════════════════════════════════════════════════
    // 2. SHRINKAGE: Штраф за малую выборку (КЛЮЧЕВОЙ ФИКС)
    // ═══════════════════════════════════════════════════════════
    // Формула доверия: N / (N + K).
    // Если K=20, то при N=5 филлах уверенность = 5 / 25 = 20%
    // При N=20 филлах уверенность = 20 / 40 = 50%
    // При N=100 филлах уверенность = 100 / 120 = 83%
    constexpr double kShrinkK = 20.0;
    const double n = static_cast<double>(window_fills);
    const double confidence = (n > 0.0) ? (n / (n + kShrinkK)) : 0.0;

    // Штраф за тонкий стакан: если BBO < $500, мы рискуем проскальзыванием.
    // Максимальный штраф = 10 bps, если BBO = $0.
    double thin_penalty = 0.0;
    if (s.last_bbo_notional < 500.0) {
        thin_penalty = ((500.0 - s.last_bbo_notional) / 500.0) * 10.0;
    }

    // Финальный скор с учетом доверия и штрафа за ликвидность
    s.last_net_edge = (raw_edge * confidence) - thin_penalty;
    s.last_snapshot_ms = now;

    // ═══ EXPONENTIAL DECAY: последние 15 минут имеют больший вес ═══
    // alpha = 0.3 означает, что последний снапшот = 30% веса, прошлые = 70%
    // Это даёт быструю реакцию на изменения рынка
    // ═══ EXPONENTIAL DECAY: последние 15 минут имеют больший вес ═══
    const double alpha = 0.3;
    if (s.ema_last_update_ms == 0) {
        s.ema_edge = s.last_net_edge;
    } else {
        s.ema_edge = alpha * s.last_net_edge + (1.0 - alpha) * s.ema_edge;
    }
    s.ema_last_update_ms = now;

    // ═══════════════════════════════════════════════════════════
    // 3. ГИСТЕРЕЗИС: обновляем счетчики
    // ═══════════════════════════════════════════════════════════
    bool is_good = (s.ema_edge >= cfg_.min_net_edge_bps) &&
                   (s.last_shadow_fills >= cfg_.min_shadow_fills) &&
                   (s.last_spread_p75 <= cfg_.max_spread_p75_bps);

    // is_bad: edge упал значительно ниже порога (запас 0.5 bps)
    bool is_bad = (s.ema_edge < (cfg_.min_net_edge_bps - 0.5));

    if (is_good) {
        s.consecutive_good_windows++;
        s.consecutive_bad_windows = 0;
    } else if (is_bad) {
        s.consecutive_bad_windows++;
        s.consecutive_good_windows = 0;
    } else {
        // Нейтральная зона: НЕ сбрасываем счетчики.
        // Символ сохраняет накопленный прогресс, пока не станет явно плохим.
    }
}

// ── Snapshot ──
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
        std::string sql =
                "INSERT INTO mm_screening_stats "
                "(symbol, run_tag, spread_median_bps, spread_p25_bps, spread_p75_bps, "
                " sigma_bps, bbo_notional_usd, update_rate_hz, "
                " target_half_bps, shadow_fills, shadow_adverse_bps, shadow_net_edge_bps) "
                "VALUES ";
        std::ostringstream rows;
        for (size_t i = 0; i < batch.size(); ++i) {
            const auto& s = batch[i];
            if (i > 0) rows << ",";
            rows << "("
                 << txn.quote(s.symbol) << ","
                 << txn.quote(run_tag_) << ","
                 << s.last_spread_median << ","
                 << s.last_spread_p25 << ","
                 << s.last_spread_p75 << ","
                 << s.last_sigma_bps << ","
                 << s.last_bbo_notional << ","
                 << s.last_update_rate << ","
                 << cfg_.target_half_bps << ","
                 << s.last_shadow_fills << ","
                 << s.last_shadow_adv << ","
                 << s.last_net_edge
                 << ")";
        }
        txn.exec(sql + rows.str());
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[Screener] persist snapshot failed: " << e.what() << "\n";
    }
}

// ── Promote ──
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

    int n_active = 0;
    int n_cand = 0;
    int n_ready = 0;
    int n_demote = 0;

    {
        std::lock_guard lk(mtx_);

        for (auto& [sym, s] : stats_) {
            const bool is_active = registry_ && registry_->is_whitelisted(sym);
            if (is_active) ++n_active;

            const bool liquid_ok = s.last_update_rate >= cfg_.min_update_rate_hz &&
                                   s.last_bbo_notional >= cfg_.min_bbo_notional_usd;
            const bool spread_is_viable = s.last_spread_median >= 2.0;
            const bool spread_ok = spread_is_viable &&
                                   s.last_spread_p75 <= cfg_.max_spread_p75_bps;
            const bool sample_ok = s.last_shadow_fills >= cfg_.min_shadow_fills;

            // Пропускаем тонкие символы с широким спредом (защита от фейкового edge)
            if (s.last_spread_median > 10.0 && s.last_bbo_notional < 3000.0) {
                continue;
            }

            // ═══ ИДЕАЛЬНЫЙ ГИСТЕРЕЗИС ═══
            const double entry_threshold = cfg_.min_net_edge_bps + 0.5; // Вход: требуется запас +0.5
            const bool edge_ok = s.ema_edge >= entry_threshold;

            const double exit_threshold = cfg_.min_net_edge_bps - 0.5;  // Выход: допускаем просадку до -0.5
            const bool bad = sample_ok && (s.last_net_edge < exit_threshold);

            if (edge_ok && liquid_ok && spread_ok && sample_ok) {
                s.consecutive_good_windows++;
                s.consecutive_bad_windows = 0;
            } else if (bad) {
                s.consecutive_bad_windows++;
                s.consecutive_good_windows = 0;
            } else {
                // Нейтральная зона: сохраняем прогресс
            }

            // Вход: 3 окна по 60 сек = 3 минуты стабильности
            if (!is_active && s.consecutive_good_windows >= 3) {
                ++n_ready;
                to_promote.emplace_back(sym, s.ema_edge);
            }

            // Выход: 8 окон по 60 сек = 8 минут стабильности (защита от шумных демоутов)
            if (is_active && s.consecutive_bad_windows >= 8) {
                ++n_demote;
                to_demote.emplace_back(sym, s.last_net_edge);
            }

            if (!is_active && sample_ok) {
                ++n_cand;
            }
        }
    }

    std::cout << "[Screener] stats=" << stats_.size()
              << " active=" << n_active
              << " candidates=" << n_cand
              << " ready=" << n_ready
              << " demote=" << n_demote << "\n";


    double best_edge = -1e9;
    std::string best_sym;
    int n_pass_edge = 0, n_pass_fills = 0;
    for (auto& [sym, s] : stats_) {
        if (s.last_net_edge > best_edge) {
            best_edge = s.last_net_edge;
            best_sym = sym;
        }
        if (s.last_net_edge >= cfg_.min_net_edge_bps) n_pass_edge++;
        if (s.last_shadow_fills >= cfg_.min_shadow_fills) n_pass_fills++;
    }

    std::cout << "[Screener] best=" << best_sym
              << " edge=" << std::fixed << std::setprecision(2) << best_edge
              << " (need ≥" << cfg_.min_net_edge_bps << ") | "
              << "pass_edge=" << n_pass_edge
              << "/" << stats_.size()
              << " pass_fills=" << n_pass_fills
              << std::defaultfloat << "\n";

    if (to_promote.empty() && to_demote.empty()) return;

    try {
        pqxx::connection conn(conn_str_);
        pqxx::work txn(conn);

        for (const auto& [sym, score] : to_promote) {
            txn.exec(
                    "INSERT INTO mm_symbols "
                    "(symbol, enabled, qty_usd, max_position_usd, max_hold_sec, min_hs_bps, note) "
                    "VALUES ($1, TRUE, 15.0, 30.0, 40, NULL, 'shadow-promoted: score=' || $2) "
                    "ON CONFLICT (symbol) DO UPDATE SET "
                    "  enabled = TRUE, "
                    "  qty_usd = COALESCE(mm_symbols.qty_usd, 15.0), "
                    "  max_position_usd = COALESCE(mm_symbols.max_position_usd, 30.0), "
                    "  max_hold_sec = COALESCE(mm_symbols.max_hold_sec, 40), "
                    "  min_hs_bps = NULL, "
                    "  note = 'shadow-promoted: score=' || $2, "
                    "  updated_at = NOW()",
                    pqxx::params{sym, score});
        }

        for (const auto& [sym, score] : to_demote) {
            txn.exec(
                    "UPDATE mm_symbols SET "
                    "  enabled = FALSE, "
                    "  note = COALESCE(note,'') || ' | shadow-demoted: score=' || $2, "
                    "  updated_at = NOW() "
                    "WHERE symbol = $1",
                    pqxx::params{sym, score});
        }

        txn.commit();

        for (const auto& [sym, score] : to_promote) {
            std::cout << "[SHADOW-PROMOTE] " << sym
                      << " score=" << score << "\n";
        }

        for (const auto& [sym, score] : to_demote) {
            std::cout << "[SHADOW-DEMOTE] " << sym
                      << " score=" << score << "\n";
        }

    } catch (const std::exception& e) {
        std::cerr << "[Screener] shadow promote/demote failed: "
                  << e.what() << "\n";
    }
}

void Screener::do_pg_revaluation() {
    if (!cfg_.auto_promote) return;

    try {
        pqxx::connection conn(conn_str_);
        pqxx::work txn(conn);

        // ═══ 1. GRADUATE (Увеличиваем риск для стабильно прибыльных) ═══
        // Критерии: Win Rate > 60%, средний 5s markout > 1.5 bps, минимум 50 филлов
        auto gres = txn.exec(
                "SELECT symbol, "
                "       COUNT(*) as total_fills, "
                "       AVG(mk_5s_bps) as avg_mk_5s, "
                "       (COUNT(CASE WHEN mk_5s_bps > 0 THEN 1 END) * 100.0 / COUNT(*)) as win_rate "
                "FROM mm_markouts "
                "WHERE finalized = TRUE AND is_close = FALSE AND mk_5s_bps IS NOT NULL "
                "  AND created_at >= NOW() - INTERVAL '24 hours' "
                "  AND symbol IN (SELECT symbol FROM mm_symbols WHERE enabled = TRUE) "
                "GROUP BY symbol "
                "HAVING COUNT(*) >= 50 "
                "   AND (COUNT(CASE WHEN mk_5s_bps > 0 THEN 1 END) * 100.0 / COUNT(*)) > 60.0 "
                "   AND AVG(mk_5s_bps) > 1.5");

        for (auto row : gres) {
            std::string sym = row["symbol"].as<std::string>();
            double wr = row["win_rate"].as<double>();
            double mk = row["avg_mk_5s"].as<double>();

            // Плавно увеличиваем qty_usd и max_hold_sec, но с жестким потолком
            txn.exec(
                    "UPDATE mm_symbols SET "
                    "  qty_usd = LEAST(qty_usd * 1.2, 100.0), "
                    "  max_position_usd = LEAST(max_position_usd * 1.2, 200.0), "
                    "  max_hold_sec = LEAST(max_hold_sec + 10, 120), " // Даем больше времени хорошим символам
                    "  note = 'graduated: wr=' || ROUND($2,1) || '%, mk5s=' || ROUND($3,1), updated_at = NOW() "
                    "WHERE symbol = $1",
                    pqxx::params{sym, wr, mk});

            std::cout << "[PG-GRADUATE] " << sym << " WR=" << wr << "% mk5s=" << mk << "\n";
        }

        // ═══ 2. DOWNGRADE (Снижаем риск для деградирующих, но не убиваем сразу) ═══
        // Критерии: Win Rate < 45% ИЛИ средний 5s markout < -3.0 bps
        auto dres = txn.exec(
                "SELECT symbol, "
                "       COUNT(*) as total_fills, "
                "       AVG(mk_5s_bps) as avg_mk_5s, "
                "       (COUNT(CASE WHEN mk_5s_bps > 0 THEN 1 END) * 100.0 / COUNT(*)) as win_rate "
                "FROM mm_markouts "
                "WHERE finalized = TRUE AND is_close = FALSE AND mk_5s_bps IS NOT NULL "
                "  AND created_at >= NOW() - INTERVAL '12 hours' "
                "  AND symbol IN (SELECT symbol FROM mm_symbols WHERE enabled = TRUE) "
                "GROUP BY symbol "
                "HAVING COUNT(*) >= 30 "
                "   AND ( (COUNT(CASE WHEN mk_5s_bps > 0 THEN 1 END) * 100.0 / COUNT(*)) < 45.0 "
                "      OR AVG(mk_5s_bps) < -3.0 )");

        for (auto row : dres) {
            std::string sym = row["symbol"].as<std::string>();
            double wr = row["win_rate"].as<double>();
            double mk = row["avg_mk_5s"].as<double>();

            // Уменьшаем размер позиции. Если она уже минимальная (10.0) — отключаем символ полностью.
            txn.exec(
                    "UPDATE mm_symbols SET "
                    "  qty_usd = GREATEST(qty_usd * 0.7, 10.0), "
                    "  max_position_usd = GREATEST(max_position_usd * 0.7, 20.0), "
                    "  max_hold_sec = GREATEST(max_hold_sec - 10, 30), " // Уменьшаем время жизни позиции
                    "  enabled = CASE WHEN qty_usd <= 10.0 THEN FALSE ELSE TRUE END, "
                    "  note = 'downgraded: wr=' || ROUND($2,1) || '%, mk5s=' || ROUND($3,1), updated_at = NOW() "
                    "WHERE symbol = $1",
                    pqxx::params{sym, wr, mk});

            std::cout << "[PG-DOWNGRADE] " << sym << " WR=" << wr << "% mk5s=" << mk << "\n";
        }

        // ═══ 3. KILL (Мертвые символы) ═══
        txn.exec(
                "UPDATE mm_symbols SET enabled = FALSE, note = note || ' | dead: no fills 48h' "
                "WHERE enabled = TRUE AND symbol NOT IN ("
                "  SELECT symbol FROM mm_markouts WHERE created_at >= NOW() - INTERVAL '48 hours'"
                ")");

        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[Screener-PG] revaluation failed: " << e.what() << "\n";
    }
}

void Screener::schedule_pg_revaluation() {
    pg_reval_timer_.expires_after(std::chrono::seconds(300));
    pg_reval_timer_.async_wait([this](boost::system::error_code ec) {
        if (ec) return;
        do_pg_revaluation();
        schedule_pg_revaluation();
    });
}

// ============================================================
// Debug accessors (tests only)
// ============================================================
void Screener::debug_add_symbol(const std::string& sym) {
    std::lock_guard lk(mtx_);
    auto& s = stats_[sym];
    s.symbol = sym;
    s.effective_min_hs_bps   = 10.0;
    s.effective_max_hold_sec = 120;
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
    running_ = true;
    schedule_snapshot();
    schedule_promote();
//    schedule_pg_revaluation();   // ← NEW: Phase 3
    std::cout << "[Screener] timers started (snapshot="
              << cfg_.snapshot_interval_sec << "s, promote="
              << cfg_.promote_interval_sec << "s, pg_reval=300s)\n";
}