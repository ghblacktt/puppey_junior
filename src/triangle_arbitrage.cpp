#include "triangle_arbitrage.h"
#include "bybit/bybit_api_client.h"
#include <iostream>
#include <cmath>
#include <algorithm>
#include <iomanip>
#include <set>

static uint64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
}

TriangularArbitrage::TriangularArbitrage(BybitApiClient& api, Config cfg)
        : api_(api), cfg_(cfg) {}

void TriangularArbitrage::discover(const std::vector<std::string>& available_symbols) {
    std::unordered_map<std::string, bool> avail;
    for (const auto& s : available_symbols) avail[s] = true;

    auto is_stable = [](const std::string& b) {
        static const std::set<std::string> st = {
                "USDC", "USDT", "DAI", "TUSD", "USDP", "PYUSD",
                "FDUSD", "USDE", "USD1", "RLUSD", "USDD", "USDB",
                "BUSD", "GUSD", "FRAX", "LUSD", "UST"
        };
        return st.count(b) > 0;
    };

    std::vector<std::string> usdt_syms;
    for (const auto& s : available_symbols) {
        if (s.size() > 4 && s.substr(s.size() - 4) == "USDT") {
            usdt_syms.push_back(s);
        }
    }

    for (size_t i = 0; i < usdt_syms.size(); ++i) {
        for (size_t j = i + 1; j < usdt_syms.size(); ++j) {
            std::string a = usdt_syms[i];
            std::string b = usdt_syms[j];
            std::string base_a = a.substr(0, a.size() - 4);
            std::string base_b = b.substr(0, b.size() - 4);

            // Обе базы — не stablecoin
            if (is_stable(base_a) || is_stable(base_b)) continue;   // ← ДОБАВИТЬ

            std::string cross_ab = base_a + base_b;
            std::string cross_ba = base_b + base_a;

            RtTriangle tri;
            if (avail.count(cross_ab)) {
                tri = {cross_ab + "/USDT", a, b, cross_ab, base_a, base_b};
            } else if (avail.count(cross_ba)) {
                tri = {cross_ba + "/USDT", b, a, cross_ba, base_b, base_a};
            } else {
                continue;
            }

            size_t idx = triangles_.size();
            triangles_.push_back(tri);
            symbol_to_triangles_[tri.usdt_a].push_back(idx);
            symbol_to_triangles_[tri.usdt_b].push_back(idx);
            symbol_to_triangles_[tri.cross].push_back(idx);
        }
    }

    std::cout << "[ARB] Discovered " << triangles_.size() << " triangles\n";
    for (size_t i = 0; i < std::min<size_t>(triangles_.size(), 10); ++i) {
        std::cout << "[ARB]   " << triangles_[i].id
                  << " (" << triangles_[i].usdt_a << ", "
                  << triangles_[i].usdt_b << ", "
                  << triangles_[i].cross << ")\n";
    }
    if (triangles_.size() > 10) {
        std::cout << "[ARB]   ... +" << (triangles_.size() - 10) << " more\n";
    }
}

bool TriangularArbitrage::is_fresh(const std::string& sym, uint64_t now) const {
    auto it = snapshots_.find(sym);
    if (it == snapshots_.end()) return false;
    return (now - it->second.ts_ms) <= (uint64_t)cfg_.max_book_age_ms;
}

