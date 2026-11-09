#include "funding_arb_strategy.h"
#include "monitor.h"
#include <iostream>
#include <cmath>
#include <iomanip>
#include <algorithm>
#include <sstream>
#include <sys/stat.h>

uint64_t FundingArbStrategy::now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
}

FundingArbStrategy::FundingArbStrategy(BybitApiClient& api, Config cfg)
        : api_(api), cfg_(cfg) {}

void FundingArbStrategy::tick() {
    uint64_t now = now_ms();

    // ---- 1. Проверить существующие позиции ----
    check_positions();

    // ---- 2. Сканировать новые возможности (раз в rescan_interval_ms) ----
    if (now - last_scan_ms_ < cfg_.rescan_interval_ms) return;
    last_scan_ms_ = now;

    refresh_spot_symbols();      // обновить кэш спот-символов
    discover_opportunities();

    // ---- 3. Открыть новые позиции ----
    for (const auto& opp : last_opps_) {
        if (positions_.size() >= cfg_.max_positions) break;
        if (has_position(opp.symbol)) continue;
        if (get_total_notional() + cfg_.notional_usd_per_pos > cfg_.max_total_notional) break;

        std::cout << "[FUNDING] OPPORTUNITY " << opp.symbol
                  << " funding=" << std::fixed << std::setprecision(2) << opp.funding_bps
                  << " bps basis=" << opp.basis_bps
                  << " bps turnover=$" << std::setprecision(1) << (opp.turnover_24h / 1e6) << "M\n";

        open_position(opp);
    }
}

void FundingArbStrategy::refresh_spot_symbols() {
    auto all = api_.get_all_spot_symbols();
    spot_symbols_.clear();
    for (const auto& s : all) spot_symbols_.insert(s);
    std::cout << "[FUNDING] Spot symbols cached: " << spot_symbols_.size() << "\n";
}

void FundingArbStrategy::discover_opportunities() {
    last_opps_.clear();

    // ---- Один запрос: все linear tickers (funding + turnover + lastPrice) ----
    auto tickers = api_.get_all_tickers_linear();
    if (tickers.empty()) {
        std::cerr << "[FUNDING] get_all_tickers_linear returned empty\n";
        return;
    }

    std::cout << "[FUNDING] Scanned " << tickers.size() << " linear tickers\n";

    // ---- Пересечение: есть перп + есть спот + фильтры ----
    for (const auto& [symbol, t] : tickers) {
        // Нужен спот для хеджа
        if (!spot_symbols_.count(symbol)) continue;

        double funding_bps = t.funding_rate * 10000.0;
        double turnover    = t.turnover_24h;

        if (funding_bps < cfg_.min_funding_bps) continue;
        if (turnover < cfg_.min_turnover_24h_usd) continue;

        // Получить спот-цену для basis
        auto spot = api_.get_orderbook_snapshot_spot(symbol, 1);
        if (spot.asks.empty() || spot.bids.empty()) continue;

        double spot_ask = spot.asks[0].first;
        double spot_mid = (spot.bids[0].first + spot.asks[0].first) / 2.0;
        double perp     = t.last_price;
        if (perp <= 0 || spot_mid <= 0) continue;

        // Цена в разумном диапазоне (иначе notional_per_pos даст фракцию монеты)
        if (spot_ask > cfg_.max_spot_ask_usd) continue;
        if (spot_ask < cfg_.min_spot_ask_usd) continue;

        double basis = (perp - spot_mid) / spot_mid * 10000.0;
        if (std::abs(basis) > cfg_.max_basis_bps) continue;

        Opportunity opp;
        opp.symbol       = symbol;
        opp.funding_bps  = funding_bps;
        opp.basis_bps    = basis;
        opp.turnover_24h = turnover;
        opp.spot_ask     = spot_ask;
        opp.perp_price   = perp;
        last_opps_.push_back(opp);
    }

    // Сортировка по funding (сначала самые прибыльные)
    std::sort(last_opps_.begin(), last_opps_.end(),
              [](const auto& a, const auto& b) { return a.funding_bps > b.funding_bps; });

    std::cout << "[FUNDING] Found " << last_opps_.size() << " opportunities\n";
    for (size_t i = 0; i < std::min<size_t>(last_opps_.size(), 10); ++i) {
        const auto& o = last_opps_[i];
        auto fmt2 = [](double v) {
            char b[64];
            snprintf(b, sizeof(b), "%.2f", v);
            return std::string(b);
        };
        std::cout << "  " << o.symbol
                  << " funding=" << fmt2(o.funding_bps) << " bps"
                  << " basis=" << fmt2(o.basis_bps) << " bps"
                  << " vol=$" << fmt2(o.turnover_24h / 1e6) << "M\n";
    }
}

