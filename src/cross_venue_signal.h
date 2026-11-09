#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class CrossVenueSignalEngine {
public:
    struct Config {
        double ofi_weight          = 0.0;   // было 1.0
        double bybit_flow_weight   = 0.3;   // было 3.0
        double bitunix_flow_weight = 1.0;   // было 1.5
        double return_weight       = 0.05;   // было 1.0
        double   max_skew_bps        = 3.0;
        double   flow_min_abs        = 0.10;
        uint64_t flow_window_ms      = 2000;      // ← было 500
        size_t   flow_min_trades     = 5;         // ← NEW
        double   ofi_clamp           = 3.0;
        size_t   ofi_window          = 20;
        size_t   delta_window        = 100;
        double   ret_clamp = 4.0;
    };

    struct Signal {
        double target_skew_bps        = 0.0;
        double delta_bps              = 0.0;
        double delta_baseline_bps     = 0.0;
        double ofi_bybit              = 0.0;
        double ret_500ms_bybit        = 0.0;
        double microprice_bybit       = 0.0;
        double microprice_bitunix     = 0.0;
        double bybit_flow_imbalance   = 0.0;
        double bitunix_flow_imbalance = 0.0;
        double flow_component_bps     = 0.0;
        double cross_skew_bps         = 0.0;
        size_t bybit_flow_count   = 0;
        size_t bitunix_flow_count = 0;
    };

    CrossVenueSignalEngine();
    explicit CrossVenueSignalEngine(const Config& cfg);

    // Pre-register all symbols at startup. Must be called before on_*/drain/get_signal.
    void set_symbols(const std::vector<std::string>& syms);

    // ── Producers (WS threads): lock-free SPSC enqueue ──
    void on_bybit_book   (const std::string& symbol, double bid, double ask,
                          double bid_size, double ask_size);
    void on_bybit_trade  (const std::string& symbol, double price, double size,
                          bool is_buyer_maker);
    void on_bitunix_book (const std::string& symbol, double bid, double ask,
                          double bid_size, double ask_size);
    void on_bitunix_trade(const std::string& symbol, double price, double size,
                          bool is_buyer_maker);

    // ── Consumer (MM tick thread): drain queues, apply to state ──
    void drain(uint64_t now_ms);

    // ── Consumer (MM tick thread): read snapshot, no lock ──
    Signal get_signal(const std::string& symbol) const;

private:
    struct CvEvent {
        uint64_t ts_ms;
        double   a, b, c, d;    // book: bid, ask, bid_sz, ask_sz | trade: px, sz, _, _
        uint32_t symbol_idx;
        uint8_t  kind;          // 0 = Book, 1 = Trade
        uint8_t  is_bm;         // is_buyer_maker (trades only)
        uint16_t _pad;
    };
    static_assert(sizeof(CvEvent) <= 64, "CvEvent must fit cache line");

    class SpscQueue {
    public:
        explicit SpscQueue(size_t cap_pow2)
                : buf_(cap_pow2), mask_(cap_pow2 - 1) {}
        bool push(const CvEvent& e) {
            size_t h = head_.load(std::memory_order_relaxed);
            size_t t = tail_.load(std::memory_order_acquire);
            if (h - t >= buf_.size()) return false;        // full — drop
            buf_[h & mask_] = e;
            head_.store(h + 1, std::memory_order_release);
            return true;
        }
        bool pop(CvEvent& out) {
            size_t t = tail_.load(std::memory_order_relaxed);
            size_t h = head_.load(std::memory_order_acquire);
            if (t == h) return false;                       // empty
            out = buf_[t & mask_];
            tail_.store(t + 1, std::memory_order_release);
            return true;
        }
    private:
        std::vector<CvEvent> buf_;
        size_t mask_;
        alignas(64) std::atomic<size_t> head_{0};
        alignas(64) std::atomic<size_t> tail_{0};
    };

    struct TradeFlow {
        std::deque<std::pair<uint64_t, double>> trades;
        double   buy_vol   = 0.0;
        double   sell_vol  = 0.0;
        uint64_t window_ms = 500;
        size_t   min_trades = 5;   // ← NEW

        void set_window(uint64_t w) { window_ms = w; }
        void set_min_trades(size_t n) { min_trades = n; }

        void add(uint64_t ts, double size, bool is_buy_aggressor);
        void prune(uint64_t now);

        double imbalance() const {
            if (trades.size() < min_trades) return 0.0;   // ← NEW: фильтр шума
            double tot = buy_vol + sell_vol;
            if (tot < 1e-9) return 0.0;
            return (buy_vol - sell_vol) / tot;
        }
    };

    struct SymbolState {
        std::deque<std::pair<uint64_t, double>> mid_history;
        std::deque<double> delta_buf;
        std::deque<double> ofi_buf;

        double microprice_bybit    = 0.0;
        double microprice_bitunix  = 0.0;
        double last_mid_bybit      = 0.0;
        double delta_baseline      = 0.0;

        double last_bid = 0.0, last_ask = 0.0;
        double last_bid_size = 0.0, last_ask_size = 0.0;

        TradeFlow bybit_flow;
        TradeFlow bitunix_flow;
    };

    static uint64_t now_ms();

    void apply_event (const CvEvent&, uint64_t now);
    void apply_book  (SymbolState&, const CvEvent&, uint64_t now, bool is_bybit);
    void apply_trade (SymbolState&, const CvEvent&, uint64_t now, bool is_bybit);

    double compute_microprice   (double bid, double ask, double bs, double as) const;
    double compute_ofi          (SymbolState&, double bid, double ask,
                                 double bid_size, double ask_size);
    double compute_ret_500ms    (const SymbolState&, double cur_mid) const;
    double compute_median_delta (const std::deque<double>&) const;

    Config cfg_;

    SpscQueue bybit_q_;
    SpscQueue bitunix_q_;

    // symbol_idx map: written once in set_symbols(), read-only after.
    // Concurrent const reads are safe — no lock.
    std::unordered_map<std::string, uint32_t> sym_to_idx_;
    std::vector<std::string>   idx_to_sym_;
    std::vector<SymbolState>   states_;    // consumer-only writes
};