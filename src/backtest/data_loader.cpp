#include "data_loader.h"
#include <clickhouse/client.h>
#include <iostream>
#include <algorithm>
#include <memory>

namespace bt {

struct DataLoader::Impl {
    Config cfg;
    std::unique_ptr<clickhouse::Client> client;
};

DataLoader::DataLoader(const Config& cfg) : impl_(std::make_unique<Impl>()) {
    impl_->cfg = cfg;
    clickhouse::ClientOptions opts;
    opts.SetHost(cfg.host);
    opts.SetPort(cfg.port);
    opts.SetUser(cfg.user);
    opts.SetPassword(cfg.password);
    opts.SetDefaultDatabase(cfg.database);
    impl_->client = std::make_unique<clickhouse::Client>(opts);
}

DataLoader::~DataLoader() = default;

bool DataLoader::is_connected() const {
    return impl_ && impl_->client != nullptr;
}

    std::vector<BookSnapshot> DataLoader::load_book_updates(
            const std::string& symbol,
            uint64_t from_ts, uint64_t to_ts) {
        std::vector<BookSnapshot> out;
        out.reserve(100000);

        int n_blocks = 0;
        int n_rows   = 0;

        try {
            std::string q =
                    "SELECT ts_local, bid, ask, bid_sz, ask_sz "
                    "FROM market_data.raw_books "
                    "WHERE venue = 'bybit' AND symbol = '" + symbol + "' "
                                                                      "  AND ts_local >= " + std::to_string(from_ts) +
                    "  AND ts_local <= " + std::to_string(to_ts) +
                    "  AND bid > 0 AND ask > 0 AND bid < ask "
                    "ORDER BY ts_local";

            impl_->client->Select(q,
                                  [&out, &n_blocks, &n_rows](const clickhouse::Block& block) {
                                      ++n_blocks;

                                      if (block.GetColumnCount() < 5) return;
                                      size_t n = block.GetRowCount();
                                      if (n == 0) return;

                                      auto ts_col  = block[0]->As<clickhouse::ColumnUInt64>();
                                      auto bid_col = block[1]->As<clickhouse::ColumnFloat64>();
                                      auto ask_col = block[2]->As<clickhouse::ColumnFloat64>();
                                      auto bs_col  = block[3]->As<clickhouse::ColumnFloat64>();
                                      auto as_col  = block[4]->As<clickhouse::ColumnFloat64>();

                                      if (!ts_col || !bid_col || !ask_col || !bs_col || !as_col) {
                                          std::cerr << "[DL] load_book_updates: column mismatch\n";
                                          return;
                                      }

                                      out.reserve(out.size() + n);
                                      for (size_t i = 0; i < n; ++i) {
                                          BookSnapshot snap;
                                          snap.timestamp_ms = ts_col->At(i);
                                          snap.update_id    = snap.timestamp_ms;
                                          snap.update_type  = "snapshot";

                                          BookLevel bl;
                                          bl.price = bid_col->At(i);
                                          bl.size  = bs_col->At(i);
                                          if (bl.price <= 0 || bl.size <= 0) continue;
                                          snap.bids.push_back(bl);

                                          BookLevel al;
                                          al.price = ask_col->At(i);
                                          al.size  = as_col->At(i);
                                          if (al.price <= 0 || al.size <= 0) continue;
                                          snap.asks.push_back(al);

                                          out.push_back(std::move(snap));
                                          ++n_rows;
                                      }
                                  });
        } catch (const std::exception& e) {
            std::cerr << "[DL] load_book_updates error: " << e.what() << "\n";
        }

        std::cerr << "[DL] load_book_updates: blocks=" << n_blocks
                  << " rows=" << n_rows << "\n";
        return out;
    }