void FundingArbStrategy::check_positions() {
    if (positions_.empty()) return;

    // Один запрос — все tickers (funding для всех сразу)
    auto tickers = api_.get_all_tickers_linear();

    for (size_t i = 0; i < positions_.size(); ) {
        auto& p = positions_[i];
        double age_h = (now_ms() - p.opened_ms) / 3600000.0;

        auto it = tickers.find(p.symbol);
        double current_funding_bps = (it != tickers.end())
                                     ? it->second.funding_rate * 10000.0
                                     : 0.0;

        double current_basis = get_basis_bps(p.symbol);

        std::cout << "[FUNDING-POS] " << p.symbol
                  << " held=" << std::fixed << std::setprecision(1) << age_h << "h"
                  << " funding_now=" << std::setprecision(2) << current_funding_bps << " bps"
                  << " entry_basis=" << p.entry_basis_bps << " bps"
                  << " now_basis=" << current_basis << " bps\n";

        // Принудительное закрытие по времени
        if (age_h > cfg_.max_hold_hours) {
            std::cerr << "[FUNDING] " << p.symbol << " max_hold exceeded, closing\n";
            close_position(i, "MAX_HOLD");
            continue;
        }

        // Funding флипнулся в отрицательный
        if (current_funding_bps < cfg_.exit_funding_bps) {
            Monitor::append_event(events_file(), "FUNDING_FLIP",
                                  p.symbol + " from=" + std::to_string(p.entry_funding_bps) +
                                  " to=" + std::to_string(current_funding_bps));
            close_position(i, "FUNDING_FLIP");
            continue;
        }

        ++i;
    }
}

