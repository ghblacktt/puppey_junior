#include "order_book.h"
#include "bybit/bybit_api_client.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <chrono>

// ============================================================
// update — приём snapshot/delta от Bybit V5 orderbook.50
// ============================================================
void OrderBook::update(const simdjson::dom::element& data, std::string_view type) {
    std::lock_guard<std::mutex> lock(mutex_);

    std::string_view effective_type = type;
    if (effective_type.empty()) effective_type = "delta";

    // Bybit V5 linear orderbook.50 — поле "u" (per-symbol update id).
    // "seq" — legacy для spot и, как правило, cross-symbol; не используем
    // как fallback, иначе можно отклонить валидные дельты чужого символа.
    uint64_t seq = 0;
    bool has_seq = (data["u"].get_uint64().get(seq) == simdjson::SUCCESS);

    if (has_seq && effective_type == "delta" && seq <= last_seq_) {
        return;   // устаревшая дельта — молча игнорируем
    }

    // last_update_ms_ ставим только когда реально применяем данные,
    // иначе watchdog "книга жива" срабатывает на повторных дельтах.
    last_update_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

    if (has_seq) last_seq_ = seq;

    simdjson::dom::array bids_arr, asks_arr;
    bool has_bids = (data["b"].get_array().get(bids_arr) == simdjson::SUCCESS);
    bool has_asks = (data["a"].get_array().get(asks_arr) == simdjson::SUCCESS);

    if (effective_type == "snapshot") {
        bids_.clear();
        asks_.clear();
    }

    if (has_bids) {
        for (auto bid_entry : bids_arr) {
            simdjson::dom::array level;
            if (bid_entry.get_array().get(level) != simdjson::SUCCESS) continue;

            auto it = level.begin();
            if (it == level.end()) continue;
            simdjson::dom::element price_elem = *it;
            ++it;
            if (it == level.end()) continue;
            simdjson::dom::element size_elem = *it;

            std::string_view price_str, size_str;
            if (price_elem.get_string().get(price_str) != simdjson::SUCCESS) continue;
            if (size_elem.get_string().get(size_str)   != simdjson::SUCCESS) continue;

            double price = std::strtod(price_str.data(), nullptr);
            double size  = std::strtod(size_str.data(),  nullptr);

            if (size == 0.0) bids_.erase(price);
            else             bids_[price] = size;
        }
    }

    if (has_asks) {
        for (auto ask_entry : asks_arr) {
            simdjson::dom::array level;
            if (ask_entry.get_array().get(level) != simdjson::SUCCESS) continue;

            auto it = level.begin();
            if (it == level.end()) continue;
            simdjson::dom::element price_elem = *it;
            ++it;
            if (it == level.end()) continue;
            simdjson::dom::element size_elem = *it;

            std::string_view price_str, size_str;
            if (price_elem.get_string().get(price_str) != simdjson::SUCCESS) continue;
            if (size_elem.get_string().get(size_str)   != simdjson::SUCCESS) continue;

            double price = std::strtod(price_str.data(), nullptr);
            double size  = std::strtod(size_str.data(),  nullptr);

            if (size == 0.0) asks_.erase(price);
            else             asks_[price] = size;
        }
    }

    // ── OFI tracking ──
    if (!bids_.empty() && !asks_.empty()) {
        BestQuote cur;
        cur.bid      = bids_.begin()->first;
        cur.bid_size = bids_.begin()->second;
        cur.ask      = asks_.begin()->first;
        cur.ask_size = asks_.begin()->second;

        if (prev_quote_.bid > 0.0 && prev_quote_.ask > 0.0) {
            double ofi = compute_ofi_step(prev_quote_, cur);
            ofi_history_.push_back(ofi);
            if (ofi_history_.size() > 500) ofi_history_.pop_front();
        }
        prev_quote_ = cur;
    }

}

// ============================================================
// BBO getters
// ============================================================
double OrderBook::get_best_bid() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bids_.empty()) return 0.0;
    return bids_.begin()->first;
}

double OrderBook::get_best_ask() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (asks_.empty()) return 0.0;
    return asks_.begin()->first;
}

double OrderBook::get_mid_price() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bids_.empty() || asks_.empty()) return 0.0;
    return (bids_.begin()->first + asks_.begin()->first) / 2.0;
}

