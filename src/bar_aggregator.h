#pragma once
#include <functional>
#include <algorithm>
#include "deque"

// Структура свечи (можно вынести в отдельный файл, если уже есть)
struct Bar {
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    double volume = 0.0;
    uint64_t timestamp = 0;
};

class BarAggregator {
public:
    // Коллбэк вызывается каждый раз, когда свеча закрывается
    using BarCallback = std::function<void(const Bar&)>;
    std::optional<Bar> add_tick(double price, double volume, uint64_t ts_ms);
    // duration_ms: например, 60000 для 1-минутных свечей
    BarAggregator(uint64_t duration_ms, BarCallback callback);
    void add_historical_bar(const Bar& bar);

    // Сюда передаем каждый тик

private:
    Bar current_bar_{};
    uint64_t duration_ms_;
    BarCallback callback_;
    std::deque<Bar> closed_bars_;   // <-- добавить
    static constexpr size_t max_bars_ = 500; //
    double last_price_ = 0.0;
    uint64_t bar_start_time_ = 0;
    bool has_bar_ = false;

    void close_bar();
};