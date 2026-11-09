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

        try {
            impl_->client->Select(
                    "SELECT ts_local, ts_event, bid, ask, bid_sz, ask_sz "
                    "FROM market_data.raw_books "
                    "WHERE venue = 'bybit' AND symbol = '" + symbol + "' "
                                                                      "  AND ts_local >= " + std::to_string(from_ts) +
                    "  AND ts_local <= " + std::to_string(to_ts) +
                    "  AND bid > 0 AND ask > 0 AND bid < ask "
                    "ORDER BY ts_local",
                    [&out](const clickhouse::Block& block) {
                        auto col_ts_local = block[0]->As<clickhouse::ColumnUInt64>();
                        auto col_ts_event = block[1]->As<clickhouse::ColumnUInt64>();
                        auto col_bid      = block[2]->As<clickhouse::ColumnFloat64>();
                        auto col_ask      = block[3]->As<clickhouse::ColumnFloat64>();
                        auto col_bid_sz   = block[4]->As<clickhouse::ColumnFloat64>();
                        auto col_ask_sz   = block[5]->As<clickhouse::ColumnFloat64>();

                        size_t n = block.GetRowCount();
                        out.reserve(out.size() + n);
                        for (size_t i = 0; i < n; ++i) {
                            BookSnapshot snap;
                            snap.timestamp_ms = col_ts_local->At(i);
                            snap.update_id    = col_ts_event->At(i);  // не идеально, но unused
                            snap.update_type  = "snapshot";

                            BookLevel bid;
                            bid.price = col_bid->At(i);
                            bid.size  = col_bid_sz->At(i);
                            BookLevel ask;
                            ask.price = col_ask->At(i);
                            ask.size  = col_ask_sz->At(i);

                            snap.bids.push_back(bid);
                            snap.asks.push_back(ask);
                            out.push_back(std::move(snap));
                        }
                    });
        } catch (const std::exception& e) {
            std::cerr << "[DL] load_book_updates error: " << e.what() << "\n";
        }
        return out;
    }
std::vector<TradeEvent> DataLoader::load_trades(
        const std::string& symbol,
        uint64_t from_ts_ms, uint64_t to_ts_ms) {
    std::vector<TradeEvent> out;
    if (!impl_->client) return out;

    std::string query =
        "SELECT timestamp, "
        "       toFloat64(price)  AS price, "
        "       toFloat64(volume) AS volume, "
        "       is_buyer_maker "
        "FROM market_ticks "
        "WHERE symbol = '" + symbol + "' "
        "  AND timestamp >= " + std::to_string(from_ts_ms) + " "
        "  AND timestamp <= " + std::to_string(to_ts_ms) + " "
        "ORDER BY timestamp";

    impl_->client->Select(query, [&out, &symbol](const clickhouse::Block& block) {
        for (size_t row = 0; row < block.GetRowCount(); ++row) {
            TradeEvent t;
            t.symbol         = symbol;
            t.timestamp_ms   = block[0]->As<clickhouse::ColumnUInt64>()->At(row);
            t.price          = block[1]->As<clickhouse::ColumnFloat64>()->At(row);
            t.volume         = block[2]->As<clickhouse::ColumnFloat64>()->At(row);
            t.is_buyer_maker = block[3]->As<clickhouse::ColumnUInt8>()->At(row) != 0;
            out.push_back(std::move(t));
        }
    });

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
