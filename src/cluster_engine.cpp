#include "cluster_engine.h"
#include <algorithm>
#include <chrono>

void ClusterEngine::process_trades(const simdjson::dom::element& data) {
    std::scoped_lock lock(mutex_);

    simdjson::dom::array trades_arr;
    if (data.get_array().get(trades_arr) != simdjson::SUCCESS) {
        return;
    }

    static uint64_t process_count = 0;
    static uint64_t total_trades_processed = 0;
    static bool first_trade_logged = false;

    process_count++;
    int trades_added = 0;
    int skip_reason_p = 0, skip_reason_v = 0, skip_reason_s = 0, skip_reason_t = 0;

    for (auto trade_elem : trades_arr) {
        Trade trade;

        // === ЛОГИРУЕМ ПЕРВЫЙ ТРЕЙД ПОЛНОСТЬЮ ===
        if (!first_trade_logged) {
            first_trade_logged = true;
        }

        // Парсим price
        double price;
        if (trade_elem["p"].get_double().get(price) != simdjson::SUCCESS) {
            // Попробуем как строку
            std::string_view price_str;
            if (trade_elem["p"].get_string().get(price_str) == simdjson::SUCCESS) {
                price = std::strtod(price_str.data(), nullptr);
            } else {
                skip_reason_p++;
                continue;
            }
        }
        trade.price = price;

        // === ПРОБЛЕМА: volume может быть числом, а не строкой! ===
        std::string_view size_str;
        if (trade_elem["v"].get_string().get(size_str) == simdjson::SUCCESS) {
            trade.size = std::strtod(size_str.data(), nullptr);
        } else {
            // Попробуем как число
            double size_double;
            if (trade_elem["v"].get_double().get(size_double) == simdjson::SUCCESS) {
                trade.size = size_double;
            } else {
                skip_reason_v++;
                continue;
            }
        }

        // Парсим side
        std::string_view side;
        if (trade_elem["S"].get_string().get(side) != simdjson::SUCCESS) {
            skip_reason_s++;
            continue;
        }
        trade.is_buyer_maker = (side == "Sell");

        // Парсим timestamp
        uint64_t timestamp_uint64 = 0;
        if (trade_elem["T"].get_uint64().get(timestamp_uint64) != simdjson::SUCCESS) {
            // Попробуем как число (int64)
            int64_t timestamp_int64;
            if (trade_elem["T"].get_int64().get(timestamp_int64) == simdjson::SUCCESS) {
                timestamp_uint64 = static_cast<uint64_t>(timestamp_int64);
            } else {
                skip_reason_t++;
                continue;
            }
        }
        trade.timestamp = static_cast<double>(timestamp_uint64);

        trades_.push_back(trade);
        trades_added++;
        total_trades_processed++;

        if (trades_.size() > MAX_TRADES) {
            trades_.erase(trades_.begin());
        }
    }

}

auto ClusterEngine::get_delta(int seconds) const -> double {
    std::scoped_lock lock(mutex_);

    if (trades_.empty()) {
        static uint64_t empty_count = 0;
        return 0.0;
    }

    uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

    // ❌ ПРОБЛЕМА: seconds * 10000 — это 10 секунд, а не 1 секунда!
    // Должно быть seconds * 1000
    uint64_t cutoff = now - (seconds * 1000);  // ← ИСПРАВЛЕНО

    double buy_volume = 0.0;
    double sell_volume = 0.0;
    int trades_in_range = 0;

    for (const auto& trade : trades_) {
        if (trade.timestamp < cutoff) continue;

        trades_in_range++;
        if (trade.is_buyer_maker) {
            sell_volume += trade.size;
        } else {
            buy_volume += trade.size;
        }
    }


    return buy_volume - sell_volume;
}