void FundingArbStrategy::open_position(const Opportunity& opp) {
    double qty = cfg_.notional_usd_per_pos / opp.spot_ask;
    qty = api_.normalize_qty_spot(opp.symbol, qty);
    if (qty <= 0) {
        std::cerr << "[FUNDING] " << opp.symbol << " qty=0 after normalize\n";
        return;
    }

    // Проверить, что notional реалистичный
    double actual_notional = qty * opp.spot_ask;
    if (actual_notional < 5.0) {
        std::cerr << "[FUNDING] " << opp.symbol << " notional too small: $" << actual_notional << "\n";
        return;
    }

    if (cfg_.dry_run) {
        std::cout << "[FUNDING] DRY-RUN OPEN " << opp.symbol
                  << " qty=" << std::setprecision(4) << qty
                  << " spot_ask=" << std::setprecision(8) << opp.spot_ask
                  << " perp=" << std::setprecision(8) << opp.perp_price
                  << " notional=$" << std::fixed << std::setprecision(2) << actual_notional
                  << " funding=" << opp.funding_bps << " bps\n"
                  << std::defaultfloat;   // сброс

        Position p;
        p.symbol          = opp.symbol;
        p.spot_qty        = qty;
        p.perp_qty        = qty;
        p.entry_spot_price = opp.spot_ask;
        p.entry_perp_price = opp.perp_price;
        p.entry_basis_bps = opp.basis_bps;
        p.entry_funding_bps = opp.funding_bps;
        p.opened_ms       = now_ms();
        positions_.push_back(p);
        save_positions();
        Monitor::append_event(events_file(), "OPEN",
                              p.symbol + " qty=" + std::to_string(qty) +
                              " notional=$" + std::to_string(actual_notional) +
                              " basis=" + std::to_string(opp.basis_bps) +
                              " funding=" + std::to_string(opp.funding_bps));
        positions_.push_back(p);
        Monitor::append_event(events_file(), "DRY_RUN_OPEN",
                              p.symbol + " qty=" + std::to_string(qty));
        return;
    }

    // ---- Реальное открытие ----
    std::cout << "[FUNDING] OPENING " << opp.symbol
              << " qty=" << qty << " notional=$" << actual_notional << "\n";

    // 1. Купить спот
    std::string spot_oid = api_.place_ioc_limit_order_spot(
            opp.symbol, "Buy",
            opp.spot_ask * 1.002,  // +0.2% выше ask — гарантирует fill по ask/better
            qty                     // limit orders всегда используют qty в base
    );
    if (spot_oid.empty()) {
        std::cerr << "[FUNDING] " << opp.symbol
                  << " spot buy FAILED: " << api_.last_ret_msg << "\n";
        return;
    }
    std::cout << "[FUNDING]   spot Buy OK: " << spot_oid << "\n";

    // 2. Зашортить перп (БЕЗ reduceOnly — открытие)
    std::string perp_oid = api_.place_market_order_open(opp.symbol, "Sell", qty);
    if (perp_oid.empty()) {
        std::cerr << "[FUNDING] " << opp.symbol
                  << " perp short FAILED: " << api_.last_ret_msg
                  << " — unwinding spot\n";
        api_.place_market_order_spot(opp.symbol, "Sell", qty);
        return;
    }
    std::cout << "[FUNDING]   perp Short OK: " << perp_oid << "\n";

    Position p;
    p.symbol          = opp.symbol;
    p.spot_qty        = qty;
    p.perp_qty        = qty;
    p.entry_spot_price = opp.spot_ask;
    p.entry_perp_price = opp.perp_price;
    p.entry_basis_bps = opp.basis_bps;
    p.entry_funding_bps = opp.funding_bps;
    p.opened_ms       = now_ms();
    positions_.push_back(p);

    std::cout << "[FUNDING] ✅ OPENED " << opp.symbol
              << " qty=" << qty << " notional=$" << actual_notional << "\n";
}

void FundingArbStrategy::close_position(size_t idx, const std::string& reason) {
    auto p = positions_[idx];
    double age_h = (now_ms() - p.opened_ms) / 3600000.0;

    if (!cfg_.dry_run) {
        // 1. Продать спот
        std::string spot_oid = api_.place_market_order_spot(p.symbol, "Sell", p.spot_qty);
        if (spot_oid.empty()) {
            std::cerr << "[FUNDING] " << p.symbol
                      << " spot sell FAILED: " << api_.last_ret_msg
                      << " — keeping position\n";
            return;
        }
        // 2. Закрыть шорт перп (reduceOnly)
        std::string perp_oid = api_.place_market_order(p.symbol, "Buy", p.perp_qty);
        if (perp_oid.empty()) {
            std::cerr << "[FUNDING] " << p.symbol
                      << " perp close FAILED: " << api_.last_ret_msg
                      << " — MANUAL ACTION REQUIRED\n";
            return;
        }
    }

    std::cout << "[FUNDING] CLOSED " << p.symbol
              << " reason=" << reason
              << " held=" << std::fixed << std::setprecision(1) << age_h << "h"
              << " entry_funding=" << p.entry_funding_bps << " bps\n";

    Monitor::append_event(events_file(), "CLOSE",
                          p.symbol + " reason=" + reason +
                          " held_h=" + std::to_string(age_h) +
                          " funding_at_entry=" + std::to_string(p.entry_funding_bps));

    positions_.erase(positions_.begin() + idx);
    save_positions();

    positions_.erase(positions_.begin() + idx);
}

