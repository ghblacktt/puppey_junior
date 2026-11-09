#include "trade_tracker.h"
#include "order_executor.h"
#include <iostream>
#include <iomanip>
#include <set>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <numeric>
#include <thread>
#include <chrono>

TradeTracker::TradeTracker(const std::string& connection_string,
                           PriceGetter price_getter,
                           LiquidityGetter liquidity_getter,
                           VWMAGetter vwma_getter,
                           OrderExecutor* executor)
        : get_current_price_(std::move(price_getter)),
          get_liquidity_(std::move(liquidity_getter)),
          get_vwma_(std::move(vwma_getter)),
          order_executor_(executor) {

    try {
        conn_ = std::make_unique<pqxx::connection>(connection_string);
        if (conn_->is_open()) {
            std::cout << "[TradeTracker] Connected to PostgreSQL\n";

            try {
                pqxx::nontransaction txn(*conn_);
                auto r = txn.exec("SELECT COALESCE(MAX(id), 0) + 1 FROM trade_tracking");
                if (!r.empty()) {
                    next_tracking_id_ = r[0][0].as<int>();
                    std::cout << "[TradeTracker] Next tracking ID: " << next_tracking_id_ << "\n";
                }

                txn.exec(
                        "UPDATE trade_tracking SET status = 'ORPHANED', exit_reason = 'ORPHANED' "
                        "WHERE status = 'OPEN'"
                );
                int updated = txn.exec(
                        "SELECT COUNT(*) FROM trade_tracking WHERE status = 'ORPHANED'"
                )[0][0].as<int>();

                if (updated > 0) {
                    std::cout << "[TradeTracker] Marked " << updated
                              << " orphaned trades from previous run\n";
                }
            } catch (const std::exception& e) {
                std::cerr << "[TradeTracker] Init error: " << e.what() << "\n";
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[TradeTracker] Connection failed: " << e.what() << "\n";
    }

    // Инициализация начального баланса для риск-менеджмента
    if (order_executor_) {
        initial_balance_ = order_executor_->get_api_client().get_balance("USDT");
        std::cout << "[RISK] Initial balance set to: " << initial_balance_ << " USDT\n";
    }
}

TradeTracker::~TradeTracker() {
    stop();
}

// ==================== РИСК-МЕНЕДЖМЕНТ ====================

bool TradeTracker::check_risk_limits(double current_balance) {
    if (kill_switch_active_) return false;

    if (initial_balance_ > 0) {
        double loss_pct = (initial_balance_ - current_balance) / initial_balance_;
        if (loss_pct >= max_daily_loss_pct_) {
            std::cerr << "[RISK] Daily loss limit reached! Loss: " << loss_pct * 100 << "%\n";
            kill_switch_active_ = true;
            close_all_positions();
            return false;
        }

        static double peak_balance = initial_balance_;
        if (current_balance > peak_balance) peak_balance = current_balance;
        double drawdown = (peak_balance - current_balance) / peak_balance;
        if (drawdown >= max_drawdown_pct_) {
            std::cerr << "[RISK] Max drawdown reached! Drawdown: " << drawdown * 100 << "%\n";
            kill_switch_active_ = true;
            close_all_positions();
            return false;
        }
    }
    return true;
}

void TradeTracker::close_all_positions() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, trade] : active_trades_) {
        if (order_executor_) {
            std::string close_side = (trade.signal_type == "LONG") ? "Sell" : "Buy";
            double qty = trade.position_size_usdt / get_current_price_(trade.symbol);
            order_executor_->close_position(trade, close_side, qty);
        }
    }
    active_trades_.clear();
    active_symbols_.clear();
    std::cout << "[RISK] All positions closed by kill switch.\n";
}

// ==================== ОСНОВНАЯ ЛОГИКА ====================

