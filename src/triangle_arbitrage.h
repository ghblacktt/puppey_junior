#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <chrono>
#include <functional>
#include <unordered_map>

class BybitApiClient;

struct TriangleLeg {
    std::string symbol;
    std::string side;    // "Buy" / "Sell"
    double      qty;     // нормализованное количество (в базовой монете)
    double      price;   // IOC limit (ask для Buy, bid для Sell)
};

class TriangularArbitrage {
public:
    struct Config {
        double   min_edge_bps        = 10.0;   // порог входа после комиссий
        double   max_book_age_ms     = 250;    // все 3 ноги должны быть свежее
        double   notional_usd        = 20.0;   // размер в USDT на 1 цикл
        double   taker_fee_bps       = 5.5;    // Bybit linear taker
        bool     dry_run             = true;   // логируем, но не торгуем
        int      max_exec_per_min    = 5;
    };

    // Провайдер текущего bid/ask для символа (читает из OrderBook)
    using SnapshotFn = std::function<bool(const std::string& symbol,
                                          double& bid, double& ask)>;

    // Провайдер размещения ордера: возвращает order_id или "" при ошибке
    using PlaceFn = std::function<std::string(const std::string& symbol,
                                              const std::string& side,
                                              double price, double qty)>;
// PlaceFn теперь указывает на place_ioc_limit_order_spot

    TriangularArbitrage(BybitApiClient& api) : api_(api) {}
    explicit TriangularArbitrage(BybitApiClient& api, Config cfg);

    void set_snapshot_fn(SnapshotFn fn) { snap_fn_ = std::move(fn); }
    void set_place_fn(PlaceFn fn)       { place_fn_ = std::move(fn); }

    // Вызывать 1 раз после получения всех символов.
    // available_symbols — все, что доступно на бирже (для кросс-пар).
    void discover(const std::vector<std::string>& available_symbols);

    // Вызывать из WS-клиента на каждое обновление стакана
    void on_book_update(const std::string& symbol);

    size_t triangle_count() const { return triangles_.size(); }
    void   print_stats() const;

    std::atomic<uint64_t> checks_{0};
    std::atomic<uint64_t> fresh_checks_{0};    // ← NEW
    std::atomic<uint64_t> opportunities_{0};
private:
    struct RtTriangle {
        std::string id;       // "ETHBTC/USDT"
        std::string usdt_a;   // "ETHUSDT"
        std::string usdt_b;   // "BTCUSDT"
        std::string cross;    // "ETHBTC"
        std::string base_a;   // "ETH"
        std::string base_b;   // "BTC"
    };

    struct Snapshot {
        double   bid;
        double   ask;
        uint64_t ts_ms;
    };

    BybitApiClient& api_;
    Config          cfg_;
    SnapshotFn      snap_fn_;
    PlaceFn         place_fn_;

    std::vector<RtTriangle>                       triangles_;
    std::unordered_map<std::string, Snapshot>     snapshots_;
    std::unordered_map<std::string, std::vector<size_t>> symbol_to_triangles_;

    std::atomic<uint64_t> executions_{0};
    std::atomic<uint64_t> failures_{0};
    std::atomic<double>   best_edge_bps_{0.0};

    std::chrono::steady_clock::time_point last_exec_;
    void check_triangle(size_t idx);
    bool is_fresh(const std::string& sym, uint64_t now_ms) const;
    void try_execute(const RtTriangle& tri,
                     const char* direction,          // "A" or "B"
                     const std::vector<TriangleLeg>& path,
                     double gross_ratio);
    void unwind_exposure(const std::vector<TriangleLeg>& filled);
};