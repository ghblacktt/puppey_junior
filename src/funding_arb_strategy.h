#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <fstream>
#include "bybit/bybit_api_client.h"

class FundingArbStrategy {
public:
    struct Config {
        // Discovery
        double   min_funding_bps       = 0.8;
        double   exit_funding_bps      = -0.2;
        double   min_turnover_24h_usd  = 5'000'000.0;
        double   max_basis_bps         = 8.0;
        double   max_spot_ask_usd      = 100'000.0;  // не берём BTC по $80k (нельзя много на $500)
        double   min_spot_ask_usd      = 0.0001;
        uint64_t rescan_interval_ms    = 600'000;    // 10 мин

        // Position sizing
        double   notional_usd_per_pos  = 500.0;
        size_t   max_positions         = 100;
        double   max_total_notional    = 3'000.0;
        double   max_hold_hours        = 720.0;

        // Runtime
        bool     dry_run               = false;
    };

    struct Opportunity {
        std::string symbol;
        double      funding_bps;
        double      basis_bps;
        double      turnover_24h;
        double      spot_ask;
        double      perp_price;
    };

    struct Position {
        std::string symbol;
        double      spot_qty;
        double      perp_qty;
        double      entry_spot_price;
        double      entry_perp_price;
        double      entry_basis_bps;
        double      entry_funding_bps;
        uint64_t    opened_ms;
    };

    FundingArbStrategy(BybitApiClient& api) : api_(api) {}
    explicit FundingArbStrategy(BybitApiClient& api, Config cfg);

    void tick();

    void print_status() const;
    void save_positions() const;              // ← в JSON
    void load_positions();                    // ← из JSON
    void save_monitor_csv(const std::string& path) const;
    void print_final_summary() const;

    double get_initial_balance() const { return initial_balance_; }
    void   set_initial_balance(double b) { initial_balance_ = b; }
    size_t position_count() const { return positions_.size(); }
    const std::vector<Position>& positions() const { return positions_; }
    const std::vector<Opportunity>& last_opportunities() const { return last_opps_; }

private:
    BybitApiClient& api_;
    Config cfg_;
    std::vector<Position> positions_;
    std::vector<Opportunity> last_opps_;
    uint64_t last_scan_ms_ = 0;
    std::unordered_set<std::string> spot_symbols_;   // кэш, обновляется редко
    double initial_balance_ = 0.0;            // для расчёта net PnL
    double funding_collected_usd_ = 0.0;      // накопленный funding

    static std::string positions_file() { return "data/funding_positions.json"; }
    static std::string monitor_file()   { return "data/funding_monitor.csv"; }
    static std::string events_file()    { return "data/funding_events.log"; }
    void refresh_spot_symbols();
    void discover_opportunities();
    void check_positions();
    void open_position(const Opportunity& opp);
    void close_position(size_t idx, const std::string& reason);
    double get_basis_bps(const std::string& symbol);
    double get_total_notional() const;
    bool has_position(const std::string& symbol) const;
    static uint64_t now_ms();
};