void TradeTracker::track_signal(
        int signal_id, const std::string& symbol, const std::string& signal_type,
        double entry_price, double stop_loss, double take_profit_1, double take_profit_2,
        uint64_t timestamp, double confidence, double volatility, double absorption_volume
) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Проверка риск-лимитов
    if (order_executor_) {
        double current_balance = order_executor_->get_api_client().get_balance("USDT");
        if (!check_risk_limits(current_balance)) {
            log_signal_not_executed(signal_id, symbol, signal_type, confidence,
                                    entry_price, stop_loss, take_profit_1, "RISK_LIMIT");
            return;
        }
    }

    if (is_in_cooldown(symbol)) {
        uint64_t remaining = get_cooldown_remaining(symbol);
        std::cout << "[TradeTracker] Cooldown for " << symbol
                  << " | Remaining: " << (remaining / 1000) << "s"
                  << " | Reason: " << last_close_reason_[symbol] << "\n";
        log_signal_not_executed(signal_id, symbol, signal_type, confidence,
                                entry_price, stop_loss, take_profit_1, "COOLDOWN");
        return;
    }

    if (order_executor_) {
        auto position = order_executor_->get_api_client().get_position_info(symbol);
        if (position.is_valid) {
            std::cout << "[TradeTracker] Position exists on exchange: " << symbol << "\n";
            log_signal_not_executed(signal_id, symbol, signal_type, confidence,
                                    entry_price, stop_loss, take_profit_1, "POSITION_EXISTS");
            return;
        }
    }

    LiquidityInfo liq;
    if (get_liquidity_) {
        liq = get_liquidity_(symbol);
    }

    if ((int)active_trades_.size() >= MAX_OPEN_POSITIONS) {
        log_signal_not_executed(signal_id, symbol, signal_type, confidence,
                                entry_price, stop_loss, take_profit_1, "MAX_POSITIONS");
        return;
    }

    if (active_symbols_.count(symbol) > 0) {
        log_signal_not_executed(signal_id, symbol, signal_type, confidence,
                                entry_price, stop_loss, take_profit_1, "DUPLICATE");
        return;
    }

    double position_size_usdt = calculate_position_size(symbol, entry_price);
    if (position_size_usdt < 200.0) {
        log_signal_not_executed(signal_id, symbol, signal_type, confidence,
                                entry_price, stop_loss, take_profit_1, "LOW_LIQUIDITY");
        return;
    }

    ActiveTrade trade;
    trade.tracking_id = next_tracking_id_++;
    trade.signal_id = signal_id;
    trade.symbol = symbol;
    trade.signal_type = signal_type;
    trade.entry_price = entry_price;
    trade.stop_loss = stop_loss;
    trade.take_profit_1 = take_profit_1;
    trade.take_profit_2 = take_profit_2;
    trade.entry_time = timestamp;
    trade.confidence = confidence;
    trade.volatility = volatility;
    trade.absorption_volume = absorption_volume;
    trade.position_size_usdt = position_size_usdt;
    trade.liquidity_depth = liq.depth_usdt;
    trade.liquidity_volume_60s = liq.volume_60s;
    trade.liquidity_spread = liq.spread_pct;
    trade.liquidity_avg_trade_size = liq.avg_trade_size;
    trade.initial_sl_distance = std::abs(entry_price - stop_loss) / entry_price;

    if (order_executor_) {
        uint64_t start = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        std::string order_id = order_executor_->execute_trade(trade);
        uint64_t end = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        trade.time_to_fill_ms = end - start;

        if (order_id.empty()) {
            log_signal_not_executed(signal_id, symbol, signal_type, confidence,
                                    entry_price, stop_loss, take_profit_1, "ORDER_FAILED");
            return;
        }
        trade.real_order_id = order_id;
        log_signal_executed(signal_id, trade.tracking_id, symbol);
    }

    if (conn_ && conn_->is_open()) {
        try {
            pqxx::work txn(*conn_);
            txn.exec(
                    "INSERT INTO trade_tracking ("
                    "id, signal_id, symbol, signal_type, entry_price, stop_loss, "
                    "take_profit_1, take_profit_2, entry_time, confidence, volatility, "
                    "absorption_volume, status, entry_order_id, position_size_usdt, "
                    "liquidity_depth, liquidity_volume_60s, liquidity_spread, "
                    "liquidity_avg_trade_size, initial_sl_distance"
                    ") VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, $11, $12, 'OPEN', "
                    "$13, $14, $15, $16, $17, $18, $19)",
                    pqxx::params{
                            trade.tracking_id, signal_id, symbol, signal_type, entry_price,
                            stop_loss, take_profit_1, take_profit_2, timestamp, confidence,
                            volatility, absorption_volume, trade.real_order_id,
                            position_size_usdt, liq.depth_usdt, liq.volume_60s,
                            liq.spread_pct, liq.avg_trade_size, trade.initial_sl_distance
                    });
            txn.commit();
        } catch (const std::exception& e) {
            std::cerr << "[TradeTracker] DB error: " << e.what() << "\n";
        }
    }

    active_trades_[trade.tracking_id] = trade;
    active_symbols_.insert(symbol);
}

