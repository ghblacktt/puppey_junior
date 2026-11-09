#include "mean_reversion_strategy.h"
#include <cmath>
#include <iostream>
#include <iomanip>
#include <algorithm>

MeanReversionStrategy::MeanReversionStrategy(
        OrderBook& orderbook,
        ClusterEngine& cluster,
        TimeWindowVWMA& vwma,
        DynamicBBEngine& bb
) : orderbook_(orderbook), cluster_(cluster), vwma_(vwma), bb_(bb) {
    bar_aggregator_ = std::make_unique<BarAggregator>(60000, [this](const Bar& bar) {
        this->on_bar_closed(bar);
    });

    regime_filter_ = std::make_unique<RegimeFilter>(14, 20.0, 24.0, 120);
}

void MeanReversionStrategy::update(double price, double volume, uint64_t timestamp) {
    last_price_ = price;

    if (bar_aggregator_) {
        bar_aggregator_->add_tick(price, volume, timestamp);
    }
    if (regime_filter_ && vwma_.is_ready()) {
        regime_filter_->update_tick(price, vwma_.get_vwma());
    }
    generate_signal();
}

void MeanReversionStrategy::on_bar_closed(const Bar& bar) {
    if (regime_filter_) {
        regime_filter_->on_bar_closed(bar);

        double vwma_val = vwma_.get_vwma();
        regime_filter_->calculate_z_score(bar.close, vwma_val);
    }
}

void MeanReversionStrategy::generate_signal() {
    current_signal_ = NONE;
    details_ = SignalDetails();

    double current_price = last_price_;
    if (current_price <= 0) return;

    // 1. ФИЛЬТР РЕЖИМА РЫНКА (ADX)
    if (!regime_filter_->is_mr_mode_allowed()) {
        return;
    }

    auto now = std::chrono::system_clock::now();
    uint64_t current_timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();

    // Кулдаун между сигналами (2 минуты)
    if (current_timestamp - last_signal_timestamp_ < SIGNAL_COOLDOWN_MS) {
        return;
    }

    // Фильтр тренда: пропускаем сигналы при сильном наклоне VWMA
    double vwma_slope = std::abs(vwma_.get_vwma_slope());
    if (vwma_slope > 0.002) {
        return;
    }

    double z = regime_filter_->get_bar_z_score();
    double bar_vwma = regime_filter_->get_bar_vwma();
    double z_trigger = 2.0;
    bool delta_weak = true;

    bool long_valid = (z < -z_trigger) && (current_price < bar_vwma);
    bool short_valid = (z > z_trigger) && (current_price > bar_vwma);

    // DI-фильтр: обязательный, но сильные сигналы (|z|>2) пропускаются
    bool di_ok_long = (z < -2.0) || (regime_filter_->get_plus_di() >= regime_filter_->get_minus_di());
    bool di_ok_short = (z > 2.0) || (regime_filter_->get_minus_di() >= regime_filter_->get_plus_di());

    bool absorption_present = false; // временно отключено

    bool long_conditions = long_valid && di_ok_long && delta_weak;
    bool short_conditions = short_valid && di_ok_short && delta_weak;

    // Отладочный вывод раз в 5 секунд
    static uint64_t last_debug = 0;
    uint64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

    if (now_ms - last_debug >= 5000) {
        last_debug = now_ms;

        std::cout << "[MR_DIAG] " << last_price_
                  << " z=" << z
                  << " vwma=" << bar_vwma
                  << " vwma_slope=" << vwma_.get_vwma_slope()
                  << " delta_weak=" << delta_weak
                  << " di_ok_long=" << di_ok_long
                  << " di_ok_short=" << di_ok_short
                  << " adx=" << regime_filter_->get_adx()
                  << " mr_mode=" << regime_filter_->is_mr_mode_allowed()
                  << " long_cond=" << long_conditions
                  << " short_cond=" << short_conditions
                  << " bb_ready=" << bb_.is_ready()
                  << " cooldown_remain=" << (current_timestamp - last_signal_timestamp_ < SIGNAL_COOLDOWN_MS ? (SIGNAL_COOLDOWN_MS - (current_timestamp - last_signal_timestamp_)) / 1000 : 0)
                  << "s\n";
    }

    bool signal_attempted = false;

    if (long_conditions && bb_.is_ready()) {
        generate_bounce_signal(current_price, bar_vwma, bb_.get_bands(), true, absorption_present);
        signal_attempted = true;
    } else if (short_conditions && bb_.is_ready()) {
        generate_bounce_signal(current_price, bar_vwma, bb_.get_bands(), false, absorption_present);
        signal_attempted = true;
    }

    if (signal_attempted && current_signal_ != NONE) {
        last_signal_timestamp_ = current_timestamp;
    }
}