double FundingArbStrategy::get_basis_bps(const std::string& symbol) {
    auto spot = api_.get_orderbook_snapshot_spot(symbol, 1);
    auto tickers = api_.get_all_tickers_linear();
    auto it = tickers.find(symbol);
    if (it == tickers.end() || spot.bids.empty() || spot.asks.empty()) return 0.0;
    double spot_mid = (spot.bids[0].first + spot.asks[0].first) / 2.0;
    if (spot_mid <= 0) return 0.0;
    return (it->second.last_price - spot_mid) / spot_mid * 10000.0;
}

double FundingArbStrategy::get_total_notional() const {
    double total = 0.0;
    for (const auto& p : positions_) total += p.spot_qty * p.entry_spot_price;
    return total;
}

bool FundingArbStrategy::has_position(const std::string& symbol) const {
    for (const auto& p : positions_) if (p.symbol == symbol) return true;
    return false;
}

// ============================================================
// Persistence — JSON
// ============================================================
void FundingArbStrategy::save_positions() const {
    std::string path = positions_file();
    ::mkdir("data", 0755);   // создаём папку если нет

    std::ofstream f(path);
    if (!f) {
        std::cerr << "[FUNDING] Cannot write " << path << "\n";
        return;
    }

    f << "{\n";
    f << "  \"saved_at_ms\": " << now_ms() << ",\n";
    f << "  \"initial_balance\": " << initial_balance_ << ",\n";
    f << "  \"funding_collected_usd\": " << funding_collected_usd_ << ",\n";
    f << "  \"positions\": [\n";

    for (size_t i = 0; i < positions_.size(); ++i) {
        const auto& p = positions_[i];
        f << "    {"
          << "\"symbol\":\"" << p.symbol << "\","
          << "\"spot_qty\":" << p.spot_qty << ","
          << "\"perp_qty\":" << p.perp_qty << ","
          << "\"entry_spot_price\":" << p.entry_spot_price << ","
          << "\"entry_perp_price\":" << p.entry_perp_price << ","
          << "\"entry_basis_bps\":" << p.entry_basis_bps << ","
          << "\"entry_funding_bps\":" << p.entry_funding_bps << ","
          << "\"opened_ms\":" << p.opened_ms
          << "}";
        if (i + 1 < positions_.size()) f << ",";
        f << "\n";
    }
    f << "  ]\n";
    f << "}\n";
}

void FundingArbStrategy::load_positions() {
    std::string path = positions_file();
    std::ifstream f(path);
    if (!f) {
        std::cout << "[FUNDING] No saved positions in " << path << " — starting fresh\n";
        return;
    }

    // Простой парсер без simdjson (позиции — наш собственный формат)
    std::stringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();

    // Извлекаем initial_balance
    auto find_val = [&](const std::string& key) -> double {
        auto pos = content.find("\"" + key + "\"");
        if (pos == std::string::npos) return 0.0;
        pos = content.find(':', pos);
        if (pos == std::string::npos) return 0.0;
        pos++;
        while (pos < content.size() && std::isspace(content[pos])) pos++;
        char* endptr = nullptr;
        double v = std::strtod(content.c_str() + pos, &endptr);
        return v;
    };

    initial_balance_ = find_val("initial_balance");
    funding_collected_usd_ = find_val("funding_collected_usd");

    // Простой парсер массива positions
    auto arr_pos = content.find("\"positions\"");
    if (arr_pos == std::string::npos) return;

    size_t i = content.find('[', arr_pos);
    if (i == std::string::npos) return;
    i++;

    while (i < content.size()) {
        auto start = content.find('{', i);
        if (start == std::string::npos) break;
        auto end = content.find('}', start);
        if (end == std::string::npos) break;

        std::string obj = content.substr(start, end - start + 1);
        Position p;

        auto get_str = [&](const std::string& k) -> std::string {
            auto pos = obj.find("\"" + k + "\"");
            if (pos == std::string::npos) return "";
            pos = obj.find(':', pos);
            if (pos == std::string::npos) return "";
            pos = obj.find('"', pos);
            if (pos == std::string::npos) return "";
            auto end_q = obj.find('"', pos + 1);
            if (end_q == std::string::npos) return "";
            return obj.substr(pos + 1, end_q - pos - 1);
        };
        auto get_num = [&](const std::string& k) -> double {
            auto pos = obj.find("\"" + k + "\"");
            if (pos == std::string::npos) return 0.0;
            pos = obj.find(':', pos);
            if (pos == std::string::npos) return 0.0;
            pos++;
            while (pos < obj.size() && std::isspace(obj[pos])) pos++;
            return std::strtod(obj.c_str() + pos, nullptr);
        };

        p.symbol           = get_str("symbol");
        p.spot_qty         = get_num("spot_qty");
        p.perp_qty         = get_num("perp_qty");
        p.entry_spot_price = get_num("entry_spot_price");
        p.entry_perp_price = get_num("entry_perp_price");
        p.entry_basis_bps  = get_num("entry_basis_bps");
        p.entry_funding_bps = get_num("entry_funding_bps");
        p.opened_ms        = (uint64_t)get_num("opened_ms");

        if (!p.symbol.empty() && p.spot_qty > 0) {
            positions_.push_back(p);
        }

        i = end + 1;
        auto comma = content.find(',', i);
        auto close = content.find(']', i);
        if (close != std::string::npos && (comma == std::string::npos || close < comma)) break;
    }

    std::cout << "[FUNDING] Loaded " << positions_.size()
              << " positions from " << path
              << " (initial_balance=$" << initial_balance_ << ")\n";

    Monitor::append_event(events_file(), "LOAD",
                          std::to_string(positions_.size()) + " positions restored");
}