void TradeTracker::log_signal_not_executed(
        int signal_id, const std::string& symbol, const std::string& signal_type,
        double confidence, double entry_price, double stop_loss, double take_profit,
        const std::string& reason
) {
    if (!conn_ || !conn_->is_open()) return;

    try {
        pqxx::work txn(*conn_);
        uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();

        txn.exec(
                "INSERT INTO signal_analysis ("
                "signal_id, symbol, signal_type, confidence, signal_time, "
                "was_executed, not_executed_reason, "
                "filtered_by_confidence, filtered_by_rr, filtered_by_liquidity, "
                "filtered_by_max_positions, filtered_by_duplicate"
                ") VALUES ($1, $2, $3, $4, $5, false, $6, $7, $8, $9, $10, $11)",
                pqxx::params{
                        signal_id, symbol, signal_type, confidence, now, reason,
                        reason == "LOW_CONFIDENCE",
                        reason == "LOW_RR",
                        reason == "LOW_LIQUIDITY",
                        reason == "MAX_POSITIONS",
                        reason == "DUPLICATE"
                });
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[TradeTracker] Error logging signal: " << e.what() << "\n";
    }
}

void TradeTracker::log_signal_executed(int signal_id, int trade_id, const std::string& symbol) {
    if (!conn_ || !conn_->is_open()) return;

    try {
        pqxx::work txn(*conn_);
        uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();

        txn.exec(
                "INSERT INTO signal_analysis ("
                "signal_id, symbol, was_executed, trade_id, signal_time"
                ") VALUES ($1, $2, true, $3, $4)",
                pqxx::params{signal_id, symbol, trade_id, now});
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[TradeTracker] Error logging executed signal: " << e.what() << "\n";
    }
}