void TriangularArbitrage::on_book_update(const std::string& symbol) {
    if (!snap_fn_) return;

    double bid = 0.0, ask = 0.0;
    if (!snap_fn_(symbol, bid, ask)) return;
    if (bid <= 0 || ask <= 0) return;

    // Диагностика: первые 3 вызова на символ
    static std::unordered_map<std::string, int> dbg_count;
    auto& cnt = dbg_count[symbol];
    if (cnt < 3) {
        std::cout << "[ARB-OB] " << symbol
                  << " bid=" << bid << " ask=" << ask
                  << " in_tri=" << (symbol_to_triangles_.count(symbol) ? "yes" : "NO") << "\n";
        cnt++;
    }

    uint64_t now = now_ms();
    snapshots_[symbol] = {bid, ask, now};

    auto it = symbol_to_triangles_.find(symbol);
    if (it == symbol_to_triangles_.end()) return;
    for (size_t idx : it->second) check_triangle(idx);
}

    void TriangularArbitrage::check_triangle(size_t idx) {
        const RtTriangle& tri = triangles_[idx];
        uint64_t now = now_ms();

        checks_++;                                  // ← ПЕРЕНЕСИ СЮДА (было в конце)

        if (!is_fresh(tri.usdt_a, now)) return;
        if (!is_fresh(tri.usdt_b, now)) return;
        if (!is_fresh(tri.cross,  now)) return;

        auto& sa = snapshots_[tri.usdt_a];
        auto& sb = snapshots_[tri.usdt_b];
        auto& sc = snapshots_[tri.cross];

// ---- Защита от corrupted/crossed books ----
        if (sa.bid <= 0 || sa.ask <= 0 || sa.bid >= sa.ask) {
            static int cnt1 = 0;
            if (cnt1++ < 5) std::cerr << "[ARB-SKIP] " << tri.usdt_a
                                      << " crossed: bid=" << sa.bid << " ask=" << sa.ask << "\n";
            return;
        }
        if (sb.bid <= 0 || sb.ask <= 0 || sb.bid >= sb.ask) {
            static int cnt2 = 0;
            if (cnt2++ < 5) std::cerr << "[ARB-SKIP] " << tri.usdt_b
                                      << " crossed: bid=" << sb.bid << " ask=" << sb.ask << "\n";
            return;
        }
        if (sc.bid <= 0 || sc.ask <= 0 || sc.bid >= sc.ask) {
            static int cnt3 = 0;
            if (cnt3++ < 5) std::cerr << "[ARB-SKIP] " << tri.cross
                                      << " crossed: bid=" << sc.bid << " ask=" << sc.ask << "\n";
            return;
        }


        fresh_checks_++;                            // ← NEW: все 3 свежие

    // --- Path A: USDT → base_a → base_b → USDT ---
    // 1. Buy base_a at ask(usdt_a)      cost = ask_a per unit
    // 2. Sell base_a at bid(cross)      get  = bid_c per unit of base_a  (base_a is base in cross)
    // 3. Sell base_b at bid(usdt_b)     get  = bid_b per unit of base_b
    // ratio = bid_cross * bid_usdt_b / ask_usdt_a
    double ratio_a = sc.bid * sb.bid / sa.ask;

    // --- Path B: USDT → base_b → base_a → USDT ---
    // 1. Buy base_b at ask(usdt_b)
    // 2. Buy base_a at ask(cross)       cost = ask_cross per unit of base_a, paid in base_b
    // 3. Sell base_a at bid(usdt_a)
    // ratio = bid_usdt_a / (ask_usdt_b * ask_cross)
    double ratio_b = sa.bid / (sb.ask * sc.ask);

    // Издержки: 3 ноги, taker fee каждый
    double fee_factor = std::pow(1.0 - cfg_.taker_fee_bps / 10000.0, 3.0);
    double net_a = ratio_a * fee_factor - 1.0;
    double net_b = ratio_b * fee_factor - 1.0;

    double net_edge_bps_a = net_a * 10000.0;
    double net_edge_bps_b = net_b * 10000.0;

    // Track best
    double best = std::max(net_edge_bps_a, net_edge_bps_b);
    double prev_best = best_edge_bps_.load();
    while (best > prev_best && !best_edge_bps_.compare_exchange_weak(prev_best, best)) {}

    checks_++;

    const char* direction = nullptr;
    double edge = 0.0;
    std::vector<TriangleLeg> path;

    if (net_edge_bps_a >= cfg_.min_edge_bps) {
        direction = "A";
        edge = net_edge_bps_a;

        // Строим последовательность ордеров для Path A
        double notional = cfg_.notional_usd;
        // Leg 1: Buy base_a on usdt_a
        double qty_a = api_.normalize_qty_spot(tri.usdt_a, notional / sa.ask);
        if (qty_a <= 0) return;
        // Leg 2: Sell base_a on cross (base_a — base, base_b — quote)
        double qty_a_cross = api_.normalize_qty_spot(tri.cross, qty_a);
        if (qty_a_cross <= 0) return;
        double base_b_received = qty_a_cross * sc.bid;
        // Leg 3: Sell base_b on usdt_b
        double qty_b = api_.normalize_qty_spot(tri.usdt_b, base_b_received);
        if (qty_b <= 0) return;

        path.push_back({tri.usdt_a, "Buy",  qty_a,       sa.ask});
        path.push_back({tri.cross,  "Sell", qty_a_cross, sc.bid});
        path.push_back({tri.usdt_b, "Sell", qty_b,       sb.bid});
    } else if (net_edge_bps_b >= cfg_.min_edge_bps) {
        direction = "B";
        edge = net_edge_bps_b;

        double notional = cfg_.notional_usd;
        // Leg 1: Buy base_b on usdt_b
        double qty_b = api_.normalize_qty_spot(tri.usdt_b, notional / sb.ask);
        if (qty_b <= 0) return;
        // Leg 2: Buy base_a on cross (base_a/base_b) — cost in base_b
        double base_a_qty = qty_b / sc.ask;
        base_a_qty = api_.normalize_qty_spot(tri.cross, base_a_qty);
        if (base_a_qty <= 0) return;
        // Leg 3: Sell base_a on usdt_a
        double qty_a = api_.normalize_qty_spot(tri.usdt_a, base_a_qty);
        if (qty_a <= 0) return;

        path.push_back({tri.usdt_b, "Buy",  qty_b,     sb.ask});
        path.push_back({tri.cross,  "Buy",  base_a_qty, sc.ask});
        path.push_back({tri.usdt_a, "Sell", qty_a,     sa.bid});
    } else {
        return;
    }

    opportunities_++;

    // Cooldown
    auto now_tp = std::chrono::steady_clock::now();
    auto since_last = std::chrono::duration_cast<std::chrono::seconds>(now_tp - last_exec_).count();
    bool cooldown_ok = since_last >= (60 / std::max(1, cfg_.max_exec_per_min));

        std::cout << "[ARB] OPPORTUNITY " << tri.id
                  << " dir=" << direction
                  << " edge=" << std::fixed << std::setprecision(2) << edge << " bps"
                  << std::defaultfloat                                    // ← СБРОС
                  << " | " << tri.usdt_a << " bid/ask=" << sa.bid << "/" << sa.ask
                  << " | " << tri.usdt_b << "=" << sb.bid << "/" << sb.ask
                  << " | " << tri.cross  << "=" << sc.bid << "/" << sc.ask
                  << (cfg_.dry_run ? " [DRY-RUN]" : "")
                  << "\n";

    if (cfg_.dry_run || !cooldown_ok) return;

    try_execute(tri, direction, path, edge);
    last_exec_ = now_tp;
}


