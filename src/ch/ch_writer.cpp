#include "ch_writer.h"
#include <clickhouse/client.h>
#include <iostream>

namespace ch {

    uint64_t fnv1a(const std::string& s) {
        uint64_t h = 14695981039346656037ULL;
        for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
        return h;
    }

    struct Writer::Impl {
        clickhouse::ClientOptions opts;
        std::unique_ptr<clickhouse::Client> client;

        explicit Impl(const Credentials& c) {
            opts.SetHost(c.host).SetPort(c.port)
                    .SetUser(c.user).SetPassword(c.password)
                    .SetDefaultDatabase("market_data");   // ← NEW
        }

        bool try_connect() {
            try {
                client = std::make_unique<clickhouse::Client>(opts);
                // force first network round-trip — .Ping() или Select
                try {
                    clickhouse::Block dummy;
                    client->Ping();   // если нет Ping — закомментируй, но проверь connect
                } catch (...) {
                    // в v2.5.1 Client ленивый — реальный connect происходит на первом Insert
                    // поэтому без явного Ping ты видишь только конструирование
                }
                return true;
            } catch (const clickhouse::ServerException& e) {
                std::cerr << "[CH-Writer] AUTH/SERVER error: " << e.what() << "\n";
                client.reset();
                return false;
            } catch (const std::exception& e) {
                std::cerr << "[CH-Writer] NETWORK error: " << e.what() << "\n";
                client.reset();
                return false;
            }
        }
    };

    Writer::Writer(const Credentials& cred, const std::vector<std::string>& symbols)
            : impl_(std::make_unique<Impl>(cred)),
              bybit_q_(1 << 15), bitunix_q_(1 << 14),
              binance_q_(1 << 15),
              symbols_(symbols)
    {
        sym_to_idx_.reserve(symbols.size() * 2);
        for (size_t i = 0; i < symbols.size(); ++i)
            sym_to_idx_[symbols[i]] = static_cast<uint32_t>(i);

        is_ch_connected_ = impl_->try_connect();
        if (is_ch_connected_)
            std::cout << "[CH-Writer] connected to " << cred.host
                      << ":" << cred.port << " (user=" << cred.user
                      << ", symbols=" << symbols.size() << ")\n";
        else
            std::cerr << "[CH-Writer] NOT CONNECTED — will retry every 10s\n";

        worker_ = std::thread([this] { worker_loop(); });
    }

    Writer::~Writer() {
        running_ = false;
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();

        if (!is_ch_connected_ || !impl_ || !impl_->client) {
            std::cout << "[CH-Writer] stopped (was disconnected)\n";
            return;
        }

        std::vector<Event> trades, books;
        Event e;
        while (bybit_q_.pop(e))   (e.kind == 0 ? trades : books).push_back(e);
        while (bitunix_q_.pop(e)) (e.kind == 0 ? trades : books).push_back(e);
        while (binance_q_.pop(e)) (e.kind == 0 ? trades : books).push_back(e);
        if (!trades.empty()) flush_batch(trades, true);
        if (!books.empty())  flush_batch(books, false);
        std::cout << "[CH-Writer] stopped\n";
    }

    void Writer::push_trade(int venue, const std::string& symbol,
                            uint64_t ts_local, uint64_t ts_event,
                            double px, double sz, int8_t aggr,
                            uint8_t is_through, uint64_t tid_hash,
                            double bbo_bid, double bbo_ask) {
        auto it = sym_to_idx_.find(symbol);
        if (it == sym_to_idx_.end()) return;
        Event ev{};
        ev.ts_local_ms = ts_local; ev.ts_event_ms = ts_event;
        ev.a = px; ev.b = sz; ev.c = bbo_bid; ev.d = bbo_ask;
        ev.trade_id_hash = tid_hash; ev.symbol_idx = it->second;
        ev.kind = 0; ev.aggressor = aggr; ev.is_through = is_through;
        ev.venue = static_cast<uint8_t>(venue);
        SpscQueue& q = (venue == VenueBybit) ? bybit_q_
                                             : (venue == VenueBitunix) ? bitunix_q_
                                                                       : binance_q_;
        if (!q.push(ev)) {
            static std::atomic<uint64_t> drops{0};
            auto n = drops.fetch_add(1, std::memory_order_relaxed);
            if ((n & 0xFF) == 0)
                std::cerr << "[CH-Writer] trade queue full (drops=" << n << ")\n";
        }
    }

    void Writer::push_book(int venue, const std::string& symbol,
                           uint64_t ts_local, uint64_t ts_event,
                           double bid, double ask, double bid_sz, double ask_sz) {
        auto it = sym_to_idx_.find(symbol);
        if (it == sym_to_idx_.end()) return;
        Event ev{};
        ev.ts_local_ms = ts_local; ev.ts_event_ms = ts_event;
        ev.a = bid; ev.b = ask; ev.c = bid_sz; ev.d = ask_sz;
        ev.trade_id_hash = 0; ev.symbol_idx = it->second;
        ev.kind = 1; ev.aggressor = 0; ev.is_through = 0;
        ev.venue = static_cast<uint8_t>(venue);
        SpscQueue& q = (venue == VenueBybit) ? bybit_q_
                                             : (venue == VenueBitunix) ? bitunix_q_
                                                                       : binance_q_;
        if (!q.push(ev)) {
            static std::atomic<uint64_t> drops{0};
            auto n = drops.fetch_add(1, std::memory_order_relaxed);
            if ((n & 0xFF) == 0)
                std::cerr << "[CH-Writer] book queue full (drops=" << n << ")\n";
        }
    }

    void Writer::flush_now() { cv_.notify_all(); }