void TradeTracker::check_active_trades() {
    std::lock_guard<std::mutex> lock(mutex_);

    auto now = std::chrono::system_clock::now();
    uint64_t current_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();

    static std::map<std::string, bool> cooldown_logged;
    for (auto& [symbol, close_time] : last_close_time_) {
        uint64_t cooldown_duration = get_cooldown_duration(last_close_reason_[symbol]);
        bool in_cooldown = (current_time - close_time) < cooldown_duration;
        if (!in_cooldown && cooldown_logged[symbol]) {
            std::cout << "[TradeTracker] ✅ Cooldown ended for " << symbol << "\n";
            cooldown_logged[symbol] = false;
        } else if (in_cooldown && !cooldown_logged[symbol]) {
            cooldown_logged[symbol] = true;
        }
    }

    std::vector<int> to_close;

    for (auto& [id, trade] : active_trades_) {
        double current_price = get_current_price_(trade.symbol);
        if (current_price <= 0) continue;

        double pnl_pct = (trade.signal_type == "LONG")
                         ? (current_price - trade.entry_price) / trade.entry_price
                         : (trade.entry_price - current_price) / trade.entry_price;

        trade.max_favorable = std::max(trade.max_favorable, pnl_pct);
        trade.max_adverse = std::min(trade.max_adverse, pnl_pct);

        double tp_distance = std::abs(trade.take_profit_1 - trade.entry_price) / trade.entry_price;

        double old_sl = trade.stop_loss;
        bool sl_updated = false;

        // Трейлинг по VWMA при 50% пути до TP
        if (trade.max_favorable > tp_distance * 0.50) {
            double current_vwma = get_vwma_(trade.symbol);
            if (current_vwma > 0) {
                if (trade.signal_type == "LONG") {
                    double new_sl = std::min(current_vwma * 0.999, current_price * 0.999);
                    if (new_sl > trade.stop_loss) {
                        trade.stop_loss = new_sl;
                        sl_updated = true;
                    }
                } else {
                    double new_sl = std::max(current_vwma * 1.001, current_price * 1.001);
                    if (new_sl < trade.stop_loss) {
                        trade.stop_loss = new_sl;
                        sl_updated = true;
                    }
                }
            }
        }

        // Фиксированный трейлинг при 75% пути до TP
        if (trade.max_favorable > tp_distance * 0.75) {
            if (trade.signal_type == "LONG") {
                double new_sl = trade.entry_price * (1.0 + tp_distance * 0.5);
                if (new_sl > trade.stop_loss) {
                    trade.stop_loss = new_sl;
                    sl_updated = true;
                }
            } else {
                double new_sl = trade.entry_price * (1.0 - tp_distance * 0.5);
                if (new_sl < trade.stop_loss) {
                    trade.stop_loss = new_sl;
                    sl_updated = true;
                }
            }
        }

        if (sl_updated && order_executor_) {
            auto position = order_executor_->get_api_client().get_position_info(trade.symbol);
            if (!position.is_valid) {
                std::cout << "[TradeTracker] ⚠️ Position already closed, skip trailing: "
                          << trade.symbol << "\n";
                close_trade(trade, current_price, "TP_SL_EXCHANGE", current_time);
                to_close.push_back(id);
                continue;
            }
            std::string side = (trade.signal_type == "LONG") ? "Buy" : "Sell";
            bool success = order_executor_->update_trailing_stop(
                    trade.symbol, side, trade.take_profit_1, trade.stop_loss,
                    position.size);
            if (success) {
                std::cout << "[TradeTracker] Trailing SL updated: "
                          << trade.symbol << " | " << old_sl << " → " << trade.stop_loss << "\n";
            }
        }

        // Timeout
        uint64_t duration_ms = current_time - trade.entry_time;
        if (duration_ms > 15 * 60 * 1000 && trade.max_favorable < tp_distance * 0.3) {
            std::cout << "[TradeTracker] Timeout for " << trade.symbol << "\n";
            if (order_executor_) {
                auto position = order_executor_->get_api_client().get_position_info(trade.symbol);
                if (!position.is_valid) {
                    close_trade(trade, current_price, "TIMEOUT_EXCHANGE", current_time);
                    to_close.push_back(id);
                    continue;
                }
                std::string close_side = (trade.signal_type == "LONG") ? "Sell" : "Buy";
                double qty = trade.position_size_usdt / current_price;
                bool closed = order_executor_->close_position(trade, close_side, qty);
                if (closed) {
                    close_trade(trade, current_price, "TIMEOUT", current_time);
                    to_close.push_back(id);
                }
            }
        }

        // TP/SL
        bool should_close = false;
        std::string reason;
        double exit_price = current_price;

        if (trade.signal_type == "LONG") {
            if (current_price >= trade.take_profit_1) { should_close = true; reason = "TP1"; }
            else if (current_price <= trade.stop_loss) { should_close = true; reason = "SL"; }
        } else {
            if (current_price <= trade.take_profit_1) { should_close = true; reason = "TP1"; }
            else if (current_price >= trade.stop_loss) { should_close = true; reason = "SL"; }
        }

        if (should_close) {
            std::cout << "[TradeTracker] Local TP/SL triggered: " << trade.symbol
                      << " | Reason: " << reason << " | Price: " << current_price << "\n";
            if (order_executor_) {
                auto position = order_executor_->get_api_client().get_position_info(trade.symbol);
                if (!position.is_valid) {
                    close_trade(trade, current_price, reason + "_EXCHANGE", current_time);
                    to_close.push_back(id);
                    continue;
                }
                std::string close_side = (trade.signal_type == "LONG") ? "Sell" : "Buy";
                double qty = trade.position_size_usdt / exit_price;
                bool closed = order_executor_->close_position(trade, close_side, qty);
                if (closed) {
                    close_trade(trade, exit_price, reason, current_time);
                    to_close.push_back(id);
                }
            }
        }
    }

    for (int id : to_close) {
        active_trades_.erase(id);
    }
}