void MeanReversionStrategy::generate_bounce_signal(
        double current_price, double vwma, const DynamicBBEngine::Bands& bands,
        bool is_long, bool absorption_present
) {
    if (bands.alma <= 0 || bands.std_dev <= 0) {
        current_signal_ = NONE;
        return;
    }

    // === АНАЛИЗ СТАКАНА (только ликвидность) ===
    auto book_analysis = orderbook_.analyze_book();
    double liquidity_quality = book_analysis.liquidity_quality;

    // === ДИНАМИЧЕСКИЙ TP ===
    double tp_buffer_pct = 0.001;
    double tp1_price_adjusted;

    if (is_long) {
        tp1_price_adjusted = vwma * (1.0 - tp_buffer_pct);
        double max_tp_distance = bands.std_dev * 3.0;   // до 3 std dev
        if (tp1_price_adjusted - current_price > max_tp_distance) {
            tp1_price_adjusted = current_price + max_tp_distance;
        }
    } else {
        tp1_price_adjusted = vwma * (1.0 + tp_buffer_pct);
        double max_tp_distance = bands.std_dev * 3.0;
        if (current_price - tp1_price_adjusted > max_tp_distance) {
            tp1_price_adjusted = current_price - max_tp_distance;
        }
    }

    // === SL на основе волатильности ===
    double sl_distance = bands.std_dev * 2.0;   // 2 стандартных отклонения
    sl_distance = std::clamp(sl_distance, current_price * 0.0015, current_price * 0.01); // мин 0.15%, макс 1%

    // Устанавливаем TP и SL
    if (is_long) {
        current_signal_ = LONG;
        details_.signal = LONG;
        details_.take_profit_1 = tp1_price_adjusted;
        details_.stop_loss = current_price - sl_distance;

        // Промежуточная цель для частичной фиксации (50% пути)
        details_.take_profit_2 = current_price + (tp1_price_adjusted - current_price) * 0.5;

        if (details_.take_profit_1 <= current_price || details_.stop_loss >= current_price) {
            current_signal_ = NONE;
            return;
        }
    } else {
        current_signal_ = SHORT;
        details_.signal = SHORT;
        details_.take_profit_1 = tp1_price_adjusted;
        details_.stop_loss = current_price + sl_distance;

        details_.take_profit_2 = current_price - (current_price - tp1_price_adjusted) * 0.5;

        if (details_.take_profit_1 >= current_price || details_.stop_loss <= current_price) {
            current_signal_ = NONE;
            return;
        }
    }

    details_.entry_price = current_price;
    details_.confidence = calculate_confidence();

    // Бонусы/штрафы за ликвидность
    if (absorption_present) {
        details_.confidence = std::min(details_.confidence + 0.10, 1.0);
    }
    if (liquidity_quality > 0.7) {
        details_.confidence = std::min(details_.confidence + 0.05, 1.0);
    } else if (liquidity_quality < 0.4) {
        details_.confidence = std::max(details_.confidence - 0.10, 0.0);
    }

    double expected_pnl_pct = std::abs(tp1_price_adjusted - current_price) / current_price;
    double sl_pct = sl_distance / current_price;
    double rr_ratio = expected_pnl_pct / sl_pct;

    double min_rr = 1.3;   // слегка снижен для увеличения частоты сигналов
    if (rr_ratio < min_rr) {
        current_signal_ = NONE;
        return;
    }

    if (details_.confidence < 0.35) {
        current_signal_ = NONE;
        return;
    }

    if (expected_pnl_pct < 0.0015) {
        current_signal_ = NONE;
        return;
    }

    std::cout << "[SUCCESS] " << (is_long ? "LONG" : "SHORT")
              << " | TP: " << std::fixed << std::setprecision(3) << (expected_pnl_pct * 100) << "%"
              << " | SL: " << (sl_pct * 100) << "%"
              << " | R:R: " << std::setprecision(2) << rr_ratio
              << " | Conf: " << std::setprecision(3) << details_.confidence
              << " | Imbalance: " << std::setprecision(2) << book_analysis.imbalance
              << " | Wall: " << (book_analysis.has_bid_wall || book_analysis.has_ask_wall)
              << "\n" << std::flush;
}

