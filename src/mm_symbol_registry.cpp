#include "mm_symbol_registry.h"
#include <pqxx/pqxx>
#include <iostream>

std::shared_ptr<MmSymbolRegistry> MmSymbolRegistry::create(
        const std::string& conn_str) {
    auto r = std::shared_ptr<MmSymbolRegistry>(new MmSymbolRegistry(conn_str));
    r->reload();
    return r;
}

MmSymbolRegistry::MmSymbolRegistry(const std::string& conn_str)
        : conn_str_(conn_str) {}

void MmSymbolRegistry::reload() {
    std::unordered_map<std::string, MmSymbolConfig> new_cache;
    MmGlobalConfig new_global;

    try {
        pqxx::connection conn(conn_str_);
        pqxx::work txn(conn);

        // ── per-symbol ──
        auto res = txn.exec(
                "SELECT symbol, enabled, "
                "       COALESCE(min_hs_bps, 0), "
                "       COALESCE(qty_usd, 0), "
                "       COALESCE(base_spread_bps, 0), "
                "       COALESCE(max_position_usd, 0), "
                "       COALESCE(momentum_weight, 0), "
                "       COALESCE(max_hold_sec, 0), "
                "       COALESCE(note, '') "
                "FROM mm_symbols");
        for (auto row : res) {
            MmSymbolConfig c;
            c.symbol          = row["symbol"].as<std::string>();
            c.enabled         = row["enabled"].as<bool>();
            c.min_hs_bps      = row[2].as<double>();
            c.qty_usd         = row[3].as<double>();
            c.base_spread_bps = row[4].as<double>();
            c.max_position_usd= row[5].as<double>();
            c.momentum_weight = row[6].as<double>();
            c.max_hold_sec    = row[7].as<int>();
            c.note            = row[8].as<std::string>();
            new_cache[c.symbol] = c;
        }

        // ── global ──
        auto gres = txn.exec(
                "SELECT maker_fee_bps, max_half_spread_bps, kill_adverse_bps, "
                "       half_spread_upper_pct, hard_cap_ratio, block_ratio, "
                "       gamma, vpin_pause_threshold, vpin_resume_threshold, "
                "       soft_exit_pnl_pct, soft_exit_ratio, "
                "       min_qty_floor_usd, max_qty_multiplier, "
                "       max_position_default_usd "
                "FROM mm_global_config WHERE id = 1");
        if (!gres.empty()) {
            auto r = gres[0];
            new_global.maker_fee_bps          = r[0].as<double>();
            new_global.max_half_spread_bps    = r[1].as<double>();
            new_global.kill_adverse_bps       = r[2].as<double>();
            new_global.half_spread_upper_pct  = r[3].as<double>();
            new_global.hard_cap_ratio         = r[4].as<double>();
            new_global.block_ratio            = r[5].as<double>();
            new_global.gamma                  = r[6].as<double>();
            new_global.vpin_pause_threshold   = r[7].as<double>();
            new_global.vpin_resume_threshold  = r[8].as<double>();
            new_global.soft_exit_pnl_pct      = r[9].as<double>();
            new_global.soft_exit_ratio        = r[10].as<double>();
            new_global.min_qty_floor_usd      = r[11].as<double>();
            new_global.max_qty_multiplier     = r[12].as<double>();
            new_global.max_position_default_usd = r[13].as<double>();
        }

        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[Registry] load failed: " << e.what() << "\n";
    }

    std::lock_guard lk(mtx_);
    cache_ = std::move(new_cache);
    global_ = new_global;
    std::cout << "[Registry] loaded " << cache_.size()
              << " symbols + global config\n";
}

MmGlobalConfig MmSymbolRegistry::global() const {
    std::lock_guard lk(mtx_);
    return global_;
}

