#include "slow_loop_supervisor.h"
#include "bybit/bybit_ws.h"
#include <iostream>
#include <chrono>
#include <boost/asio/post.hpp>
#include <unordered_set>
namespace {
    inline uint64_t now_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
    }
}

SlowLoopSupervisor::SlowLoopSupervisor(
        boost::asio::io_context& exec_ioc,
        boost::asio::io_context& supervisor_ioc,
        const std::string& conn_str,
        std::shared_ptr<MmSymbolRegistry> registry,
        WsWeakPtr ws_client,
        Config cfg)
        : exec_ioc_(exec_ioc),
          supervisor_ioc_(supervisor_ioc),
          conn_str_(conn_str),
          registry_(std::move(registry)),
          ws_client_(std::move(ws_client)),
          timer_(supervisor_ioc),
          cfg_(cfg) {}

void SlowLoopSupervisor::start() {
    if (!cfg_.enabled) {
        std::cout << "[SLOW-LOOP] disabled in config\n";
        return;
    }
    running_ = true;
    std::cout << "[SLOW-LOOP] started (interval=" << cfg_.check_interval_sec << "s)\n";
    schedule_check();
}

void SlowLoopSupervisor::stop() {
    running_ = false;
    timer_.cancel();
    std::cout << "[SLOW-LOOP] stopped\n";
}

void SlowLoopSupervisor::schedule_check() {
    if (!running_) return;

    timer_.expires_after(std::chrono::seconds(cfg_.check_interval_sec));
    timer_.async_wait([self = shared_from_this()](boost::system::error_code ec) {
        if (ec) return;
        self->do_check();
        self->schedule_check();
    });
}

void SlowLoopSupervisor::do_check() {
    if (!running_) return;

    auto ws = ws_client_.lock();
    if (!ws) return;

    registry_->reload();

    auto trading_now = ws->currently_trading_symbols();
    std::unordered_set<std::string> trading_set(
            trading_now.begin(), trading_now.end());

    int to_enable = 0, to_disable = 0;

    // ── Disable: только явно disabled в registry (enabled=false в БД) ──
    // Неизвестные символы (нет в БД) → is_enabled() = true → не трогаем.
    for (const auto& sym : trading_now) {
        if (!registry_->is_enabled(sym)) {
            request_disable(sym);
            ++to_disable;
        }
    }

    // ── Enable: явно enabled в БД, но не торгуется (recovery после crash) ──
    auto enabled_in_registry = registry_->enabled_symbols();
    for (const auto& sym : enabled_in_registry) {
        if (trading_set.count(sym) == 0) {
            request_enable(sym);
            ++to_enable;
        }
    }

    if (to_enable > 0 || to_disable > 0) {
        std::cout << "[SLOW-LOOP] diff: enable=" << to_enable
                  << " disable=" << to_disable
                  << " (trading=" << trading_now.size()
                  << ", explicit_enabled=" << enabled_in_registry.size()
                  << ")\n";
    } else if (cfg_.log_verbose) {
        std::cout << "[SLOW-LOOP] no changes (trading=" << trading_now.size()
                  << ", explicit_enabled=" << enabled_in_registry.size()
                  << ")\n";
    }
}

void SlowLoopSupervisor::request_enable(const std::string& symbol) {
    auto ws_weak = ws_client_;
    boost::asio::post(exec_ioc_, [ws_weak, symbol]() {
        auto ws = ws_weak.lock();
        if (!ws) return;
        try {
            ws->enable_symbol(symbol);
        } catch (const std::exception& e) {
            std::cerr << "[SLOW-LOOP] enable " << symbol
                      << " failed: " << e.what() << "\n";
        }
    });
}

void SlowLoopSupervisor::request_disable(const std::string& symbol) {
    auto ws_weak = ws_client_;
    boost::asio::post(exec_ioc_, [ws_weak, symbol]() {
        auto ws = ws_weak.lock();
        if (!ws) return;
        try {
            ws->disable_symbol(symbol);
        } catch (const std::exception& e) {
            std::cerr << "[SLOW-LOOP] disable " << symbol
                      << " failed: " << e.what() << "\n";
        }
    });
}