auto MeanReversionStrategy::is_delta_weakening() const -> bool {
    double delta_60s = cluster_.get_delta(60);
    double delta_10s = cluster_.get_delta(10);
    double current_price = last_price_;
    double vwma_slope = vwma_.get_vwma_slope();
    double price_dev = vwma_.get_price_deviation(current_price);

    if (std::abs(delta_60s) < 0.5) return false;
    if (std::abs(vwma_slope) > 0.003) return false;   // оставляем, согласовано с фильтром выше
    if (std::abs(price_dev) > 0.02) return false;

    double weakening_ratio = std::abs(delta_10s) / std::max(std::abs(delta_60s), 0.001);
    return weakening_ratio < 0.85;
}

auto MeanReversionStrategy::is_absorption_zone_present() const -> bool {
    return false; // временно отключено
}

auto MeanReversionStrategy::calculate_confidence() const -> double {
    double confidence = 0.0;
    double current_price = last_price_;
    auto bands = bb_.get_bands();

    // 1. Overshoot
    if (bands.std_dev > 0) {
        double overshoot = std::abs(current_price - bands.alma) / bands.std_dev;
        confidence += std::min(overshoot * 0.15, 0.15);
    }

    // 2. Volatility (низкая волатильность лучше для mean reversion)
    if (bands.volatility > 0 && bands.volatility < 0.003) {
        confidence += 0.15 * (1.0 - bands.volatility / 0.003);
    }

    // 3. Delta momentum
    double delta_60s = cluster_.get_delta(60);
    double delta_10s = cluster_.get_delta(10);
    if (std::abs(delta_60s) > 10.0) {
        double ratio = std::abs(delta_10s) / std::abs(delta_60s);
        if (ratio < 0.7) {
            confidence += 0.15 * (1.0 - ratio);
        }
    }

    // 4. VWMA slope (плоский лучше)
    double vwma_slope = std::abs(vwma_.get_vwma_slope());
    if (vwma_slope < 0.00005) {
        confidence += 0.15 * (1.0 - vwma_slope / 0.00005);
    }

    // 5. Price deviation
    double price_dev = std::abs(vwma_.get_price_deviation(current_price));
    if (price_dev < 0.002) {
        confidence += 0.15 * (1.0 - price_dev / 0.002);
    }

    // 6. Z-Score strength
    double z = std::abs(regime_filter_->get_bar_z_score());
    if (z > 1.5) {
        confidence += std::min((z - 1.5) * 0.1, 0.1);
    }

    return std::min(confidence, 1.0);
}

auto MeanReversionStrategy::is_price_at_extreme() const -> bool {
    double current_price = last_price_;
    if (!bb_.is_ready()) return false;

    auto bands = bb_.get_bands();
    if (bands.alma <= 0 || bands.std_dev <= 0) return false;

    if (current_price > bands.band1_upper) return true;
    if (current_price < bands.band1_lower) return true;

    double threshold = bands.std_dev * 0.1;
    double distance_to_upper = bands.band1_upper - current_price;
    if (distance_to_upper >= 0 && distance_to_upper < threshold) return true;
    double distance_to_lower = current_price - bands.band1_lower;
    if (distance_to_lower >= 0 && distance_to_lower < threshold) return true;

    return false;
}

auto MeanReversionStrategy::get_signal() const -> MeanReversionStrategy::Signal {
    return current_signal_;
}

auto MeanReversionStrategy::get_signal_details() const -> MeanReversionStrategy::SignalDetails {
    return details_;
}