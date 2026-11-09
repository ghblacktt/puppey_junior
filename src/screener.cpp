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
                std::chrono::steady_clock::now().time_since_epoch()).count();
    }
}

Screener::Screener(boost::asio::io_context& ioc, const std::string& conn_str)
        : ioc_(ioc),
          conn_str_(conn_str),
          snapshot_timer_(ioc),
          promote_timer_(ioc) {}

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
                "       min_bbo_notional_usd, max_spread_p75_bps, auto_promote "
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
        }
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[Screener] config load failed: " << e.what()
                  << " — using defaults\n";
    }

    // 2) Список символов — top-N ликвидных
    auto syms = api.get_top_liquid_symbols(cfg_.scan_top_n);
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

// ── Обработка обновлений ──
void Screener::on_book_update(const std::string& symbol,
                              double best_bid, double best_ask,
                              double bid_size, double ask_size) {
    if (best_bid <= 0.0 || best_ask <= 0.0 || best_ask <= best_bid) return;

    std::lock_guard lk(mtx_);
    auto it = stats_.find(symbol);
    if (it == stats_.end()) return;
    auto& s = it->second;

    double mid   = (best_bid + best_ask) / 2.0;
    double spread_bps = (best_ask - best_bid) / mid * 10000.0;
    double bbo_notional = std::min(bid_size * best_bid, ask_size * best_ask);

    s.price = mid;
    s.spread_bps_buf.push_back(spread_bps);
    s.mid_buf.push_back(mid);
    s.bbo_notional_buf.push_back(bbo_notional);

    // Trim rolling window по размеру
    // 5 минут × ~5 updates/sec = 1500 сэмплов. Ограничим 2000.
    constexpr size_t MAX_BUF = 2000;
    if (s.spread_bps_buf.size() > MAX_BUF) s.spread_bps_buf.pop_front();
    if (s.mid_buf.size() > MAX_BUF) s.mid_buf.pop_front();
    if (s.bbo_notional_buf.size() > MAX_BUF) s.bbo_notional_buf.pop_front();

    // Rate tracking
    uint64_t t = now_ms();
    if (s.first_update_ms == 0) s.first_update_ms = t;
    s.last_update_ms = t;
    s.update_count++;

    // ── Shadow fill model ──
    double H = cfg_.target_half_bps / 10000.0;
    double shadow_bid = mid * (1.0 - H);
    double shadow_ask = mid * (1.0 + H);

    // Инициализация ожидающих теней
    if (!s.shadow_bid_pending && s.shadow_bid_fill_ms == 0) {
        s.shadow_bid_pending = true;
        s.shadow_bid_price   = shadow_bid;
    }
    if (!s.shadow_ask_pending && s.shadow_ask_fill_ms == 0) {
        s.shadow_ask_pending = true;
        s.shadow_ask_price   = shadow_ask;
    }

    // Проверка fill по bid: mid упал до shadow_bid
    if (s.shadow_bid_pending && mid <= s.shadow_bid_price) {
        s.shadow_bid_pending = false;
        s.shadow_bid_fill_ms = t;
        s.shadow_bid_fills++;
    }
    // Проверка fill по ask: mid поднялся до shadow_ask
    if (s.shadow_ask_pending && mid >= s.shadow_ask_price) {
        s.shadow_ask_pending = false;
        s.shadow_ask_fill_ms = t;
        s.shadow_ask_fills++;
    }

    // Оценка adverse через 1000мс после fill
    constexpr uint64_t ADVERSE_WINDOW_MS = 1000;
    if (s.shadow_bid_fill_ms > 0 && (t - s.shadow_bid_fill_ms) >= ADVERSE_WINDOW_MS) {
        double adv_bps = (s.shadow_bid_price - mid) / s.shadow_bid_price * 10000.0;
        s.shadow_bid_adverse_bps += adv_bps;
        s.shadow_bid_adverse_n++;
        s.shadow_bid_fill_ms = 0;  // сброс, ждём новый цикл
        s.shadow_bid_pending = false;
    }
    if (s.shadow_ask_fill_ms > 0 && (t - s.shadow_ask_fill_ms) >= ADVERSE_WINDOW_MS) {
        double adv_bps = (mid - s.shadow_ask_price) / s.shadow_ask_price * 10000.0;
        s.shadow_ask_adverse_bps += adv_bps;
        s.shadow_ask_adverse_n++;
        s.shadow_ask_fill_ms = 0;
        s.shadow_ask_pending = false;
    }
}

