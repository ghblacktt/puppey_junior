#pragma once
#include <pqxx/pqxx>
#include <string>
#include <memory>
#include <mutex>
#include "mean_reversion_strategy.h"
#include "order_book.h"
#include "cluster_engine.h"
#include "dynamic_bb_engine.h"

class SignalLogger {
public:
    explicit SignalLogger(const std::string& connection_string);
    ~SignalLogger();

    // Возвращает ID вставленного сигнала или -1 при ошибке
    int log_signal(
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
    );

private:
    std::unique_ptr<pqxx::connection> conn_;
    std::mutex mutex_;
    bool connected_ = false;
};