    void Writer::worker_loop() {
        std::vector<Event> trades, books;
        trades.reserve(20000); books.reserve(20000);
        auto last_flush = std::chrono::steady_clock::now();
        auto last_reconnect = std::chrono::steady_clock::now();

        while (running_) {
            { std::unique_lock<std::mutex> lk(cv_mtx_);
                cv_.wait_for(lk, std::chrono::milliseconds(100)); }

            if (!is_ch_connected_) {
                // ── Пока нет связи — выжираем очереди в /dev/null ──
                Event e;
                while (bybit_q_.pop(e))   { /* drop */ }
                while (bitunix_q_.pop(e)) { /* drop */ }
                while (binance_q_.pop(e)) { /* drop */ }
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(
                        now - last_reconnect).count() >= 10) {
                    last_reconnect = now;
                    is_ch_connected_ = impl_->try_connect();
                    if (is_ch_connected_)
                        std::cout << "[CH-Writer] RECONNECTED — resuming\n";
                }
                continue;
            }

            Event e;
            while (bybit_q_.pop(e))   (e.kind == 0 ? trades : books).push_back(e);
            while (bitunix_q_.pop(e)) (e.kind == 0 ? trades : books).push_back(e);
            while (binance_q_.pop(e)) (e.kind == 0 ? trades : books).push_back(e);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - last_flush).count();
            if (ms < 100 && trades.size() + books.size() < 1000) continue;

            if (!trades.empty()) { flush_batch(trades, true); trades.clear(); }
            if (!books.empty())  { flush_batch(books, false); books.clear(); }
            last_flush = std::chrono::steady_clock::now();
        }
    }

    void Writer::flush_batch(std::vector<Event>& batch, bool is_trade) {
        if (batch.empty() || !is_ch_connected_ || !impl_->client) return;
        try {
            clickhouse::Block block;

            auto c_ts_local = std::make_shared<clickhouse::ColumnUInt64>();
            auto c_ts_event = std::make_shared<clickhouse::ColumnUInt64>();
            auto c_venue    = std::make_shared<clickhouse::ColumnString>();
            auto c_symbol   = std::make_shared<clickhouse::ColumnString>();

            if (is_trade) {
                auto c_px       = std::make_shared<clickhouse::ColumnFloat64>();
                auto c_sz       = std::make_shared<clickhouse::ColumnFloat64>();
                auto c_aggr     = std::make_shared<clickhouse::ColumnInt8>();
                auto c_through  = std::make_shared<clickhouse::ColumnUInt8>();
                auto c_tid      = std::make_shared<clickhouse::ColumnString>();
                auto c_bbo_bid  = std::make_shared<clickhouse::ColumnFloat64>();
                auto c_bbo_ask  = std::make_shared<clickhouse::ColumnFloat64>();

                for (auto& e : batch) {
                    c_ts_local->Append(e.ts_local_ms);
                    c_ts_event->Append(e.ts_event_ms);
                    c_venue->Append(
                            e.venue == 0 ? "bybit" :
                            e.venue == 1 ? "bitunix" :
                            "binance");
                    c_symbol->Append(symbols_[e.symbol_idx]);
                    c_px->Append(e.a);
                    c_sz->Append(e.b);
                    c_aggr->Append(e.aggressor);
                    c_through->Append(e.is_through);
                    c_tid->Append(std::to_string(e.trade_id_hash));
                    c_bbo_bid->Append(e.c);
                    c_bbo_ask->Append(e.d);
                }
                block.AppendColumn("ts_local",   c_ts_local);
                block.AppendColumn("ts_event",   c_ts_event);
                block.AppendColumn("venue",      c_venue);
                block.AppendColumn("symbol",     c_symbol);
                block.AppendColumn("px",         c_px);
                block.AppendColumn("sz",         c_sz);
                block.AppendColumn("aggressor",  c_aggr);
                block.AppendColumn("is_through", c_through);
                block.AppendColumn("trade_id",   c_tid);
                block.AppendColumn("bbo_bid",    c_bbo_bid);
                block.AppendColumn("bbo_ask",    c_bbo_ask);
                impl_->client->Insert("raw_trades", block);
            } else {
                auto c_bid    = std::make_shared<clickhouse::ColumnFloat64>();
                auto c_ask    = std::make_shared<clickhouse::ColumnFloat64>();
                auto c_bid_sz = std::make_shared<clickhouse::ColumnFloat64>();
                auto c_ask_sz = std::make_shared<clickhouse::ColumnFloat64>();

                for (auto& e : batch) {
                    c_ts_local->Append(e.ts_local_ms);
                    c_ts_event->Append(e.ts_event_ms);
                    c_venue->Append(
                            e.venue == 0 ? "bybit" :
                            e.venue == 1 ? "bitunix" :
                            "binance");
                    c_symbol->Append(symbols_[e.symbol_idx]);
                    c_bid->Append(e.a);
                    c_ask->Append(e.b);
                    c_bid_sz->Append(e.c);
                    c_ask_sz->Append(e.d);
                }
                block.AppendColumn("ts_local", c_ts_local);
                block.AppendColumn("ts_event", c_ts_event);
                block.AppendColumn("venue",    c_venue);
                block.AppendColumn("symbol",   c_symbol);
                block.AppendColumn("bid",      c_bid);
                block.AppendColumn("ask",      c_ask);
                block.AppendColumn("bid_sz",   c_bid_sz);
                block.AppendColumn("ask_sz",   c_ask_sz);
                impl_->client->Insert("raw_books", block);
            }
        } catch (const std::exception& ex) {
            std::cerr << "[CH-Writer] flush error: " << ex.what()
                      << " — will reconnect in 10s\n";
            is_ch_connected_ = false;
            impl_->client.reset();
        }
    }

} // namespace ch