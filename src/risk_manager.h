// risk_manager.h
#pragma once
class RiskManager {
public:
    RiskManager(double max_daily_loss_pct, double max_position_pct);
    bool check_daily_loss(double current_equity, double initial_equity);
    bool check_position_size(double position, double equity);
private:
    double max_daily_loss_pct_;
    double max_position_pct_;
};
// risk_manager.cpp
#include "risk_manager.h"
RiskManager::RiskManager(double max_daily_loss_pct, double max_position_pct)
        : max_daily_loss_pct_(max_daily_loss_pct), max_position_pct_(max_position_pct) {}
bool RiskManager::check_daily_loss(double current_equity, double initial_equity) {
    if (current_equity <= 0) return false;
    double loss = (initial_equity - current_equity) / initial_equity;
    return loss < max_daily_loss_pct_;
}
bool RiskManager::check_position_size(double position, double equity) {
    return std::abs(position) <= max_position_pct_ * equity;
}