double OrderBook::get_best_bid_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bids_.empty()) return 0.0;
    return bids_.begin()->second;
}

double OrderBook::get_best_ask_size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (asks_.empty()) return 0.0;
    return asks_.begin()->second;
}

OrderBook::BBO OrderBook::get_bbo() const {
    std::lock_guard<std::mutex> lock(mutex_);
    BBO b;
    if (!bids_.empty()) {
        b.bid = bids_.begin()->first;
        b.bid_size = bids_.begin()->second;
    }
    if (!asks_.empty()) {
        b.ask = asks_.begin()->first;
        b.ask_size = asks_.begin()->second;
    }
    return b;
}

uint64_t OrderBook::get_last_update_ms() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_update_ms_;
}

// ============================================================
// Debug print
// ============================================================
void OrderBook::print_top_levels() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::cout << "\n=== ORDER BOOK TOP 5 ===\n";
    std::cout << std::fixed << std::setprecision(2);

    std::cout << "ASKS:\n";
    int count = 0;
    for (auto it = asks_.begin(); it != asks_.end() && count < 5; ++it, ++count) {
        std::cout << "  " << it->first << " : " << it->second << "\n";
    }

    double mid = 0.0;
    if (!bids_.empty() && !asks_.empty()) {
        mid = (bids_.begin()->first + asks_.begin()->first) / 2.0;
    }
    std::cout << "  --- MID: " << mid << " ---\n";

    std::cout << "BIDS:\n";
    count = 0;
    for (auto it = bids_.begin(); it != bids_.end() && count < 5; ++it, ++count) {
        std::cout << "  " << it->first << " : " << it->second << "\n";
    }
    std::cout << "========================\n\n";
    std::cout << std::flush;
}

// ============================================================
// Imbalance / weighted mid
// ============================================================
double OrderBook::get_imbalance_at_levels(int levels) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (levels <= 0) return 0.0;
    if (bids_.size() < (size_t)levels || asks_.size() < (size_t)levels) return 0.0;

    double bid_volume = 0.0, ask_volume = 0.0;

    auto bid_it = bids_.begin();
    for (int i = 0; i < levels && bid_it != bids_.end(); ++i, ++bid_it) {
        bid_volume += bid_it->second;
    }
    auto ask_it = asks_.begin();
    for (int i = 0; i < levels && ask_it != asks_.end(); ++i, ++ask_it) {
        ask_volume += ask_it->second;
    }

    if (bid_volume + ask_volume <= 0.0) return 0.0;
    return (bid_volume - ask_volume) / (bid_volume + ask_volume);
}

double OrderBook::get_bid_ask_imbalance(int levels) const {
    // Явный алиас — раньше был полный дубликат реализации.
    return get_imbalance_at_levels(levels);
}

double OrderBook::get_weighted_mid_price() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (bids_.empty() || asks_.empty()) return 0.0;

    double best_bid  = bids_.begin()->first;
    double best_ask  = asks_.begin()->first;
    double bid_size  = bids_.begin()->second;
    double ask_size  = asks_.begin()->second;

    if (bid_size + ask_size <= 0.0) return (best_bid + best_ask) / 2.0;
    return (best_bid * ask_size + best_ask * bid_size) / (bid_size + ask_size);
}

// ============================================================
// Slope — исправлен prev_size (раньше был 0, ломал первый дифф)
// ============================================================
double OrderBook::get_book_slope(int levels) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (levels <= 0) return 0.0;
    if (bids_.size() < (size_t)levels || asks_.size() < (size_t)levels) return 0.0;

    double bid_slope = 0.0, ask_slope = 0.0;

    // BID: цена убывает по мере удаления от mid
    {
        auto it = bids_.begin();
        double prev_price = it->first;
        double prev_size  = it->second;   // ← правильная инициализация
        ++it;
        for (int i = 1; i < levels && it != bids_.end(); ++i, ++it) {
            double price_diff = prev_price - it->first;
            double size_diff  = it->second  - prev_size;
            if (price_diff > 1e-12) bid_slope += size_diff / price_diff;
            prev_price = it->first;
            prev_size  = it->second;
        }
    }

    // ASK: цена возрастает по мере удаления от mid
    {
        auto it = asks_.begin();
        double prev_price = it->first;
        double prev_size  = it->second;
        ++it;
        for (int i = 1; i < levels && it != asks_.end(); ++i, ++it) {
            double price_diff = it->first  - prev_price;
            double size_diff  = it->second - prev_size;
            if (price_diff > 1e-12) ask_slope += size_diff / price_diff;
            prev_price = it->first;
            prev_size  = it->second;
        }
    }

    return (bid_slope + ask_slope) / 2.0;
}