void TriangularArbitrage::try_execute(const RtTriangle& tri,
                                      const char* direction,
                                      const std::vector<TriangleLeg>& path,
                                      double edge_bps) {
    if (!place_fn_) return;

    std::vector<TriangleLeg> filled;

    for (size_t i = 0; i < path.size(); ++i) {
        const auto& leg = path[i];
        std::string oid = place_fn_(leg.symbol, leg.side, leg.price, leg.qty);

        if (oid.empty()) {
            failures_++;
            std::cerr << "[ARB] LEG " << (i+1) << " FAILED: "
                      << leg.symbol << " " << leg.side
                      << " qty=" << leg.qty
                      << " price=" << leg.price
                      << " | api_msg=" << api_.last_ret_msg
                      << " | UNWINDING " << filled.size() << " legs\n";
            unwind_exposure(filled);
            return;
        }
        filled.push_back(leg);
        std::cout << "[ARB] LEG " << (i+1) << " OK: "
                  << leg.symbol << " " << leg.side
                  << " qty=" << leg.qty
                  << " @ " << leg.price
                  << " oid=" << oid << "\n";
    }

    executions_++;
    std::cout << "[ARB] ✓ COMPLETE " << tri.id
              << " dir=" << direction
              << " edge=" << edge_bps << " bps\n";
}

void TriangularArbitrage::unwind_exposure(const std::vector<TriangleLeg>& filled) {
    for (const auto& leg : filled) {
        // Для spot: продаём остаток базовой монеты
        std::string base = leg.symbol.substr(0, leg.symbol.size() - 4);
        double bal = api_.get_balance(base);
        if (bal <= 0) continue;
        double sell_qty = api_.normalize_qty(leg.symbol, bal);
        if (sell_qty <= 0) continue;
        std::string oid = api_.place_market_order_spot(leg.symbol, "Sell", sell_qty);
        std::cerr << "[ARB] UNWIND " << leg.symbol << " Sell " << sell_qty
                  << (oid.empty() ? " FAILED" : " OK") << "\n";
    }
}

void TriangularArbitrage::print_stats() const {
    std::cout << "[ARB-STATS] triangles=" << triangles_.size()
              << " snapshots=" << snapshots_.size()        // ← NEW
              << " checks=" << checks_.load()
              << " fresh=" << fresh_checks_.load()          // ← NEW
              << " opportunities=" << opportunities_.load()
              << " executions=" << executions_.load()
              << " failures=" << failures_.load()
              << " best_edge_bps=" << std::fixed << std::setprecision(2)
              << best_edge_bps_.load() << "\n";
}