uint64_t TradeTracker::get_cooldown_duration(const std::string& reason) const {
    if (reason.find("SL") != std::string::npos) return COOLDOWN_AFTER_SL_MS;
    else if (reason.find("TP") != std::string::npos) return COOLDOWN_AFTER_TP_MS;
    else if (reason.find("TIMEOUT") != std::string::npos) return COOLDOWN_AFTER_TIMEOUT_MS;
    return COOLDOWN_AFTER_TIMEOUT_MS;
}

bool TradeTracker::is_in_cooldown(const std::string& symbol) const {
    auto it = last_close_time_.find(symbol);
    if (it == last_close_time_.end()) return false;
    auto reason_it = last_close_reason_.find(symbol);
    if (reason_it == last_close_reason_.end()) return false;
    uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    uint64_t cooldown_duration = get_cooldown_duration(reason_it->second);
    return (now - it->second) < cooldown_duration;
}

uint64_t TradeTracker::get_cooldown_remaining(const std::string& symbol) const {
    auto it = last_close_time_.find(symbol);
    if (it == last_close_time_.end()) return 0;
    auto reason_it = last_close_reason_.find(symbol);
    if (reason_it == last_close_reason_.end()) return 0;
    uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    uint64_t cooldown_duration = get_cooldown_duration(reason_it->second);
    uint64_t elapsed = now - it->second;
    return elapsed >= cooldown_duration ? 0 : cooldown_duration - elapsed;
}

void TradeTracker::close_trade(ActiveTrade& trade, double exit_price,
                               const std::string& reason, uint64_t timestamp) {
    double actual_entry = trade.entry_price;
    double actual_exit = exit_price;
    double entry_commission = 0.0;
    double exit_commission = 0.0;
    double entry_slippage = 0.0;
    double exit_slippage = 0.0;

    last_close_time_[trade.symbol] = timestamp;
    last_close_reason_[trade.symbol] = reason;

    if (order_executor_ && !trade.real_order_id.empty()) {
        auto entry_exec = order_executor_->get_api_client().get_last_execution(
                trade.symbol, trade.real_order_id);
        if (entry_exec.is_valid) {
            actual_entry = entry_exec.exec_price;
            entry_commission = entry_exec.exec_fee;
        }

        if (!trade.exit_order_id.empty()) {
            auto exit_exec = order_executor_->get_api_client().get_last_execution(
                    trade.symbol, trade.exit_order_id);
            if (exit_exec.is_valid) {
                actual_exit = exit_exec.exec_price;
                exit_commission = exit_exec.exec_fee;
            }
        }
    }

    if (trade.entry_price > 0) {
        entry_slippage = (actual_entry - trade.entry_price) / trade.entry_price;
    }
    if (exit_price > 0) {
        exit_slippage = (actual_exit - exit_price) / exit_price;
    }

    double pnl_pct = 0.0;
    if (trade.signal_type == "LONG") {
        pnl_pct = (actual_exit - actual_entry) / actual_entry;
    } else {
        pnl_pct = (actual_entry - actual_exit) / actual_entry;
    }

    double gross_pnl = pnl_pct * trade.position_size_usdt;
    double net_pnl = gross_pnl - entry_commission - exit_commission;

    if (conn_ && conn_->is_open()) {
        try {
            pqxx::work txn(*conn_);
            txn.exec(
                    "UPDATE trade_tracking SET "
                    "exit_price = $1, exit_time = $2, exit_reason = $3, "
                    "pnl_percent = $4, max_favorable_excursion = $5, "
                    "max_adverse_excursion = $6, status = 'CLOSED', "
                    "actual_entry_price = $7, actual_exit_price = $8, "
                    "entry_commission = $9, exit_commission = $10, "
                    "net_pnl = $11, trailing_stop_updates = $12, "
                    "final_stop_loss = $13, "
                    "entry_slippage = $14, exit_slippage = $15 "
                    "WHERE id = $16",
                    pqxx::params{
                            actual_exit, timestamp, reason,
                            pnl_pct * 100.0, trade.max_favorable, trade.max_adverse,
                            actual_entry, actual_exit,
                            entry_commission, exit_commission,
                            net_pnl, trade.trailing_stop_updates,
                            trade.stop_loss,
                            entry_slippage, exit_slippage,
                            trade.tracking_id
                    });
            txn.commit();
        } catch (const std::exception& e) {
            std::cerr << "[TradeTracker] DB error: " << e.what() << "\n";
        }
    }

    active_symbols_.erase(trade.symbol);

    uint64_t duration_sec = (timestamp - trade.entry_time) / 1000;
    std::string emoji = (net_pnl > 0) ? "✅" : "❌";
    std::cout << emoji << " [TRADE] " << trade.symbol << " " << trade.signal_type
              << " | Entry: " << actual_entry << " | Exit: " << actual_exit
              << " | Reason: " << reason
              << " | PnL: " << std::fixed << std::setprecision(3) << (pnl_pct * 100) << "%"
              << " | Net PnL: " << net_pnl << " USDT"
              << " | Commission: " << (entry_commission + exit_commission) << " USDT"
              << " | Duration: " << duration_sec << "s"
              << "\n" << std::flush;

    // Запускаем markout-сбор после закрытия
    collect_markout(trade, actual_exit, timestamp);
}

