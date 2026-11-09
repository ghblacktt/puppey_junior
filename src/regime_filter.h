// regime_filter.h
#pragma once
#include "bar_aggregator.h"
#include <deque>
#include <vector>
#include <cmath>
#include <algorithm>

class RegimeFilter {
public:
    RegimeFilter(int adx_len = 14, double adx_on = 20.0, double adx_off = 30.0,
                 int z_period = 120, int bar_vwma_len = 30); // bar_vwma_len = 30 баров

    void on_bar_closed(const Bar& bar);
    void update_tick(double price, double vwma); // Оставляем для тикового (можно удалить позже)
    void calculate_z_score(double close_price, double vwma_value);

    bool is_ready() const { return is_initialized_; }
    bool is_mr_mode_allowed() const { return mr_mode_; }
    double get_atr() const { return atr_; }

    double get_adx() const { return adx_; }
    double get_plus_di() const { return plus_di_; }
    double get_minus_di() const { return minus_di_; }

    // НОВОЕ: Баровый Z-Score (именно его будем использовать для входов)
    double get_bar_z_score() const { return bar_z_score_; }
    double get_bar_vwma() const { return bar_vwma_; }
    bool is_mr_mode() const { return mr_mode_; }
    bool is_initialized() const { return is_initialized_; }
private:
    int adx_len_;
    double adx_on_;
    double adx_off_;
    int z_period_;
    int bar_vwma_len_; // Длина VWMA в барах

    // ... (все старые поля для ADX и тикового Z-Score) ...
    bool is_initialized_ = false;
    std::vector<Bar> warmup_bars_;
    double prev_high_ = 0.0, prev_low_ = 0.0, prev_close_ = 0.0;
    double atr_ = 0.0, plus_dm_smoothed_ = 0.0, minus_dm_smoothed_ = 0.0, adx_ = 0.0;
    double plus_di_ = 0.0, minus_di_ = 0.0;
    bool mr_mode_ = true;
    std::deque<double> residuals_;
    double z_score_ = 0.0;

    // НОВОЕ: Поля для Барового Z-Score
    std::deque<Bar> bar_history_; // Храним историю закрытых баров
    double bar_vwma_ = 0.0;
    double bar_z_score_ = 0.0;

    void initialize_adx();
    void process_bar(const Bar& bar);
    void update_hysteresis();

    // НОВОЕ: Расчет VWMA и Z-Score по барам
    void calculate_bar_z_score();
};