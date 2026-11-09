#include "order_executor.h"
#include <iostream>
#include <chrono>

OrderExecutor::OrderExecutor(BybitApiClient& api_client)
        : api_client_(api_client) {}

OrderExecutor::~OrderExecutor() {
    stop();
}

void OrderExecutor::start() {
    if (running_) return;

    running_ = true;
    monitor_thread_ = std::thread([this]() {
        monitor_loop();
    });

    std::cout << "[OrderExecutor] Started monitoring orders\n" << std::flush;
}

void OrderExecutor::stop() {
    if (!running_) return;

    running_ = false;
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }

    std::cout << "[OrderExecutor] Stopped\n" << std::flush;
}

std::string OrderExecutor::execute_trade(const ActiveTrade& trade) {
    std::string side = (trade.signal_type == "LONG") ? "Buy" : "Sell";

    double raw_qty = trade.position_size_usdt / trade.entry_price;
    double normalized_qty = api_client_.normalize_qty(trade.symbol, raw_qty);

    std::cout << "[OrderExecutor] 📈 FUTURES " << side << " " << trade.symbol
              << " | Price: " << trade.entry_price
              << " | Qty: " << normalized_qty
              << " | TP: " << trade.take_profit_1
              << " | SL: " << trade.stop_loss << "\n" << std::flush;
    std::string order_id = api_client_.place_limit_order(
            trade.symbol, trade.signal_type == "LONG" ? "Buy" : "Sell",
            trade.entry_price, normalized_qty, 0, 0, 0);

    if (order_id.empty()) {
        std::cerr << "[OrderExecutor] Failed to place order\n" << std::flush;
        return "";
    }

    PendingOrder pending;
    pending.order_id = order_id;
    pending.symbol = trade.symbol;
    pending.side = side;
    pending.price = trade.entry_price;
    pending.qty = normalized_qty;
    pending.placed_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
    ).count();
    pending.is_entry = true;


    pending.take_profit = 0;
    pending.stop_loss = 0;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_orders_[order_id] = pending;
    }

    return order_id;
}

bool OrderExecutor::close_position(const ActiveTrade& trade, const std::string& side, double qty) {
    auto position = api_client_.get_position_info(trade.symbol);

    if (!position.is_valid) {
        std::cout << "[OrderExecutor] ⚠️ No open position on exchange for " << trade.symbol
                  << ", skipping close\n" << std::flush;
        return true;  // Возвращаем true, т.к. позиции уже нет (это не ошибка)
    }

    // Используем реальный размер позиции
    double normalized_qty = api_client_.normalize_qty(trade.symbol, position.size);

    // Получаем текущую цену для закрытия
    double current_price = api_client_.get_last_price(trade.symbol);
    if (current_price <= 0) {
        std::cerr << "[OrderExecutor] ❌ Cannot get current price for " << trade.symbol << "\n" << std::flush;
        return false;
    }

    // Для закрытия LONG (Sell) — ставим лимитку чуть ниже текущей цены
    // Для закрытия SHORT (Buy) — ставим лимитку чуть выше текущей цены
    double limit_price;
    if (side == "Sell") {
        limit_price = current_price * 0.9995;  // На 0.05% ниже
    } else {
        limit_price = current_price * 1.0005;  // На 0.05% выше
    }

    std::cout << "[OrderExecutor] 🚪 Closing position: " << trade.symbol
              << " | Side: " << side
              << " | Qty: " << normalized_qty
              << " | Current Price: " << current_price
              << " | Limit Price: " << limit_price << "\n" << std::flush;

    std::string order_id = api_client_.place_close_order(
            trade.symbol,
            side,
            limit_price,
            normalized_qty
    );

    if (order_id.empty()) {
        std::cerr << "[OrderExecutor] ❌ Failed to close position for " << trade.symbol << "\n" << std::flush;
        return false;
    }

    std::cout << "[OrderExecutor] ✅ Close order placed: " << order_id << "\n" << std::flush;

    PendingOrder pending;
    pending.order_id = order_id;
    pending.symbol = trade.symbol;
    pending.side = side;
    pending.qty = normalized_qty;
    pending.price = limit_price;
    pending.placed_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
    ).count();
    pending.is_entry = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_orders_[order_id] = pending;
    }

    return true;
}

bool OrderExecutor::update_trailing_stop(const std::string& symbol, const std::string& side,
                                         double take_profit, double new_stop_loss,
                                         double position_size) {
    std::cout << "[OrderExecutor] 🔄 Updating trailing stop: " << symbol
              << " | Side: " << side
              << " | TP: " << take_profit
              << " | New SL: " << new_stop_loss
              << " | Size: " << position_size << "\n" << std::flush;

    bool success = api_client_.set_trading_stop(symbol, side, take_profit, new_stop_loss, position_size);

    if (success) {
        std::cout << "[OrderExecutor] ✅ Trailing stop updated on exchange\n" << std::flush;
    } else {
        std::cerr << "[OrderExecutor] ❌ Failed to update trailing stop\n" << std::flush;
    }

    return success;
}

bool OrderExecutor::cancel_trade(const std::string& order_id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = pending_orders_.find(order_id);
    if (it == pending_orders_.end()) {
        return false;
    }

    bool success = api_client_.cancel_order(it->second.symbol, order_id);

    if (success) {
        pending_orders_.erase(it);
        std::cout << "[OrderExecutor] Order cancelled: " << order_id << "\n" << std::flush;
    }

    return success;
}

void OrderExecutor::monitor_loop() {
    while (running_) {
        check_order_status();
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void OrderExecutor::check_order_status() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> to_remove;

    for (auto& [order_id, pending] : pending_orders_) {
        std::cout << "[OrderExecutor] Checking order " << order_id << " for " << pending.symbol << "\n" << std::flush;

        BybitOrder order = api_client_.get_order(pending.symbol, order_id);

        if (order.order_id.empty()) {
            std::cerr << "[OrderExecutor] Order not found, skipping\n" << std::flush;
            continue;
        }

        std::cout << "[OrderExecutor] Order status: " << order.order_status << "\n" << std::flush;

        if (order.order_status == "Filled") {
            uint64_t fill_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();

            uint64_t fill_duration_ms = fill_time - pending.placed_time;

            std::cout << "[OrderExecutor] ✅ Order FILLED in " << fill_duration_ms << " ms\n" << std::flush;

            // Уведомляем TradeTracker о времени заполнения
            // (нужно добавить колбэк или поле в ActiveTrade)

            to_remove.push_back(order_id);
        }
        else if (order.order_status == "Cancelled" || order.order_status == "Rejected") {
            std::cout << "[OrderExecutor] ❌ Order " << order.order_status << ": " << order_id << "\n" << std::flush;
            to_remove.push_back(order_id);
        }
        else if (order.order_status == "New" || order.order_status == "PartiallyFilled") {
            uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()
            ).count();

            if (now - pending.placed_time > 6000) {
                std::cout << "[OrderExecutor] ⏰ Order timeout, cancelling: " << order_id << "\n" << std::flush;
                api_client_.cancel_order(pending.symbol, order_id);
                to_remove.push_back(order_id);
            }
        }
    }

    for (const auto& order_id : to_remove) {
        pending_orders_.erase(order_id);
    }
}