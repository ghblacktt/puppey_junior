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
        uint64_t from_ts_ms, uint64_t to_ts_ms) {
    std::vector<BookSnapshot> out;
    if (!impl_->client) return out;

    // toString(update_type) - конвертирует LowCardinality(String) в обычный String
    std::string query =
        "SELECT timestamp, update_id, toString(update_type) AS update_type, "
        "       arrayMap(x -> toFloat64(x), bid_prices) AS bp, "
        "       arrayMap(x -> toFloat64(x), bid_sizes)  AS bs, "
        "       arrayMap(x -> toFloat64(x), ask_prices) AS ap, "
        "       arrayMap(x -> toFloat64(x), ask_sizes)  AS as "
        "FROM orderbook_updates "
        "WHERE symbol = '" + symbol + "' "
        "  AND timestamp >= " + std::to_string(from_ts_ms) + " "
        "  AND timestamp <= " + std::to_string(to_ts_ms) + " "
        "ORDER BY timestamp, update_id";

    impl_->client->Select(query, [&out, &symbol](const clickhouse::Block& block) {
        for (size_t row = 0; row < block.GetRowCount(); ++row) {
            BookSnapshot snap;
            snap.symbol       = symbol;
            snap.timestamp_ms = block[0]->As<clickhouse::ColumnUInt64>()->At(row);
            snap.update_id    = block[1]->As<clickhouse::ColumnUInt64>()->At(row);

            auto type_col = block[2]->As<clickhouse::ColumnString>();
            if (type_col) {
                snap.update_type = type_col->At(row);
            } else {
                snap.update_type = "delta";
            }

            auto read_doubles = [&](size_t idx) -> std::vector<double> {
                std::vector<double> res;
                auto arr = block[idx]->As<clickhouse::ColumnArray>();
                if (!arr) return res;
                auto inner = arr->GetAsColumn(row);
                if (!inner) return res;
                auto f64 = std::dynamic_pointer_cast<clickhouse::ColumnFloat64>(inner);
                if (!f64) return res;
                size_t n = f64->Size();
                res.reserve(n);
                for (size_t i = 0; i < n; ++i) res.push_back(f64->At(i));
                return res;
            };

            auto bp = read_doubles(3);
            auto bs = read_doubles(4);
            auto ap = read_doubles(5);
            auto as = read_doubles(6);

            size_t n_bids = std::min(bp.size(), bs.size());
            for (size_t i = 0; i < n_bids; ++i)
                snap.bids.push_back({bp[i], bs[i]});

            size_t n_asks = std::min(ap.size(), as.size());
            for (size_t i = 0; i < n_asks; ++i)
                snap.asks.push_back({ap[i], as[i]});

            out.push_back(std::move(snap));
        }
    });

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