// ============================================================
// initialize — сбрасываем last_seq_, чтобы после реконнекта
// первые дельты не отклонялись
// ============================================================
void OrderBook::initialize(const BybitApiClient::OrderBookSnapshot& snap) {
    std::lock_guard<std::mutex> lock(mutex_);
    bids_.clear();
    asks_.clear();
    last_seq_ = 0;
    prev_quote_ = {};
    ofi_history_.clear();

    for (const auto& [price, qty] : snap.bids) bids_[price] = qty;
    for (const auto& [price, qty] : snap.asks) asks_[price] = qty;

    last_update_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
}

// ============================================================
// analyze_book — корректные avg по фактическому числу уровней,
// осмысленные bid/ask_pressure
// ============================================================
BookAnalysis OrderBook::analyze_book() const {
    std::lock_guard<std::mutex> lock(mutex_);

    BookAnalysis analysis{};

    if (bids_.empty() || asks_.empty()) return analysis;

    const double best_bid  = bids_.begin()->first;
    const double best_ask  = asks_.begin()->first;
    const double mid       = (best_bid + best_ask) / 2.0;
    const double bid_size  = bids_.begin()->second;
    const double ask_size  = asks_.begin()->second;

    // === 1. Взвешенная середина ===
    if (bid_size + ask_size > 0.0) {
        analysis.weighted_mid = (best_bid * ask_size + best_ask * bid_size) / (bid_size + ask_size);
    } else {
        analysis.weighted_mid = mid;
    }

    // === 2. Imbalance top-N (по фактическому числу уровней) ===
    const int levels = 5;
    const int actual_bid_levels = std::min<int>(levels, (int)bids_.size());
    const int actual_ask_levels = std::min<int>(levels, (int)asks_.size());

    double bid_volume = 0.0, ask_volume = 0.0;
    {
        auto it = bids_.begin();
        for (int i = 0; i < actual_bid_levels && it != bids_.end(); ++i, ++it)
            bid_volume += it->second;
    }
    {
        auto it = asks_.begin();
        for (int i = 0; i < actual_ask_levels && it != asks_.end(); ++i, ++it)
            ask_volume += it->second;
    }

    if (bid_volume + ask_volume > 0.0) {
        analysis.imbalance = (bid_volume - ask_volume) / (bid_volume + ask_volume);
    }

    // Средний размер уровня — по фактически наблюдённым уровням
    const double avg_bid_size = (actual_bid_levels > 0) ? bid_volume / actual_bid_levels : 0.0;
    const double avg_ask_size = (actual_ask_levels > 0) ? ask_volume / actual_ask_levels : 0.0;

    // === 3. Walls ===
    const double wall_threshold = 5.0;   // 5x среднего размера уровня

    analysis.support_level     = 0.0;
    analysis.resistance_level  = 0.0;
    analysis.wall_distance_pct = 100.0;

    if (avg_bid_size > 1e-12) {
        for (const auto& [price, size] : bids_) {
            if (size > avg_bid_size * wall_threshold) {
                analysis.has_bid_wall   = true;
                analysis.support_level  = price;
                double distance = (mid - price) / mid * 100.0;
                analysis.wall_distance_pct = std::min(analysis.wall_distance_pct, distance);
                break;
            }
        }
    }
    if (avg_ask_size > 1e-12) {
        for (const auto& [price, size] : asks_) {
            if (size > avg_ask_size * wall_threshold) {
                analysis.has_ask_wall      = true;
                analysis.resistance_level  = price;
                double distance = (price - mid) / mid * 100.0;
                analysis.wall_distance_pct = std::min(analysis.wall_distance_pct, distance);
                break;
            }
        }
    }

    // === 4. Pressure: top-of-book size / avg level size ===
    // Раньше формула тождественно давала 1.0 (bid_volume / (bid_volume/5*5)),
    // теперь это реальная метрика "насколько лучший уровень больше среднего".
    analysis.bid_pressure = (avg_bid_size > 1e-12) ? (bid_size / avg_bid_size) : 0.0;
    analysis.ask_pressure = (avg_ask_size > 1e-12) ? (ask_size / avg_ask_size) : 0.0;

    // === 5. Качество ликвидности (min/max по top-N) ===
    const double min_liq = std::min(bid_volume, ask_volume);
    const double max_liq = std::max(bid_volume, ask_volume);
    analysis.liquidity_quality = (max_liq > 0.0) ? (min_liq / max_liq) : 0.0;

    // === 6. Флаги благоприятности ===
    analysis.favorable_for_long = (
            analysis.imbalance > 0.1 &&
            analysis.has_bid_wall
    );
    analysis.favorable_for_short = (
            analysis.imbalance < -0.1 &&
            analysis.has_ask_wall
    );

    return analysis;
}

