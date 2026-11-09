#pragma once
#include "bybit/bybit_api_client.h"
#include "common_types.h"
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <map>
#include <chrono>

class OrderExecutor {
public:
    explicit OrderExecutor(BybitApiClient& api_client);
    ~OrderExecutor();

    void start();
    void stop();

    std::string execute_trade(const ActiveTrade& trade);
    bool close_position(const ActiveTrade& trade, const std::string& side, double qty);
    bool update_trailing_stop(const std::string& symbol, const std::string& side,
                              double take_profit, double stop_loss, double qty);
    bool cancel_trade(const std::string& order_id);   // <-- добавлено

    BybitApiClient& get_api_client() { return api_client_; }

private:
    struct PendingOrder {
        std::string order_id;          // <-- добавлено
        uint64_t placed_time = 0;      // <-- добавлено
        bool is_entry = false;         // <-- добавлено
        double take_profit = 0.0;      // <-- добавлено
        double stop_loss = 0.0;        // <-- добавлено
        std::string symbol;
        std::string side;
        double qty = 0.0;
        double price = 0.0;
        bool is_close = false;
    };

    void check_order_status();         // <-- добавлено
    void monitor_loop();

    BybitApiClient& api_client_;
    std::thread monitor_thread_;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    std::map<std::string, PendingOrder> pending_orders_;
};