double TradeTracker::calculate_position_size(const std::string& symbol, double entry_price) {
    if (!get_liquidity_) {
        std::cout << "[POSITION_SIZE] " << symbol << " | ⚠️ No liquidity getter, using 500 USDT\n";
        return 500.0;
    }

    LiquidityInfo liq = get_liquidity_(symbol);

    std::cout << "[POSITION_SIZE] " << symbol
              << " | Valid: " << (liq.is_valid ? "YES" : "NO")
              << " | Depth: " << std::fixed << std::setprecision(0) << liq.depth_usdt
              << " | Vol60s: " << liq.volume_60s
              << " | AvgTrade: " << liq.avg_trade_size
              << " | Spread: " << std::setprecision(3) << liq.spread_pct << "%"
              << "\n" << std::flush;

    if (!liq.is_valid || entry_price <= 0) {
        std::cout << "[POSITION_SIZE] " << symbol << " | ⚠️ Invalid data, using 500 USDT\n";
        return 500.0;
    }

    const double MAX_DEPTH_USAGE = 0.10;
    const double MAX_VOLUME_USAGE = 0.05;

    double limit_by_depth = liq.depth_usdt * MAX_DEPTH_USAGE;
    double limit_by_volume = liq.volume_60s * MAX_VOLUME_USAGE;
    double limit_by_avg_trade = liq.avg_trade_size * 10.0;

    std::cout << "[POSITION_SIZE] " << symbol
              << " | Limits: depth=" << std::setprecision(0) << limit_by_depth
              << " vol=" << limit_by_volume
              << " avg=" << limit_by_avg_trade
              << "\n" << std::flush;

    double spread_factor = 1.0;
    if (liq.spread_pct > 0.05) {
        spread_factor = 0.05 / liq.spread_pct;
        spread_factor = std::max(0.1, spread_factor);
        std::cout << "[POSITION_SIZE] " << symbol
                  << " | Spread penalty: " << std::setprecision(2) << spread_factor << "\n";
    }

    double final_size = std::min({limit_by_depth, limit_by_volume, limit_by_avg_trade});
    final_size *= spread_factor;
    final_size = std::max(500.0, std::min(1500.0, final_size));
    final_size = std::floor(final_size);

    std::cout << "[POSITION_SIZE] " << symbol
              << " | ✅ FINAL SIZE: " << std::setprecision(0) << final_size << " USDT"
              << "\n" << std::flush;

    return final_size;
}

