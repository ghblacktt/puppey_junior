#include "regime_filter.h"
#include "iostream"

RegimeFilter::RegimeFilter(int adx_len, double adx_on, double adx_off, int z_period, int bar_vwma_len)
        : adx_len_(adx_len), adx_on_(adx_on), adx_off_(adx_off), z_period_(z_period), bar_vwma_len_(bar_vwma_len) {
    if (adx_len_ < 2) adx_len_ = 2;
}

void RegimeFilter::initialize_adx() {
    double sum_tr = 0, sum_plus_dm = 0, sum_minus_dm = 0;
    std::vector<double> dx_values;

    double p_high = warmup_bars_[0].high;
    double p_low = warmup_bars_[0].low;
    double p_close = warmup_bars_[0].close;

    // Считаем SMA для первых adx_len баров
    for (size_t i = 1; i < warmup_bars_.size(); ++i) {
        const Bar& bar = warmup_bars_[i];

        double up_move = bar.high - p_high;
        double down_move = p_low - bar.low;

        double plus_dm = (up_move > down_move && up_move > 0) ? up_move : 0.0;
        double minus_dm = (down_move > up_move && down_move > 0) ? down_move : 0.0;

        double tr = std::max({bar.high - bar.low,
                              std::abs(bar.high - p_close),
                              std::abs(bar.low - p_close)});

        sum_tr += tr;
        sum_plus_dm += plus_dm;
        sum_minus_dm += minus_dm;

        if (sum_tr > 0) {
            double pDI = 100.0 * sum_plus_dm / sum_tr;
            double mDI = 100.0 * sum_minus_dm / sum_tr;
            if (pDI + mDI > 0) {
                dx_values.push_back(100.0 * std::abs(pDI - mDI) / (pDI + mDI));
            } else {
                dx_values.push_back(0.0);
            }
        }

        p_high = bar.high;
        p_low = bar.low;
        p_close = bar.close;
    }

    // Начальные значения - SMA
    atr_ = sum_tr / adx_len_;
    plus_dm_smoothed_ = sum_plus_dm / adx_len_;
    minus_dm_smoothed_ = sum_minus_dm / adx_len_;

    if (!dx_values.empty()) {
        double sum_dx = 0;
        for (double dx : dx_values) sum_dx += dx;
        adx_ = sum_dx / dx_values.size();
    }
}

void RegimeFilter::process_bar(const Bar& bar) {
    double up_move = bar.high - prev_high_;
    double down_move = prev_low_ - bar.low;

    double plus_dm = (up_move > down_move && up_move > 0) ? up_move : 0.0;
    double minus_dm = (down_move > up_move && down_move > 0) ? down_move : 0.0;

    double tr = std::max({bar.high - bar.low,
                          std::abs(bar.high - prev_close_),
                          std::abs(bar.low - prev_close_)});

    // Wilder's smoothing (RMA): alpha = 1/length
    double alpha = 1.0 / adx_len_;

    atr_ = atr_ * (1.0 - alpha) + tr * alpha;
    plus_dm_smoothed_ = plus_dm_smoothed_ * (1.0 - alpha) + plus_dm * alpha;
    minus_dm_smoothed_ = minus_dm_smoothed_ * (1.0 - alpha) + minus_dm * alpha;

    if (atr_ > 0) {
        plus_di_ = 100.0 * plus_dm_smoothed_ / atr_;
        minus_di_ = 100.0 * minus_dm_smoothed_ / atr_;
    } else {
        plus_di_ = 0.0;
        minus_di_ = 0.0;
    }

    double dx = 0.0;
    if (plus_di_ + minus_di_ > 0) {
        dx = 100.0 * std::abs(plus_di_ - minus_di_) / (plus_di_ + minus_di_);
    }

    // Сглаживаем ADX
    adx_ = adx_ * (1.0 - alpha) + dx * alpha;
}

void RegimeFilter::on_bar_closed(const Bar& bar) {
    // 1. Сохраняем бар в историю для расчета Барового Z-Score
    bar_history_.push_back(bar);
    if (bar_history_.size() > static_cast<size_t>(bar_vwma_len_ + 10)) {
        bar_history_.pop_front();
    }

    // 2. Считаем Баровый Z-Score
    calculate_bar_z_score();

    // 3. Логика ADX (как и раньше)
    if (!is_initialized_) {
        warmup_bars_.push_back(bar);
        if (warmup_bars_.size() >= static_cast<size_t>(adx_len_)) {
            initialize_adx();
            is_initialized_ = true;
        }
        return;
    }

    process_bar(bar);
    update_hysteresis();

    prev_high_ = bar.high;
    prev_low_ = bar.low;
    prev_close_ = bar.close;
}

void RegimeFilter::calculate_bar_z_score() {
    if (bar_history_.size() < static_cast<size_t>(bar_vwma_len_)) {
        bar_z_score_ = 0.0;
        return;
    }

    size_t start_idx = bar_history_.size() - bar_vwma_len_;
    double sum_pv = 0.0, sum_v = 0.0;

    // 1. Считаем VWMA для текущего окна
    for (size_t i = start_idx; i < bar_history_.size(); ++i) {
        sum_pv += bar_history_[i].close * bar_history_[i].volume;
        sum_v += bar_history_[i].volume;
    }
    if (sum_v <= 0) return;

    double current_vwma = sum_pv / sum_v;
    bar_vwma_ = current_vwma; // Обновляем для внешнего использования

    // 2. Считаем стандартное отклонение ЦЕНЫ от этой VWMA в рамках окна
    double sum_sq = 0.0;
    for (size_t i = start_idx; i < bar_history_.size(); ++i) {
        double diff = bar_history_[i].close - current_vwma; // Корректное сравнение
        sum_sq += diff * diff;
    }

    double sd = std::sqrt(sum_sq / bar_vwma_len_);
    double last_close = bar_history_.back().close;

    // 3. Z-Score последнего бара
    bar_z_score_ = (sd > 0.0) ? (last_close - current_vwma) / sd : 0.0;
}

void RegimeFilter::update_tick(double price, double vwma) {
    calculate_z_score(price, vwma);
}

void RegimeFilter::update_hysteresis() {
    // Логика гистерезиса из Pine Script
    if (mr_mode_) {
        if (adx_ > adx_off_) mr_mode_ = false; // Тренд усилился, выходим из MR режима
    } else {
        if (adx_ < adx_on_) mr_mode_ = true;   // Рынок затих, можно торговать возврат
    }
}

void RegimeFilter::calculate_z_score(double close_price, double vwma_value) {
    double residual = close_price - vwma_value;
    residuals_.push_back(residual);
    if (residuals_.size() > static_cast<size_t>(z_period_)) {
        residuals_.pop_front();
    }

    if (residuals_.size() < 20) {
        z_score_ = 0.0;
        return;
    }

    double mean = 0;
    for (double r : residuals_) mean += r;
    mean /= residuals_.size();

    double sq_sum = 0;
    for (double r : residuals_) sq_sum += (r - mean) * (r - mean);
    double sd = std::sqrt(sq_sum / residuals_.size());

    z_score_ = (sd > 0) ? (residual / sd) : 0.0;
}