    std::vector<TradeEvent> DataLoader::load_trades(
            const std::string& symbol,
            uint64_t from_ts_ms, uint64_t to_ts_ms) {
        std::vector<TradeEvent> out;
        if (!impl_->client) return out;

        int n_blocks = 0;
        int n_rows   = 0;

        try {
            std::string q =
                    "SELECT ts_local, px, sz, aggressor "
                    "FROM market_data.raw_trades "
                    "WHERE venue = 'bybit' AND symbol = '" + symbol + "' "
                                                                      "  AND ts_local >= " + std::to_string(from_ts_ms) +
                    "  AND ts_local <= " + std::to_string(to_ts_ms) +
                    " ORDER BY ts_local";

            impl_->client->Select(q, [&out, &symbol, &n_blocks, &n_rows]
                    (const clickhouse::Block& block) {
                ++n_blocks;

                if (block.GetColumnCount() < 4) {
                    // финальный пустой блок — норм, тихо выходим
                    return;
                }
                size_t n = block.GetRowCount();
                if (n == 0) return;

                auto ts_col  = block[0]->As<clickhouse::ColumnUInt64>();
                auto px_col  = block[1]->As<clickhouse::ColumnFloat64>();
                auto sz_col  = block[2]->As<clickhouse::ColumnFloat64>();
                auto agg_col = block[3]->As<clickhouse::ColumnInt8>();

                if (!ts_col || !px_col || !sz_col || !agg_col) {
                    std::cerr << "[DL] load_trades: column type mismatch\n";
                    return;
                }

                out.reserve(out.size() + n);
                for (size_t i = 0; i < n; ++i) {
                    TradeEvent t;
                    t.symbol         = symbol;
                    t.timestamp_ms   = ts_col->At(i);
                    t.price          = px_col->At(i);
                    t.volume         = sz_col->At(i);
                    t.is_buyer_maker = (agg_col->At(i) < 0);
                    out.push_back(std::move(t));
                    ++n_rows;
                }
            });
        } catch (const std::exception& e) {
            std::cerr << "[DL] load_trades error: " << e.what() << "\n";
        }

        std::cerr << "[DL] load_trades: blocks=" << n_blocks
                  << " rows=" << n_rows << "\n";
        return out;
    }

std::vector<TimedEvent> DataLoader::load_merged(
        const std::string& symbol,
        uint64_t from_ts_ms, uint64_t to_ts_ms) {
    auto books  = load_book_updates(symbol, from_ts_ms, to_ts_ms);
    auto trades = load_trades(symbol, from_ts_ms, to_ts_ms);

    std::vector<TimedEvent> merged;
    merged.reserve(books.size() + trades.size());

    size_t bi = 0, ti = 0;
    while (bi < books.size() || ti < trades.size()) {
        bool take_trade = false;
        if (bi >= books.size()) take_trade = true;
        else if (ti >= trades.size()) take_trade = false;
        else take_trade = trades[ti].timestamp_ms <= books[bi].timestamp_ms;

        TimedEvent ev;
        if (take_trade) {
            ev.type = TimedEvent::TRADE;
            ev.timestamp_ms = trades[ti].timestamp_ms;
            ev.trade = std::move(trades[ti]);
            ++ti;
        } else {
            ev.type = TimedEvent::BOOK;
            ev.timestamp_ms = books[bi].timestamp_ms;
            ev.book = std::move(books[bi]);
            ++bi;
        }
        merged.push_back(std::move(ev));
    }

    return merged;
}

std::vector<std::string> DataLoader::available_symbols(
        uint64_t from_ts_ms, uint64_t to_ts_ms) {
    std::vector<std::string> out;
    if (!impl_->client) return out;

    // toString(symbol) - конвертирует LowCardinality(String) в обычный String
    std::string query =
        "SELECT DISTINCT toString(symbol) AS symbol FROM orderbook_updates "
        "WHERE timestamp >= " + std::to_string(from_ts_ms) + " "
        "  AND timestamp <= " + std::to_string(to_ts_ms) + " "
        "ORDER BY symbol";

    impl_->client->Select(query, [&out](const clickhouse::Block& block) {
        for (size_t row = 0; row < block.GetRowCount(); ++row) {
            auto col = block[0]->As<clickhouse::ColumnString>();
            if (col) {
                std::string_view sv = col->At(row);
                out.emplace_back(sv);
            }
        }
    });

    return out;
}


}  // namespace bt