void TradeTracker::start() {
    if (running_) return;
    running_ = true;
    worker_thread_ = std::thread([this]() {
        while (running_) {
            check_active_trades();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
    std::cout << "[TradeTracker] Started monitoring trades\n";
}

void TradeTracker::print_performance_summary() {
    if (!conn_ || !conn_->is_open()) return;

    try {
        pqxx::work txn(*conn_);
        auto res = txn.exec(
                "SELECT symbol, signal_type, entry_price, exit_price, "
                "net_pnl, exit_reason, exit_time, entry_time, "
                "entry_commission, exit_commission, pnl_percent, "
                "entry_slippage, exit_slippage "
                "FROM trade_tracking WHERE status = 'CLOSED' ORDER BY exit_time"
        );

        if (res.empty()) {
            std::cout << "[PERF] No closed trades yet.\n";
            return;
        }

        double total_net_pnl = 0.0;
        double total_fees = 0.0;
        int wins = 0, losses = 0;
        double gross_profit = 0.0, gross_loss = 0.0;
        std::vector<double> pnl_series;
        std::unordered_map<std::string, double> pnl_by_symbol;
        std::unordered_map<std::string, int> trades_by_reason;
        double sum_duration = 0.0;
        double sum_entry_slippage = 0.0;
        double sum_exit_slippage = 0.0;

        for (auto row : res) {
            double pnl = row["net_pnl"].as<double>();
            double fee = row["entry_commission"].as<double>() + row["exit_commission"].as<double>();
            std::string reason = row["exit_reason"].as<std::string>();
            std::string symbol = row["symbol"].as<std::string>();

            if (!row["entry_slippage"].is_null()) sum_entry_slippage += row["entry_slippage"].as<double>();
            if (!row["exit_slippage"].is_null()) sum_exit_slippage += row["exit_slippage"].as<double>();

            total_net_pnl += pnl;
            total_fees += fee;
            if (pnl > 0) { wins++; gross_profit += pnl; }
            else { losses++; gross_loss += std::abs(pnl); }

            pnl_series.push_back(pnl);
            pnl_by_symbol[symbol] += pnl;
            trades_by_reason[reason]++;

            if (!row["exit_time"].is_null() && !row["entry_time"].is_null()) {
                uint64_t exit_time = row["exit_time"].as<uint64_t>();
                uint64_t entry_time = row["entry_time"].as<uint64_t>();
                sum_duration += (exit_time - entry_time) / 1000.0;
            }
        }

        int total_trades = wins + losses;
        double win_rate = total_trades > 0 ? (double)wins / total_trades : 0.0;
        double profit_factor = gross_loss > 0 ? gross_profit / gross_loss : 999.0;
        double avg_duration = total_trades > 0 ? sum_duration / total_trades : 0.0;
        double avg_entry_slippage = total_trades > 0 ? sum_entry_slippage / total_trades : 0.0;
        double avg_exit_slippage = total_trades > 0 ? sum_exit_slippage / total_trades : 0.0;

        double equity = 0.0, peak = 0.0, max_dd = 0.0;
        for (double pnl : pnl_series) {
            equity += pnl;
            if (equity > peak) peak = equity;
            double dd = peak - equity;
            if (dd > max_dd) max_dd = dd;
        }

        double mean_pnl = pnl_series.empty() ? 0.0 :
                          std::accumulate(pnl_series.begin(), pnl_series.end(), 0.0) / pnl_series.size();
        double sq_sum = 0.0;
        for (double pnl : pnl_series) sq_sum += (pnl - mean_pnl) * (pnl - mean_pnl);
        double stddev = std::sqrt(sq_sum / pnl_series.size());
        double sharpe = stddev > 0 ? mean_pnl / stddev : 0.0;

        std::cout << "\n[PERF] ===== Performance Summary =====\n";
        std::cout << "Trades: " << total_trades << "\n";
        std::cout << "Win Rate: " << std::fixed << std::setprecision(2) << win_rate * 100 << "%\n";
        std::cout << "Profit Factor: " << profit_factor << "\n";
        std::cout << "Total Net P&L: " << total_net_pnl << " USDT\n";
        std::cout << "Total Fees: " << total_fees << " USDT\n";
        std::cout << "Max Drawdown: " << max_dd << " USDT\n";
        std::cout << "Sharpe (approx): " << sharpe << "\n";
        std::cout << "Avg Duration: " << avg_duration << " sec\n";
        std::cout << "Avg Entry Slippage: " << avg_entry_slippage << "\n";
        std::cout << "Avg Exit Slippage: " << avg_exit_slippage << "\n";

        std::cout << "\nP&L by Symbol:\n";
        for (const auto& [sym, pnl] : pnl_by_symbol) {
            std::cout << "  " << sym << ": " << pnl << " USDT\n";
        }

        std::cout << "\nTrades by Exit Reason:\n";
        for (const auto& [reason, count] : trades_by_reason) {
            std::cout << "  " << reason << ": " << count << "\n";
        }
        std::cout << "=====================================\n\n";

        std::stringstream pnl_json, reason_json;
        pnl_json << "{";
        bool first = true;
        for (const auto& [sym, pnl] : pnl_by_symbol) {
            if (!first) pnl_json << ",";
            pnl_json << "\"" << sym << "\":" << pnl;
            first = false;
        }
        pnl_json << "}";

        reason_json << "{";
        first = true;
        for (const auto& [reason, count] : trades_by_reason) {
            if (!first) reason_json << ",";
            reason_json << "\"" << reason << "\":" << count;
            first = false;
        }
        reason_json << "}";
        txn.commit();

        try {
            pqxx::work txn2(*conn_);
            txn2.exec(
                    "INSERT INTO performance_summary ("
                    "total_trades, win_rate, profit_factor, total_net_pnl, total_fees, "
                    "max_drawdown, sharpe, avg_duration_sec, pnl_by_symbol, trades_by_reason"
                    ") VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9::jsonb, $10::jsonb)",
                    pqxx::params{
                            total_trades, win_rate, profit_factor, total_net_pnl, total_fees,
                            max_dd, sharpe, avg_duration, pnl_json.str(), reason_json.str()
                    });
            txn2.commit();
            std::cout << "[PERF] Performance summary saved to DB.\n";
        } catch (const std::exception& e) {
            std::cerr << "[PERF] Error saving performance summary: " << e.what() << "\n";
        }

    } catch (const std::exception& e) {
        std::cerr << "[PERF] Error computing performance: " << e.what() << "\n";
    }
}

void TradeTracker::collect_markout(const ActiveTrade& trade, double exit_price, uint64_t exit_time) {
    if (!conn_ || !conn_->is_open()) return;

    std::thread([this, trade, exit_price, exit_time]() {
        auto get_price_after = [this, &trade](int seconds) -> double {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            return get_current_price_(trade.symbol);
        };

        double p5 = get_price_after(5);
        double p15 = get_price_after(15);
        double p30 = get_price_after(30);
        double p60 = get_price_after(60);

        auto calc = [exit_price](double p) -> double {
            if (exit_price > 0 && p > 0) return (p - exit_price) / exit_price;
            return 0.0;
        };

        try {
            pqxx::work txn(*conn_);
            txn.exec(
                    "INSERT INTO trade_markout (trade_id, symbol, exit_reason, exit_time, "
                    "markout_5s, markout_15s, markout_30s, markout_60s) "
                    "VALUES ($1, $2, $3, $4, $5, $6, $7, $8)",
                    pqxx::params{
                            trade.tracking_id, trade.symbol, last_close_reason_[trade.symbol],
                            exit_time, calc(p5), calc(p15), calc(p30), calc(p60)
                    });
            txn.commit();
            std::cout << "[MARKOUT] Collected for " << trade.symbol << "\n";
        } catch (const std::exception& e) {
            std::cerr << "[MARKOUT] Error: " << e.what() << "\n";
        }
    }).detach();
}

void TradeTracker::stop() {
    if (!running_) return;
    running_ = false;
    if (worker_thread_.joinable()) worker_thread_.join();
    std::cout << "[TradeTracker] Stopped\n";
}

bool TradeTracker::has_open_position(const std::string& symbol) const {
    return active_symbols_.count(symbol) > 0;
}

size_t TradeTracker::get_open_positions_count() {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_trades_.size();
}