// ============================================================
// print_status — расширенный вывод
// ============================================================
void FundingArbStrategy::print_status() const {
    uint64_t now = now_ms();

    // Получить текущий баланс
    double balance = api_.get_balance("USDT");
    double unrealized = 0.0;
    double notional = 0.0;

    for (const auto& p : positions_) {
        // Текущая цена через tickers
        double cur_spot = p.entry_spot_price;
        double cur_perp = p.entry_perp_price;

        auto spot = api_.get_orderbook_snapshot_spot(p.symbol, 1);
        auto tickers = api_.get_all_tickers_linear();
        if (!spot.bids.empty() && !spot.asks.empty()) {
            cur_spot = (spot.bids[0].first + spot.asks[0].first) / 2.0;
        }
        auto it = tickers.find(p.symbol);
        if (it != tickers.end()) cur_perp = it->second.last_price;

        // Spot PnL + Perp PnL
        double spot_pnl = (cur_spot - p.entry_spot_price) * p.spot_qty;
        double perp_pnl = (p.entry_perp_price - cur_perp) * p.perp_qty;   // short
        unrealized += spot_pnl + perp_pnl;

        notional += p.spot_qty * p.entry_spot_price;
    }

    double equity = balance + unrealized;
    double net_pnl = balance - initial_balance_;

    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║  FUNDING ARB STATUS  " << Monitor::fmt_ts(now) << "      ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════╣\n";

    std::cout << std::fixed << std::setprecision(4);
    std::cout << "║  Wallet Balance:   $" << std::setw(12) << balance << "  USDT"
              << "   (" << (net_pnl >= 0 ? "+" : "") << net_pnl << " vs start)\n";
    std::cout << "║  Equity:           $" << std::setw(12) << equity << "  USDT"
              << "   (" << (unrealized >= 0 ? "+" : "") << unrealized << " unrealized)\n";
    std::cout << "║  Funding collected:$" << std::setw(12) << funding_collected_usd_ << "  USDT\n";
    std::cout << "║  Positions:        " << std::setw(13) << positions_.size()
              << "  |  Notional: $" << std::setw(9) << notional << "\n";

    std::cout << "╠══════════════════════════════════════════════════════════════╣\n";

    if (positions_.empty()) {
        std::cout << "║  No open positions                                           ║\n";
    } else {
        std::cout << "║  SYMBOL        QTY        ENTRY_BASIS  AGE       PnL        ║\n";
        std::cout << "║  ───────────────────────────────────────────────────────────  ║\n";

        for (const auto& p : positions_) {
            auto spot = api_.get_orderbook_snapshot_spot(p.symbol, 1);
            auto tickers = api_.get_all_tickers_linear();
            double cur_spot = p.entry_spot_price;
            double cur_perp = p.entry_perp_price;
            if (!spot.bids.empty() && !spot.asks.empty()) {
                cur_spot = (spot.bids[0].first + spot.asks[0].first) / 2.0;
            }
            auto it = tickers.find(p.symbol);
            if (it != tickers.end()) cur_perp = it->second.last_price;

            double spot_pnl = (cur_spot - p.entry_spot_price) * p.spot_qty;
            double perp_pnl = (p.entry_perp_price - cur_perp) * p.perp_qty;
            double pnl = spot_pnl + perp_pnl;

            double age_h = (now - p.opened_ms) / 3600000.0;

            std::cout << "║  " << std::left << std::setw(14) << p.symbol
                      << std::right << std::setw(10) << std::setprecision(2) << p.spot_qty
                      << std::setw(13) << std::setprecision(2) << p.entry_basis_bps
                      << std::setw(10) << std::setprecision(1) << age_h << "h"
                      << std::setw(12) << std::setprecision(2) << (pnl >= 0 ? "+" : "") << pnl << "\n";
        }
    }

    std::cout << "╚══════════════════════════════════════════════════════════════╝\n";
    std::cout << std::flush;

    // Сохранить snapshot в CSV
    save_monitor_csv(monitor_file());
}

