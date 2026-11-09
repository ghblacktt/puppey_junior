#include "backtest_runner.h"
#include "data_loader.h"
#include "backtest_types.h"
#include <iostream>
#include <chrono>
#include <cstring>
#include "bybit/bybit_api_client.h"
#include "util/env.h"
#include "util/dotenv.h"


int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: bt_backtest SYMBOL [--hours N] "
                     "[--min-hs N] [--qty N] [--hold N] [--momentum-weight N]\n";
        return 1;
    }

    bt::RunConfig cfg;
    cfg.symbol = argv[1];
    uint64_t hours = 1;
    uint64_t from_ts = 0, to_ts = 0;

    for (int i = 2; i < argc; i += 2) {
        if (i + 1 >= argc) break;
        std::string k = argv[i];
        std::string v = argv[i + 1];
        if (k == "--hours")            hours = std::stoull(v);
        else if (k == "--from-ts")     from_ts = std::stoull(v);
        else if (k == "--to-ts")       to_ts = std::stoull(v);
        else if (k == "--min-hs")      cfg.min_hs_bps = std::stod(v);
        else if (k == "--qty")         cfg.qty_usd = std::stod(v);
        else if (k == "--hold")        cfg.max_hold_sec = std::stoi(v);
        else if (k == "--momentum-weight") cfg.momentum_weight = std::stod(v);
        else if (k == "--base-spread") cfg.base_spread_bps = std::stod(v);
    }

    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    uint64_t from = from_ts > 0 ? from_ts : (now_ms - hours * 3600 * 1000);
    uint64_t to   = to_ts > 0 ? to_ts : now_ms;

    bt::DataLoader::Config dl;
    bt::DataLoader loader(dl);

    std::cout << "Loading " << cfg.symbol << " last " << hours << "h...\n";
    auto events = loader.load_merged(cfg.symbol, from, to);
    std::cout << "Window: " << from << " → " << to
              << " (" << (to - from) / 60000 << " min)\n";
    std::cout << "Loaded " << events.size() << " events\n";
    if (events.empty()) {
        std::cerr << "No events, abort\n";
        return 1;
    }

    bt::InstrumentParams inst;
    inst.tick_size = 0.01;
    inst.step_size = 0.01;
    inst.min_order_qty = 0.01;
    inst.min_order_amt = 5.0;

// Загрузить реальные параметры из Bybit API
    {
        std::string key, secret;
        try {
            key    = env_required("BYBIT_API_KEY");
            secret = env_required("BYBIT_API_SECRET");
        } catch (const std::exception& e) {
            std::cerr << "[BT-INST] " << e.what()
                      << " — using defaults (tick=0.01 step=0.01)\n";
            // inst остаётся с дефолтами, идём дальше
        }

        if (!key.empty()) {
            BybitApiClient api(key, secret, /*use_testnet=*/false);
            auto info = api.get_instrument_info(cfg.symbol, "linear");
            if (info.tick_size > 0)     inst.tick_size     = info.tick_size;
            if (info.step_size > 0)     inst.step_size     = info.step_size;
            if (info.min_order_qty > 0) inst.min_order_qty = info.min_order_qty;
            if (info.min_order_amt > 0) inst.min_order_amt = info.min_order_amt;
            std::cout << "[BT-INST] " << cfg.symbol
                      << " tick=" << inst.tick_size
                      << " step=" << inst.step_size
                      << " min_qty=" << inst.min_order_qty
                      << " min_amt=" << inst.min_order_amt << "\n";
        }
    }

    bt::BacktestRunner runner(cfg, inst);
    auto r = runner.run(events);
    // Markout stats
    std::vector<double> mk_1s_vals, mk_30s_vals;
    for (const auto& m : r.markouts) {
        if (!m.finalized) continue;
        double sign = (m.side == "Buy") ? 1.0 : -1.0;
        double mk1 = (m.mid_1s - m.fill_price) / m.fill_price * 10000.0 * sign;
        double mk30 = (m.mid_30s - m.fill_price) / m.fill_price * 10000.0 * sign;
        mk_1s_vals.push_back(mk1);
        mk_30s_vals.push_back(mk30);
    }

    if (!mk_1s_vals.empty()) {
        std::sort(mk_1s_vals.begin(), mk_1s_vals.end());
        std::sort(mk_30s_vals.begin(), mk_30s_vals.end());
        auto median = [](const std::vector<double>& v) {
            return v.size() % 2 ? v[v.size()/2] : (v[v.size()/2-1] + v[v.size()/2]) / 2.0;
        };
        auto pctl = [](const std::vector<double>& v, double p) {
            return v[(size_t)((v.size()-1) * p)];
        };
        int positives_1s = 0;
        for (double v : mk_1s_vals) if (v > 0) positives_1s++;
        double hit_pct = 100.0 * positives_1s / mk_1s_vals.size();

        std::cout << "\n=== MARKOUTS ===\n";
        std::cout << "n:          " << mk_1s_vals.size() << "\n";
        std::cout << "med_1s:     " << median(mk_1s_vals) << " bps\n";
        std::cout << "med_30s:    " << median(mk_30s_vals) << " bps\n";
        std::cout << "recovery:   " << (median(mk_30s_vals) - median(mk_1s_vals)) << " bps\n";
        std::cout << "hit_pct:    " << hit_pct << "%\n";
    }

    std::cout << "\n=== BACKTEST RESULTS ===\n";
    std::cout << "Symbol:       " << cfg.symbol << "\n";
    std::cout << "Events:       " << r.total_events << " (book=" << r.book_events
              << " trade=" << r.trade_events << ")\n";
    std::cout << "Fills:        " << r.fills << " (full=" << r.full_fills << ")\n";
    std::cout << "Volume:       " << r.total_volume << "\n";
    std::cout << "Fees:         $" << r.total_fees << "\n";
    std::cout << "Realized:     $" << r.realized_pnl << "\n";
    std::cout << "Net:          $" << r.net_pnl << "\n";
    std::cout << "Final pos:    " << r.final_position_qty
              << " @ " << r.final_position_avg << "\n";
    return 0;
}