void MmSymbolRegistry::snapshot_config(const std::string& comment) {
    try {
        pqxx::connection conn(conn_str_);
        pqxx::work txn(conn);

        // Собираем JSON-представление текущего кэша
        std::string symbols_json = "[";
        bool first = true;
        for (const auto& [s, c] : cache_) {
            if (!first) symbols_json += ",";
            first = false;
            symbols_json += "{\"symbol\":\"" + s + "\","
                            + "\"enabled\":" + (c.enabled ? "true" : "false") + ","
                            + "\"min_hs_bps\":" + std::to_string(c.min_hs_bps) + ","
                            + "\"qty_usd\":" + std::to_string(c.qty_usd) + ","
                            + "\"base_spread_bps\":" + std::to_string(c.base_spread_bps) + ","
                            + "\"max_position_usd\":" + std::to_string(c.max_position_usd) + "}";
        }
        symbols_json += "]";

        std::string global_json = "{"
                                  + std::string("\"maker_fee_bps\":") + std::to_string(global_.maker_fee_bps) + ","
                                  + "\"max_half_spread_bps\":" + std::to_string(global_.max_half_spread_bps) + ","
                                  + "\"kill_adverse_bps\":" + std::to_string(global_.kill_adverse_bps) + ","
                                  + "\"half_spread_upper_pct\":" + std::to_string(global_.half_spread_upper_pct) + ","
                                  + "\"hard_cap_ratio\":" + std::to_string(global_.hard_cap_ratio) + ","
                                  + "\"block_ratio\":" + std::to_string(global_.block_ratio) + ","
                                  + "\"gamma\":" + std::to_string(global_.gamma) + ","
                                  + "\"vpin_pause_threshold\":" + std::to_string(global_.vpin_pause_threshold) + ","
                                  + "\"vpin_resume_threshold\":" + std::to_string(global_.vpin_resume_threshold) + ","
                                  + "\"soft_exit_pnl_pct\":" + std::to_string(global_.soft_exit_pnl_pct) + ","
                                  + "\"soft_exit_ratio\":" + std::to_string(global_.soft_exit_ratio) + ","
                                  + "\"min_qty_floor_usd\":" + std::to_string(global_.min_qty_floor_usd) + ","
                                  + "\"max_qty_multiplier\":" + std::to_string(global_.max_qty_multiplier) + ","
                                  + "\"max_position_default_usd\":" + std::to_string(global_.max_position_default_usd)
                                  + "}";

        txn.exec(
                "INSERT INTO mm_config_snapshots (symbols_json, global_json, comment) "
                "VALUES ($1::jsonb, $2::jsonb, $3)",
                pqxx::params{symbols_json, global_json, comment});
        txn.commit();
        std::cout << "[Registry] config snapshot saved\n";
    } catch (const std::exception& e) {
        std::cerr << "[Registry] snapshot failed: " << e.what() << "\n";
    }
}

MmSymbolConfig MmSymbolRegistry::get(const std::string& symbol) const {
    std::lock_guard lk(mtx_);
    auto it = cache_.find(symbol);
    if (it == cache_.end()) {
        MmSymbolConfig c;
        c.symbol = symbol;
        return c;   // enabled=true, все overrides = 0
    }
    return it->second;
}

bool MmSymbolRegistry::is_enabled(const std::string& symbol) const {
    std::lock_guard lk(mtx_);
    auto it = cache_.find(symbol);
    if (it == cache_.end()) return true;   // неизвестный — разрешён
    return it->second.enabled;
}

std::vector<std::string> MmSymbolRegistry::enabled_symbols() const {
    std::lock_guard lk(mtx_);
    std::vector<std::string> out;
    out.reserve(cache_.size());
    for (const auto& [s, c] : cache_) {
        if (c.enabled) out.push_back(s);
    }
    return out;
}

void MmSymbolRegistry::save_stats(
        const std::string& symbol,
        int fills, int cancels, int rejects,
        int avg_age_ms,
        double half_bps, double adverse_bps,
        double gross_edge_bps, double net_edge_bps,
        double fee_usd,
        const std::string& verdict) {
    try {
        pqxx::connection conn(conn_str_);
        pqxx::work txn(conn);
        txn.exec(
                "INSERT INTO mm_symbol_stats "
                "(symbol, fills, cancels, rejects, avg_age_ms, half_bps, "
                " adverse_bps, gross_edge_bps, net_edge_bps, fee_usd, verdict) "
                "VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11)",
                pqxx::params{symbol, fills, cancels, rejects, avg_age_ms,
                             half_bps, adverse_bps, gross_edge_bps,
                             net_edge_bps, fee_usd, verdict});
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[Registry] save_stats failed: " << e.what() << "\n";
    }
}

bool MmSymbolRegistry::is_whitelisted(const std::string& symbol) const {
    std::lock_guard lk(mtx_);
    auto it = cache_.find(symbol);
    if (it == cache_.end()) return false;   // НЕТ в таблице = НЕ торгуем
    return it->second.enabled;
}