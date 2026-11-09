#pragma once
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <chrono>
#include <functional>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include "mm_symbol_registry.h"

class BybitWSClient;

class SlowLoopSupervisor : public std::enable_shared_from_this<SlowLoopSupervisor> {
public:
    struct Config {
        int  check_interval_sec;
        bool enabled;
        bool log_verbose;

        Config() : check_interval_sec(30), enabled(true), log_verbose(true) {}
    };

    using WsWeakPtr = std::weak_ptr<BybitWSClient>;

    SlowLoopSupervisor(boost::asio::io_context& exec_ioc,
                       boost::asio::io_context& supervisor_ioc,
                       const std::string& conn_str,
                       std::shared_ptr<MmSymbolRegistry> registry,
                       WsWeakPtr ws_client,
                       Config cfg = Config());

    void start();
    void stop();

private:
    void schedule_check();
    void do_check();

    // Выполняются в exec_ioc через net::post — там MM живёт
    void request_enable(const std::string& symbol);
    void request_disable(const std::string& symbol);

    boost::asio::io_context& exec_ioc_;
    boost::asio::io_context& supervisor_ioc_;
    std::string conn_str_;
    std::shared_ptr<MmSymbolRegistry> registry_;
    WsWeakPtr ws_client_;
    boost::asio::steady_timer timer_;
    Config cfg_;

    std::atomic<bool> running_{false};
    uint64_t last_check_ms_ = 0;
};