// ============================================================
// Walls (упрощённый поиск по 10 уровням)
// ============================================================
std::pair<double, double> OrderBook::get_walls(double threshold_multiplier) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (bids_.empty() || asks_.empty()) return {0.0, 0.0};

    auto avg_of_top10 = [](const auto& map) {
        double total = 0.0;
        int count = 0;
        for (const auto& [p, s] : map) {
            total += s;
            ++count;
            if (count >= 10) break;
        }
        return (count > 0) ? total / count : 0.0;
    };

    const double avg_bid = avg_of_top10(bids_);
    const double avg_ask = avg_of_top10(asks_);

    double bid_wall = 0.0, ask_wall = 0.0;

    if (avg_bid > 1e-12) {
        for (const auto& [price, size] : bids_) {
            if (size > avg_bid * threshold_multiplier) { bid_wall = price; break; }
        }
    }
    if (avg_ask > 1e-12) {
        for (const auto& [price, size] : asks_) {
            if (size > avg_ask * threshold_multiplier) { ask_wall = price; break; }
        }
    }

    return {bid_wall, ask_wall};
}

// ============================================================
// Liquidity ratio
// ============================================================
double OrderBook::get_liquidity_ratio(double price_range_pct) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (bids_.empty() || asks_.empty()) return 0.0;

    const double mid   = (bids_.begin()->first + asks_.begin()->first) / 2.0;
    const double range = mid * price_range_pct;

    double bid_liquidity = 0.0, ask_liquidity = 0.0;

    for (const auto& [price, size] : bids_) {
        if (price >= mid - range) bid_liquidity += size;
    }
    for (const auto& [price, size] : asks_) {
        if (price <= mid + range) ask_liquidity += size;
    }

    const double total = bid_liquidity + ask_liquidity;
    if (total <= 0.0) return 0.0;
    return std::min(bid_liquidity, ask_liquidity) / (total / 2.0);
}

double OrderBook::get_microprice() const {
    // Уже реализован как get_weighted_mid_price, но с явным контрактом:
    // это microprice, а не WAP.
    return get_weighted_mid_price();
}

double OrderBook::compute_ofi_step(const BestQuote& prev,
                                   const BestQuote& cur) const {
    // Cont, Kukanov, Stoikov (2014) "The Price Impact of Order Book Events"
    // e_n = Δbid_vol_signed - Δask_vol_signed
    double e_bid = 0.0;
    if (cur.bid > prev.bid)        e_bid =  cur.bid_size;
    else if (cur.bid < prev.bid)   e_bid = -prev.bid_size;
    else                           e_bid =  cur.bid_size - prev.bid_size;

    double e_ask = 0.0;
    if (cur.ask < prev.ask)        e_ask = -cur.ask_size;
    else if (cur.ask > prev.ask)   e_ask =  prev.ask_size;
    else                           e_ask = -(cur.ask_size - prev.ask_size);

    return e_bid + e_ask;   // positive = buy pressure
}

double OrderBook::get_ofi_sum(int n_samples) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ofi_history_.empty() || n_samples <= 0) return 0.0;

    double sum = 0.0;
    int count = 0;
    for (auto it = ofi_history_.rbegin();
         it != ofi_history_.rend() && count < n_samples;
         ++it, ++count) {
        sum += *it;
    }
    return sum;
}