auto ClusterEngine::find_absorption_zones(double price_range) const -> std::vector<AbsorptionZone> {
    std::scoped_lock lock(mutex_);

    std::vector<AbsorptionZone> zones;

    if (trades_.size() < 100) {
        return zones;
    }

    // 1. <--- ИЗМЕНЕНО: Получаем средний объем в секунду за последние 60 сек для нормализации
    // Это делает алгоритм универсальным для ЛЮБОГО тикера (BTC, ETH, SOL и т.д.)
    double total_vol_60s = get_total_volume(60);
    double avg_vol_per_sec = std::max(total_vol_60s / 60.0, 0.001); // защита от деления на 0

    std::map<double, AbsorptionZone> zone_map;
    std::map<double, std::vector<double>> zone_timestamps;

    for (const auto& trade : trades_) {
        // Убедимся, что price_range не слишком огромный (защита от ошибок вызова)
        double actual_range = std::min(price_range, trade.price * 0.001); // макс 0.1% шаг
        double rounded_price = std::round(trade.price / actual_range) * actual_range;

        auto& zone = zone_map[rounded_price];
        zone.price = rounded_price;
        zone.total_volume += trade.size;
        zone.trade_count++;
        zone_timestamps[rounded_price].push_back(trade.timestamp);

        if (trade.is_buyer_maker) {
            zone.buy_volume += trade.size;
        } else {
            zone.sell_volume += trade.size;
        }
    }

    for (const auto& [price, zone] : zone_map) {
        // МЯГЧЕ: Минимум 5 трейдов ИЛИ объем хотя бы 1.5 секунды от среднего
        if (zone.trade_count < 5 || zone.total_volume < (avg_vol_per_sec * 1.5)) {
            continue;
        }

        const auto& timestamps = zone_timestamps[price];
        if (timestamps.size() < 2) {
            continue;
        }

        double duration_ms = timestamps.back() - timestamps.front();

        // МЯГЧЕ: Абсорбция бывает и за 200 мс. Не отсекаем быстрые всплески.
        // if (duration_ms < 1000.0) { continue; } // <-- УДАЛИ ЭТУ СТРОКУ ПОЛНОСТЬЮ

        double duration_score = std::min(duration_ms / 30000.0, 1.0);

        double volume_per_second = zone.total_volume / (duration_ms / 1000.0 + 0.001);

        // МЯГЧЕ: Достаточно, если плотность в 2 раза выше средней (было 3.0)
        double density_score = std::min(volume_per_second / (avg_vol_per_sec * 2.0), 1.0);

        double balance = std::abs(zone.buy_volume - zone.sell_volume) / zone.total_volume;
        double balance_score = balance;

        bool is_buying = (zone.buy_volume > zone.sell_volume * 1.1); // МЯГЧЕ: 1.1 вместо 1.2
        bool is_selling = (zone.sell_volume > zone.buy_volume * 1.1);

        double strength = duration_score * 0.3 + density_score * 0.5 + (1.0 - balance_score) * 0.2;

        // МЯГЧЕ: Вернули порог 0.3 вместо 0.45
        if (strength > 0.3 && (is_buying || is_selling)) {
            AbsorptionZone abs_zone = zone;
            abs_zone.is_buying_absorption = is_buying;
            abs_zone.absorption_strength = strength;
            abs_zone.price_stall_score = duration_score;
            zones.push_back(abs_zone);
        }
    }

    std::ranges::sort(zones,
                      [](const AbsorptionZone& a, const AbsorptionZone& b) {
                          return a.absorption_strength > b.absorption_strength;
                      });

    return zones;
}