void Screener::on_mid_tick(const std::string& /*symbol*/, double /*mid*/) {
    // Не используется: mid уже обновляется в on_book_update
}

// ── Compute snapshot ──
void Screener::compute_snapshot_metrics(SymbolScreenerStats& s) {
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

    // Update rate
    uint64_t now = now_ms();
    double elapsed_sec = (now - s.first_update_ms) / 1000.0;
    s.last_update_rate = (elapsed_sec > 0.0) ? (s.update_count / elapsed_sec) : 0.0;

    // Shadow fills
    int total_fills = s.shadow_bid_fills + s.shadow_ask_fills;
    s.last_shadow_fills = total_fills;

    if (s.shadow_bid_adverse_n + s.shadow_ask_adverse_n > 0) {
        double total_adv = s.shadow_bid_adverse_bps + s.shadow_ask_adverse_bps;
        int total_n = s.shadow_bid_adverse_n + s.shadow_ask_adverse_n;
        s.last_shadow_adv = total_adv / total_n;
    } else {
        s.last_shadow_adv = 0.0;
    }

    // Net edge = H - adverse - 4 bps (maker roundtrip)
    s.last_net_edge = cfg_.target_half_bps - s.last_shadow_adv - 4.0;

    s.last_snapshot_ms = now;
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

    // Отбираем символы, проходящие фильтры
    std::vector<std::string> to_promote;
    {
        std::lock_guard lk(mtx_);
        for (auto& [sym, s] : stats_) {
            if (s.last_shadow_fills < cfg_.min_shadow_fills) continue;
            if (s.last_update_rate   < cfg_.min_update_rate_hz) continue;
            if (s.last_bbo_notional  < cfg_.min_bbo_notional_usd) continue;
            if (s.last_spread_p75    > cfg_.max_spread_p75_bps) continue;
            if (s.last_net_edge      < cfg_.min_net_edge_bps) continue;
            to_promote.push_back(sym);
        }
    }
    if (to_promote.empty()) return;

    // Пишем в mm_symbols (enabled=TRUE, min_hs_bps = target_half)
    try {
        pqxx::connection conn(conn_str_);
        pqxx::work txn(conn);
        for (const auto& sym : to_promote) {
            txn.exec(
                    "INSERT INTO mm_symbols (symbol, enabled, min_hs_bps, note) "
                    "VALUES ($1, TRUE, $2, 'auto-promoted by screener') "
                    "ON CONFLICT (symbol) DO UPDATE SET "
                    "  enabled = TRUE, "
                    "  min_hs_bps = COALESCE(mm_symbols.min_hs_bps, EXCLUDED.min_hs_bps), "
                    "  note = 'auto-promoted by screener', "
                    "  updated_at = NOW()",
                    pqxx::params{sym, cfg_.target_half_bps});
        }
        txn.exec(
                "UPDATE mm_screening_stats SET is_promoted = TRUE, "
                "reason = 'auto-promoted' "
                "WHERE symbol = ANY($1) AND id IN ("
                "  SELECT MAX(id) FROM mm_screening_stats WHERE symbol = ANY($1) GROUP BY symbol)",
                pqxx::params{to_promote});
        txn.commit();
        std::cout << "[Screener] promoted " << to_promote.size()
                  << " symbols to mm_symbols\n";
    } catch (const std::exception& e) {
        std::cerr << "[Screener] promote failed: " << e.what() << "\n";
    }
}

void Screener::start() {
    if (!cfg_.enabled) {
        std::cout << "[Screener] disabled in config, skipping timers\n";
        return;
    }
    running_ = true;
    schedule_snapshot();
    schedule_promote();
    std::cout << "[Screener] timers started (snapshot="
              << cfg_.snapshot_interval_sec << "s, promote="
              << cfg_.promote_interval_sec << "s)\n";
}