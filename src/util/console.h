#pragma once
#include <iostream>
#include <sstream>
#include <iomanip>
#include <string>
#include <chrono>
#include <cstdio>

// ═══════════════════════════════════════════════════════════
// Цвета ANSI
// ═══════════════════════════════════════════════════════════
namespace clr {
    constexpr const char* reset   = "\033[0m";
    constexpr const char* bold    = "\033[1m";
    constexpr const char* dim     = "\033[2m";
    constexpr const char* red     = "\033[31m";
    constexpr const char* green   = "\033[32m";
    constexpr const char* yellow  = "\033[33m";
    constexpr const char* blue    = "\033[34m";
    constexpr const char* magenta = "\033[35m";
    constexpr const char* cyan    = "\033[36m";
    constexpr const char* gray    = "\033[90m";
    constexpr const char* bright_green = "\033[92m";
    constexpr const char* bright_red   = "\033[91m";
    constexpr const char* bright_cyan  = "\033[96m";
}

// ═══════════════════════════════════════════════════════════
// Цветная печать
// ═══════════════════════════════════════════════════════════
inline void log_info (const std::string& s) { std::cout << clr::cyan  << s << clr::reset << "\n"; }
inline void log_ok   (const std::string& s) { std::cout << clr::green << s << clr::reset << "\n"; }
inline void log_warn (const std::string& s) { std::cout << clr::yellow << s << clr::reset << "\n"; }
inline void log_err  (const std::string& s) { std::cerr << clr::red   << s << clr::reset << "\n"; }
inline void log_dbg  (const std::string& s) { std::cout << clr::gray  << s << clr::reset << "\n"; }

// ═══════════════════════════════════════════════════════════
// Rate limiter — не чаще N раз в M секунд
// ═══════════════════════════════════════════════════════════
struct RateLimiter {
    std::chrono::steady_clock::time_point last;
    int64_t interval_ms;
    explicit RateLimiter(int64_t ms = 5000) : interval_ms(ms) {}
    bool ok() {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last).count() < interval_ms) return false;
        last = now;
        return true;
    }
};

// ═══════════════════════════════════════════════════════════
// Форматирование
// ═══════════════════════════════════════════════════════════
inline std::string fmt_ts() {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count() % 1000;
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
    char out[24];
    std::snprintf(out, sizeof(out), "%s.%03lld", buf, (long long)ms);
    return out;
}

// pad справа (выравнивание влево)
inline std::string pad_r(const std::string& s, size_t w) {
    if (s.size() >= w) return s;
    return s + std::string(w - s.size(), ' ');
}
// pad слева (выравнивание вправо)
inline std::string pad_l(const std::string& s, size_t w) {
    if (s.size() >= w) return s;
    return std::string(w - s.size(), ' ') + s;
}
// число с фиксированной точностью
inline std::string num(double v, int prec) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(prec) << v;
    return ss.str();
}