void ClusterEngine::print_stats() const {
    std::scoped_lock lock(mutex_);

    std::cout << "\n=== CLUSTER STATS ===\n";
    std::cout << "Total trades: " << trades_.size() << "\n";

    double delta = 0.0;
    if (!trades_.empty()) {
        uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        uint64_t cutoff = now - (60 * 1000);

        double buy_vol = 0.0, sell_vol = 0.0;
        for (const auto& trade : trades_) {
            if (trade.timestamp < cutoff) continue;
            if (trade.is_buyer_maker) {
                sell_vol += trade.size;
            } else {
                buy_vol += trade.size;
            }
        }
        delta = buy_vol - sell_vol;
    }
    std::cout << "Delta (60s): " << std::fixed << std::setprecision(3) << delta << "\n";

    std::vector<AbsorptionZone> zones;
    if (!trades_.empty()) {
        double price_range = 50.0;
        std::map<double, AbsorptionZone> zone_map;

        for (const auto& trade : trades_) {
            double rounded_price = std::round(trade.price / price_range) * price_range;

            auto& zone = zone_map[rounded_price];
            zone.price = rounded_price;
            zone.total_volume += trade.size;
            zone.trade_count++;

            if (trade.is_buyer_maker) {
                zone.buy_volume += trade.size;
            } else {
                zone.sell_volume += trade.size;
            }
        }

        for (const auto& [price, zone] : zone_map) {
            if (zone.total_volume > 0.5 && zone.trade_count > 2) {
                AbsorptionZone abs_zone = zone;
                abs_zone.is_buying_absorption = (zone.buy_volume > zone.sell_volume);
                zones.push_back(abs_zone);
            }
        }

        std::sort(zones.begin(), zones.end(),
                  [](const AbsorptionZone& a, const AbsorptionZone& b) -> bool {
                      return a.total_volume > b.total_volume;
                  });
    }

    std::cout << "Absorption zones found: " << zones.size() << "\n";

    for (size_t i = 0; i < std::min(zones.size(), static_cast<size_t>(5)); ++i) {
        const auto& zone = zones[i];
        std::cout << "  Price: " << std::fixed << std::setprecision(1) << zone.price
                  << " | Volume: " << std::setprecision(3) << zone.total_volume
                  << " | Buy: " << std::setprecision(3) << zone.buy_volume
                  << " | Sell: " << std::setprecision(3) << zone.sell_volume
                  << " | Trades: " << zone.trade_count
                  << " | Type: " << (zone.is_buying_absorption ? "BUY ABSORPTION" : "SELL ABSORPTION")
                  << "\n";
    }
    std::cout << "=====================\n\n";
    std::cout << std::flush;
}

double ClusterEngine::get_delta_momentum(int short_seconds, int long_seconds) const {
    std::scoped_lock lock(mutex_);

    if (trades_.empty()) return 0.0;

    uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

    uint64_t cutoff_short = now - (short_seconds * 1000);
    uint64_t cutoff_long = now - (long_seconds * 1000);

    double delta_short = 0.0, delta_long = 0.0;
    double vol_short = 0.0, vol_long = 0.0;

    for (const auto& trade : trades_) {
        if (trade.timestamp < cutoff_long) continue;

        double signed_vol = trade.is_buyer_maker ? -trade.size : trade.size;

        delta_long += signed_vol;
        vol_long += trade.size;

        if (trade.timestamp >= cutoff_short) {
            delta_short += signed_vol;
            vol_short += trade.size;
        }
    }

    if (vol_short == 0 || vol_long == 0) return 0.0;

    // Нормализованная дельта (от -1 до +1)
    double norm_short = delta_short / vol_short;
    double norm_long = delta_long / vol_long;

    // Momentum = изменение нормализованной дельты
    return norm_short - norm_long;
}

double ClusterEngine::get_volume_imbalance(int seconds) const {
    std::scoped_lock lock(mutex_);

    if (trades_.empty()) return 0.0;

    uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    uint64_t cutoff = now - (seconds * 1000);

    double buy_vol = 0.0, sell_vol = 0.0;
    int buy_trades = 0, sell_trades = 0;

    for (const auto& trade : trades_) {
        if (trade.timestamp < cutoff) continue;

        if (trade.is_buyer_maker) {
            sell_vol += trade.size;
            sell_trades++;
        } else {
            buy_vol += trade.size;
            buy_trades++;
        }
    }

    if (buy_vol + sell_vol == 0) return 0.0;

    // Объёмный дисбаланс (от -1 до +1)
    double vol_imbalance = (buy_vol - sell_vol) / (buy_vol + sell_vol);

    // Дисбаланс количества трейдов
    double trade_imbalance = 0.0;
    if (buy_trades + sell_trades > 0) {
        trade_imbalance = (double)(buy_trades - sell_trades) / (buy_trades + sell_trades);
    }

    // Комбинируем (объём важнее)
    return vol_imbalance * 0.7 + trade_imbalance * 0.3;
}

