#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>

struct MmSymbolConfig {
    std::string symbol;
    bool        enabled         = true;
    double      min_hs_bps      = 0.0;    // 0 = дефолт по цене
    double      qty_usd         = 0.0;    // 0 = дефолт по шагу
    double      base_spread_bps = 0.0;    // 0 = min_hs * 2
    double      max_position_usd = 0.0;   // 0 = глобальный дефолт
    double      momentum_weight  = 0.0;
    int max_hold_sec = 0;
    std::string note;
};

// NEW: глобальный конфиг (все поля с дефолтами в коде)
struct MmGlobalConfig {
    double maker_fee_bps          = 2.0;
    double max_half_spread_bps    = 50.0;
    double kill_adverse_bps       = 8.0;
    double half_spread_upper_pct  = 0.05;
    double hard_cap_ratio         = 0.85;
    double block_ratio            = 0.10;
    double gamma                  = 10.0;
    double vpin_pause_threshold   = 0.85;
    double vpin_resume_threshold  = 0.60;
    double soft_exit_pnl_pct      = 0.01;
    double soft_exit_ratio        = 0.15;
    double min_qty_floor_usd      = 4.0;
    double max_qty_multiplier     = 1.8;
    double max_position_default_usd = 50.0;
};

class MmSymbolRegistry {
public:
    static std::shared_ptr<MmSymbolRegistry> create(const std::string& conn_str);

    MmSymbolConfig get(const std::string& symbol) const;
    bool           is_enabled(const std::string& symbol) const;
    std::vector<std::string> enabled_symbols() const;

    // NEW: глобальный конфиг из БД (или дефолты, если строки нет)
    MmGlobalConfig global() const;

    void reload();

    // NEW: записать snapshot конфига перед запуском
    void snapshot_config(const std::string& comment);

    void save_stats(const std::string& symbol,
                    int fills, int cancels, int rejects,
                    int avg_age_ms,
                    double half_bps, double adverse_bps,
                    double gross_edge_bps, double net_edge_bps,
                    double fee_usd,
                    const std::string& verdict);

private:
    explicit MmSymbolRegistry(const std::string& conn_str);

    std::string conn_str_;
    mutable std::mutex mtx_;
    std::unordered_map<std::string, MmSymbolConfig> cache_;
    MmGlobalConfig global_;
};