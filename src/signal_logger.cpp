#include "signal_logger.h"
#include <iostream>
#include <sstream>
#include <chrono>

SignalLogger::SignalLogger(const std::string& connection_string) {
    try {
        conn_ = std::make_unique<pqxx::connection>(connection_string);
        if (conn_->is_open()) {
            connected_ = true;
            std::cout << "[SignalLogger] Connected to PostgreSQL\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "[SignalLogger] Connection failed: " << e.what() << "\n";
        connected_ = false;
    }
}

SignalLogger::~SignalLogger() {
    if (conn_ && conn_->is_open()) {
        conn_->close();
    }
}

int SignalLogger::log_signal(
        const std::string& symbol,
        const MeanReversionStrategy::SignalDetails& signal,
        const OrderBook& orderbook,
        const ClusterEngine& cluster,
        const DynamicBBEngine::Bands& bands,
        double vwma,
        double alma,
        bool at_extreme,
        bool absorption_present,
        bool delta_weakening,
        bool in_range
) {
    if (!connected_) {
        std::cerr << "[SignalLogger] Not connected to DB!\n";
        return -1;
    }

    double delta_60s = cluster.get_delta(60);
    double delta_10s = cluster.get_delta(10);

    std::string signal_type = (signal.signal == MeanReversionStrategy::LONG) ? "LONG" : "SHORT";

    std::lock_guard<std::mutex> lock(mutex_);

    try {
        pqxx::work txn(*conn_);
        auto now = std::chrono::system_clock::now();
        auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count();

        pqxx::result r = txn.exec(
                "INSERT INTO signals ("
                "symbol, signal_type, entry_price, stop_loss, take_profit_1, take_profit_2, "
                "confidence, volatility, absorption_volume, at_extreme, absorption_present, "
                "delta_weakening, in_range, created_at"
                ") VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, $13, to_timestamp($14/1000.0)) "
                "RETURNING id",
                pqxx::params{
                        symbol,
                        signal_type,
                        signal.entry_price,
                        signal.stop_loss,
                        signal.take_profit_1,
                        signal.take_profit_2,
                        signal.confidence,
                        bands.volatility,
                        0.0, // absorption_volume
                        at_extreme,
                        absorption_present,
                        delta_weakening,
                        in_range,
                        timestamp
                }
        );

        if (r.empty()) {
            std::cerr << "[SignalLogger] INSERT returned no rows!\n";
            return -1;
        }

        int inserted_id = r[0][0].as<int>();
        txn.commit();

        std::cout << "[SignalLogger] Signal committed: " << symbol << " "
                  << signal_type << " @ " << signal.entry_price << " (id=" << inserted_id << ")\n";
        return inserted_id;
    } catch (const std::exception& e) {
        std::cerr << "[SignalLogger] Error: " << e.what() << "\n";
        return -1;
    }
}