double ClusterEngine::get_total_volume(int seconds) const {
    std::scoped_lock lock(mutex_);

    if (trades_.empty()) return 0.0;

    uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    uint64_t cutoff = now - (seconds * 1000);

    double total_volume = 0.0;

    for (const auto& trade : trades_) {
        if (trade.timestamp < cutoff) continue;
        total_volume += trade.size;
    }

    return total_volume;
}

void ClusterEngine::add_historical_trade(uint64_t timestamp, double price, double volume, bool is_buyer_maker) {
    std::lock_guard lock(mutex_);
    Trade t;
    t.timestamp = static_cast<double>(timestamp); // если поле timestamp double, иначе привести
    t.price = price;
    t.size = volume;
    t.is_buyer_maker = is_buyer_maker;
    trades_.push_back(t);
    if (trades_.size() > MAX_TRADES) {
        trades_.erase(trades_.begin());
    }
}

bool ClusterEngine::is_exhaustion_detected() const {
    // Exhaustion = дельта меняет направление при продолжении тренда
    double momentum = get_delta_momentum(10, 60);
    double imbalance = get_volume_imbalance(30);

    // Если momentum против imbalance — exhaustion
    // Например: imbalance > 0 (больше покупок), но momentum < 0 (покупки слабеют)
    return (momentum * imbalance) < -0.1;
}

// ============================================================
// VPIN: Volume-Synchronized Probability of Informed Trading
//
// Разбиваем последние N трейдов на бакеты по объёму (не по времени).
// В каждом бакете считаем |buy_vol - sell_vol| / total_vol.
// VPIN = среднее по бакетам.
//
// > 0.7 = информированный поток, MM должен паузить.
// < 0.3 = шум, MM может котировать агрессивно.
// ============================================================
double ClusterEngine::get_vpin(int num_buckets) const {
    std::scoped_lock lock(mutex_);

    if (trades_.size() < 100) return 0.0;

    // Считаем средний объём трейда, чтобы сформировать размер бакета
    double total_vol = 0.0;
    for (const auto& t : trades_) total_vol += t.size;
    double avg_trade_vol = total_vol / trades_.size();
    if (avg_trade_vol <= 0.0) return 0.0;

    // Размер бакета = ~50 средних трейдов
    double bucket_vol = avg_trade_vol * 50.0;

    // Идём с конца (свежие) — собираем num_buckets бакетов
    std::vector<double> imbalances;
    imbalances.reserve(num_buckets);

    double cur_buy = 0.0, cur_sell = 0.0, cur_total = 0.0;

    for (auto it = trades_.rbegin(); it != trades_.rend(); ++it) {
        const auto& t = *it;
        cur_total += t.size;
        if (t.is_buyer_maker) cur_sell += t.size;
        else                  cur_buy  += t.size;

        if (cur_total >= bucket_vol) {
            double imbalance = std::abs(cur_buy - cur_sell) / cur_total;
            imbalances.push_back(imbalance);
            if ((int)imbalances.size() >= num_buckets) break;
            cur_buy = cur_sell = cur_total = 0.0;
        }
    }

    if (imbalances.empty()) return 0.0;

    double sum = 0.0;
    for (double x : imbalances) sum += x;
    return sum / imbalances.size();
}

double ClusterEngine::get_volume_usd(int seconds, double mid) const {
    return get_total_volume(seconds) * mid;
}