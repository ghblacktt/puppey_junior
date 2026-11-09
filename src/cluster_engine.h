#pragma once
#include <simdjson.h>
#include <string_view>
#include <map>
#include <vector>
#include <mutex>
#include <iostream>
#include <iomanip>

struct Trade {
    double price;
    double size;
    bool is_buyer_maker;  // true = продавец был агрессором (sell), false = покупатель (buy)
    double timestamp;
};

// ОДНО объявление структуры на уровне файла
// В cluster_engine.h добавь структуру
struct AbsorptionZone {
    double price;
    double total_volume;
    double buy_volume;
    double sell_volume;
    int trade_count;
    bool is_buying_absorption;

    // НОВОЕ:衡量 absorption силы
    double absorption_strength;  // 0-1, чем выше тем сильнее
    double price_stall_score;    // насколько цена "зависла" на уровне
};

class ClusterEngine {
public:
    ClusterEngine() = default;
    // VPIN (Volume-Synchronized Probability of Informed Trading)
// Возвращает [0..1]: > 0.7 = toxic flow. Считается по объёмным бакетам.
    double get_vpin(int num_buckets = 20) const;

// Сырой объём последних N секунд в USD (нужен для volume bucket sizing)
    double get_volume_usd(int seconds, double mid) const;

    // Основной метод: принимает массив сделок и обрабатывает их
    void process_trades(const simdjson::dom::element& data);
    void add_historical_trade(uint64_t timestamp, double price, double volume, bool is_buyer_maker);

    auto get_total_volume(int seconds) const -> double;

    // Получить дельту за последние N секунд
    auto get_delta(int seconds = 60) const -> double;

    // Найти зоны абсорбции
    auto find_absorption_zones(double price_range = 100.0) const -> std::vector<AbsorptionZone>;
    // Распечатать статистику (для отладки)
    void print_stats() const;

    // Новые методы
    double get_delta_momentum(int short_seconds = 10, int long_seconds = 60) const;
    double get_volume_imbalance(int seconds = 30) const;
    bool is_exhaustion_detected() const;

private:
    std::vector<Trade> trades_{};  // Кольцевой буфер последних сделок
    mutable std::mutex mutex_;
    mutable std::vector<double> vpin_buckets_;   // cache: (buy-sell)/total per bucket
    static constexpr size_t MAX_TRADES = 10000;  // Храним последние 10k сделок
};