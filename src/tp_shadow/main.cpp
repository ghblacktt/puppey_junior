#include <clickhouse/client.h>
#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <chrono>
#include "true_price/true_price_engine.h"

struct TradeRow {
    uint64_t ts_local;
    std::string venue;
    double px;
    double sz;
    int8_t aggressor;
    double bbo_bid;
    double bbo_ask;
};

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: tp_shadow SYMBOL HOURS\n";
        return 1;
    }
    std::string symbol = argv[1];
    int hours = std::stoi(argv[2]);
    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    uint64_t from_ms = now_ms - uint64_t(hours) * 3600 * 1000;

    try {
        clickhouse::ClientOptions opts;
        opts.SetHost("localhost").SetPort(9000)
                .SetUser("robot").SetPassword("robot_pass")
                .SetDefaultDatabase("market_data");
        clickhouse::Client client(opts);

        std::vector<TradeRow> rows;

        std::string q =
                "SELECT "
                "  toUInt64(ts_local) AS ts_local, "
                "  CAST(venue AS String) AS venue, "
                "  toFloat64(px) AS px, "
                "  toFloat64(sz) AS sz, "
                "  toInt32(aggressor) AS aggressor, "
                "  toFloat64(bbo_bid) AS bbo_bid, "
                "  toFloat64(bbo_ask) AS bbo_ask "
                "FROM raw_trades "
                "WHERE symbol = '" + symbol + "' "
                                              "  AND venue IN ('bybit','binance') "
                                              "  AND ts_local >= " + std::to_string(from_ms) +
                " ORDER BY ts_local";

        client.Select(q, [&rows](const clickhouse::Block& b) {
            if (b.GetColumnCount() < 7) return;
            size_t n = b.GetRowCount();
            if (n == 0) return;

            auto ts  = b[0]->As<clickhouse::ColumnUInt64>();
            auto ven = b[1]->As<clickhouse::ColumnString>();
            auto px  = b[2]->As<clickhouse::ColumnFloat64>();
            auto sz  = b[3]->As<clickhouse::ColumnFloat64>();
            auto ag  = b[4]->As<clickhouse::ColumnInt32>();
            auto bb  = b[5]->As<clickhouse::ColumnFloat64>();
            auto ba  = b[6]->As<clickhouse::ColumnFloat64>();

            if (!ts || !ven || !px || !sz || !ag || !bb || !ba) return;

            rows.reserve(rows.size() + n);
            for (size_t i = 0; i < n; ++i) {
                TradeRow r;
                r.ts_local  = ts->At(i);
                auto sv = ven->At(i);
                r.venue     = std::string(sv.data(), sv.size());
                r.px        = px->At(i);
                r.sz        = sz->At(i);
                r.aggressor = static_cast<int8_t>(ag->At(i));
                r.bbo_bid   = bb->At(i);
                r.bbo_ask   = ba->At(i);
                rows.push_back(std::move(r));
            }
        });

        std::cerr << "[TP_SHADOW] loaded " << rows.size() << " trades\n";
        if (rows.empty()) return 1;

        // После load trades:
        struct BookRow { uint64_t ts; double mid; };
        std::vector<BookRow> books;

        std::string q_book =
                "SELECT toUInt64(ts_local) AS ts, "
                "       toFloat64((bid+ask)/2) AS mid "
                "FROM raw_books "
                "WHERE symbol = '" + symbol + "' AND venue='bybit' "
                                              "  AND ts_local >= " + std::to_string(from_ms) +
                "  AND bid > 0 AND ask > 0 "
                "ORDER BY ts_local";

        client.Select(q_book, [&books](const clickhouse::Block& b) {
            if (b.GetColumnCount() < 2) return;
            auto ts = b[0]->As<clickhouse::ColumnUInt64>();
            auto mid = b[1]->As<clickhouse::ColumnFloat64>();
            if (!ts || !mid) return;
            for (size_t i = 0; i < b.GetRowCount(); ++i) {
                books.push_back({ts->At(i), mid->At(i)});
            }
        });

        std::cerr << "[TP_SHADOW] loaded " << books.size() << " books\n";
        if (books.empty()) {
            std::cerr << "[TP_SHADOW] WARN: no books, future_mid will be 0\n";
        }

        // ── median_sz для нормализации ──
        std::vector<double> all_sz;
        all_sz.reserve(rows.size());
        for (const auto& r : rows) if (r.sz > 0) all_sz.push_back(r.sz);
        std::sort(all_sz.begin(), all_sz.end());
        double median_sz = all_sz.empty() ? 1.0 : all_sz[all_sz.size() / 2];
        std::cerr << "[TP_SHADOW] median_sz = " << median_sz << "\n";

        // ── DDL ──
        client.Execute(
                "CREATE TABLE IF NOT EXISTS market_data.tp_shadow_output ("
                " ts UInt64,"
                " symbol LowCardinality(String),"
                " target_skew_bps Float64,"
                " mid_bybit Float64,"
                " venue_src LowCardinality(String),"
                " side_src Int8,"
                " sz_norm Float64,"
                " future_mid_100ms Float64,"
                " future_mid_1s Float64,"
                " future_mid_5s Float64"
                ") ENGINE = MergeTree ORDER BY (symbol, ts)"
                " TTL toDateTime(ts/1000) + INTERVAL 7 DAY");

        // ── Engine ──
        tp::TruePriceEngine eng;
        tp::TruePriceEngine::Config cfg;
        cfg.max_skew_bps = 20.0;   // было 50 — снижаем saturation
// Amplitude по умолчанию 2.0 — уменьшаем для активных монет
        cfg.amplitude[0][0] = 0.5;  cfg.amplitude[0][1] = 0.5;  // bybit
        cfg.amplitude[1][0] = 0.5;  cfg.amplitude[1][1] = 0.5;  // binance
        eng.set_config(cfg);

        auto find_mid_at = [&books](uint64_t target_ts) -> double {
            if (books.empty()) return 0.0;
            size_t lo = 0, hi = books.size();
            while (lo < hi) {
                size_t m = (lo + hi) / 2;
                if (books[m].ts < target_ts) lo = m + 1;
                else                         hi = m;
            }
            if (lo >= books.size()) return 0.0;
            // closest = min(|books[lo].ts - target|, |books[lo-1].ts - target|)
            uint64_t best_ts = books[lo].ts;
            double best_mid = books[lo].mid;
            if (lo > 0) {
                uint64_t d1 = books[lo].ts - target_ts;
                uint64_t d2 = target_ts - books[lo-1].ts;
                if (d2 < d1) {
                    best_ts = books[lo-1].ts;
                    best_mid = books[lo-1].mid;
                }
            }
            // reject if snapshot too far (>500ms)
            uint64_t gap = (best_ts > target_ts) ? (best_ts - target_ts)
                                                 : (target_ts - best_ts);
            if (gap > 500) return 0.0;
            return best_mid;
        };

        constexpr size_t BATCH = 5000;
        for (size_t base = 0; base < rows.size(); base += BATCH) {
            size_t end = std::min(base + BATCH, rows.size());

            clickhouse::Block block;
            auto c_ts   = std::make_shared<clickhouse::ColumnUInt64>();
            auto c_sym  = std::make_shared<clickhouse::ColumnString>();
            auto c_skew = std::make_shared<clickhouse::ColumnFloat64>();
            auto c_mid  = std::make_shared<clickhouse::ColumnFloat64>();
            auto c_ven  = std::make_shared<clickhouse::ColumnString>();
            auto c_side = std::make_shared<clickhouse::ColumnInt8>();
            auto c_sz   = std::make_shared<clickhouse::ColumnFloat64>();
            auto c_100  = std::make_shared<clickhouse::ColumnFloat64>();
            auto c_1s   = std::make_shared<clickhouse::ColumnFloat64>();
            auto c_5s   = std::make_shared<clickhouse::ColumnFloat64>();

            for (size_t i = base; i < end; ++i) {
                const auto& r = rows[i];
                if (r.bbo_bid <= 0 || r.bbo_ask <= 0) continue;

                // ── log1p нормализация на медиану ──
                double sz_norm = std::log1p(r.sz / std::max(median_sz, 1e-9))
                                 / std::log1p(5.0);
                sz_norm = std::min(1.0, std::max(0.0, sz_norm));

                int venue_id = (r.venue == "bybit") ? 0 : 1;
                int side_id  = (r.aggressor < 0) ? 1 : 0;

// ── skew ДО текущего трейда (без contamination) ──
                double skew = eng.target_skew_bps(r.ts_local);
                double mid  = (r.bbo_bid + r.bbo_ask) * 0.5;

// ── добавляем трейд в state ПОСЛЕ расчёта ──
                eng.on_trade(venue_id, side_id, sz_norm, r.ts_local);

                c_ts->Append(r.ts_local);
                c_sym->Append(symbol);
                c_skew->Append(skew);
                c_mid->Append(mid);
                c_ven->Append(r.venue);
                c_side->Append(static_cast<int8_t>(r.aggressor));
                c_sz->Append(sz_norm);
                c_100->Append(find_mid_at(r.ts_local + 100));
                c_1s ->Append(find_mid_at(r.ts_local + 1000));
                c_5s ->Append(find_mid_at(r.ts_local + 5000));
            }

            block.AppendColumn("ts",               c_ts);
            block.AppendColumn("symbol",           c_sym);
            block.AppendColumn("target_skew_bps",  c_skew);
            block.AppendColumn("mid_bybit",        c_mid);
            block.AppendColumn("venue_src",        c_ven);
            block.AppendColumn("side_src",         c_side);
            block.AppendColumn("sz_norm",          c_sz);
            block.AppendColumn("future_mid_100ms", c_100);
            block.AppendColumn("future_mid_1s",    c_1s);
            block.AppendColumn("future_mid_5s",    c_5s);

            client.Insert("tp_shadow_output", block);
            std::cerr << "[TP_SHADOW] flushed " << end << "/" << rows.size() << "\n";
        }

        std::cerr << "[TP_SHADOW] done\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "[TP_SHADOW] ERROR: " << e.what() << "\n";
        return 2;
    }
}