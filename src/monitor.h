#pragma once
#include <string>
#include <fstream>
#include <chrono>
#include <ctime>
#include <sstream>
#include <iomanip>

class Monitor {
public:
    // Append-only CSV: timestamp,balance,equity,unrealized,positions,notional,net_pnl
    static void append_csv(const std::string& path,
                           uint64_t ts_ms,
                           double balance,
                           double equity,
                           double unrealized,
                           size_t positions,
                           double notional,
                           double net_pnl,
                           double funding_collected) {
        bool need_header = !file_exists(path);
        std::ofstream f(path, std::ios::app);
        if (!f) return;
        if (need_header) {
            f << "timestamp,balance_usdt,equity_usdt,unrealized_pnl,"
                 "positions,notional_usd,net_pnl_usd,funding_collected_usd\n";
        }
        f << fmt_ts(ts_ms) << ","
          << std::fixed << std::setprecision(4)
          << balance << ","
          << equity << ","
          << unrealized << ","
          << positions << ","
          << notional << ","
          << net_pnl << ","
          << funding_collected << "\n";
    }

    // Append-only plain log: timestamp | event | details
    static void append_event(const std::string& path,
                             const std::string& event,
                             const std::string& details) {
        std::ofstream f(path, std::ios::app);
        if (!f) return;
        uint64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        f << fmt_ts(ts) << " | " << event << " | " << details << "\n";
    }

    static std::string fmt_ts(uint64_t ts_ms) {
        std::time_t t = ts_ms / 1000;
        std::tm tm_buf;
        localtime_r(&t, &tm_buf);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
        return buf;
    }

private:
    static bool file_exists(const std::string& p) {
        std::ifstream f(p);
        return f.good();
    }
};