// ============================================================
// save_monitor_csv
// ============================================================
void FundingArbStrategy::save_monitor_csv(const std::string& path) const {
    ::mkdir("data", 0755);

    double balance = api_.get_balance("USDT");
    double unrealized = 0.0;
    double notional = 0.0;

    for (const auto& p : positions_) {
        auto spot = api_.get_orderbook_snapshot_spot(p.symbol, 1);
        auto tickers = api_.get_all_tickers_linear();
        double cur_spot = p.entry_spot_price;
        double cur_perp = p.entry_perp_price;
        if (!spot.bids.empty() && !spot.asks.empty()) {
            cur_spot = (spot.bids[0].first + spot.asks[0].first) / 2.0;
        }
        auto it = tickers.find(p.symbol);
        if (it != tickers.end()) cur_perp = it->second.last_price;

        unrealized += (cur_spot - p.entry_spot_price) * p.spot_qty;
        unrealized += (p.entry_perp_price - cur_perp) * p.perp_qty;
        notional += p.spot_qty * p.entry_spot_price;
    }

    double equity = balance + unrealized;
    double net_pnl = balance - initial_balance_;

    Monitor::append_csv(path, now_ms(), balance, equity, unrealized,
                        positions_.size(), notional, net_pnl,
                        funding_collected_usd_);
}

// ============================================================
// print_final_summary — при shutdown
// ============================================================
void FundingArbStrategy::print_final_summary() const {
    double balance = api_.get_balance("USDT");
    double net_pnl = balance - initial_balance_;

    std::cout << "\n╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║  FINAL SUMMARY                                               ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════╣\n";
    std::cout << std::fixed << std::setprecision(4);
    std::cout << "║  Initial balance:    $" << std::setw(11) << initial_balance_ << " USDT\n";
    std::cout << "║  Final balance:      $" << std::setw(11) << balance << " USDT\n";
    std::cout << "║  Net PnL:            " << std::setw(12)
              << (net_pnl >= 0 ? "+" : "") << net_pnl << " USDT\n";
    std::cout << "║  Open positions:     " << std::setw(12) << positions_.size() << "\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n";

    if (!positions_.empty()) {
        std::cout << "\n⚠️  MANUAL ACTION REQUIRED — close on Bybit UI:\n";
        for (const auto& p : positions_) {
            std::cout << "  " << p.symbol
                      << ": SELL " << p.spot_qty << " spot + BUY "
                      << p.perp_qty << " perp\n";
        }
    }
}