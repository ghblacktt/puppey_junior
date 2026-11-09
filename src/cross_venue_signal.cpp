#include "cross_venue_signal.h"
#include <algorithm>
#include <cmath>
#include <iostream>

// ── constants ──
static constexpr size_t kQueueCap = 16384;   // must be pow2

// ── ctor ──
CrossVenueSignalEngine::CrossVenueSignalEngine()
        : CrossVenueSignalEngine(Config{}) {}

CrossVenueSignalEngine::CrossVenueSignalEngine(const Config& cfg)
        : cfg_(cfg),
          bybit_q_(kQueueCap),
          bitunix_q_(kQueueCap) {}

// ── setup ──
void CrossVenueSignalEngine::set_symbols(const std::vector<std::string>& syms) {
    sym_to_idx_.clear();
    idx_to_sym_.clear();
    states_.clear();
    sym_to_idx_.reserve(syms.size());
    idx_to_sym_.reserve(syms.size());
    states_.resize(syms.size());
    for (size_t i = 0; i < syms.size(); ++i) {
        sym_to_idx_[syms[i]] = static_cast<uint32_t>(i);
        idx_to_sym_.push_back(syms[i]);
        states_[i].bybit_flow.set_window(cfg_.flow_window_ms);
        states_[i].bybit_flow.set_min_trades(cfg_.flow_min_trades);   // ← NEW
        states_[i].bitunix_flow.set_window(cfg_.flow_window_ms);
        states_[i].bitunix_flow.set_min_trades(cfg_.flow_min_trades); // ← NEW
    }
}

uint64_t CrossVenueSignalEngine::now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
}

// ── producers (lock-free enqueue) ──
void CrossVenueSignalEngine::on_bybit_book(
        const std::string& symbol, double bid, double ask,
        double bid_size, double ask_size) {
    const auto& m = sym_to_idx_;              // const → concurrent-safe find
    auto it = m.find(symbol);
    if (it == m.end()) return;
    CvEvent e{};
    e.ts_ms      = now_ms();
    e.a = bid; e.b = ask; e.c = bid_size; e.d = ask_size;
    e.symbol_idx = it->second;
    e.kind       = 0;
    if (!bybit_q_.push(e)) {
        static std::atomic<uint64_t> drops{0};
        auto n = drops.fetch_add(1, std::memory_order_relaxed);
        if ((n & 0xFF) == 0)
            std::cerr << "[CROSS] bybit queue full (total drops=" << n << ")\n";
    }
}

void CrossVenueSignalEngine::on_bybit_trade(
        const std::string& symbol, double price, double size, bool is_buyer_maker) {
    const auto& m = sym_to_idx_;
    auto it = m.find(symbol);
    if (it == m.end()) return;
    CvEvent e{};
    e.ts_ms      = now_ms();
    e.a = price; e.b = size;
    e.symbol_idx = it->second;
    e.kind       = 1;
    e.is_bm      = is_buyer_maker ? 1 : 0;
    if (!bybit_q_.push(e)) {
        static std::atomic<uint64_t> drops{0};
        auto n = drops.fetch_add(1, std::memory_order_relaxed);
        if ((n & 0xFF) == 0)
            std::cerr << "[CROSS] bybit trade queue full (drops=" << n << ")\n";
    }
}

void CrossVenueSignalEngine::on_bitunix_book(
        const std::string& symbol, double bid, double ask,
        double bid_size, double ask_size) {
    const auto& m = sym_to_idx_;
    auto it = m.find(symbol);
    if (it == m.end()) return;
    CvEvent e{};
    e.ts_ms      = now_ms();
    e.a = bid; e.b = ask; e.c = bid_size; e.d = ask_size;
    e.symbol_idx = it->second;
    e.kind       = 0;
    if (!bitunix_q_.push(e)) {
        static std::atomic<uint64_t> drops{0};
        auto n = drops.fetch_add(1, std::memory_order_relaxed);
        if ((n & 0xFF) == 0)
            std::cerr << "[CROSS] bitunix queue full (drops=" << n << ")\n";
    }
}

void CrossVenueSignalEngine::on_bitunix_trade(
        const std::string& symbol, double price, double size, bool is_buyer_maker) {
    const auto& m = sym_to_idx_;
    auto it = m.find(symbol);
    if (it == m.end()) return;
    CvEvent e{};
    e.ts_ms      = now_ms();
    e.a = price; e.b = size;
    e.symbol_idx = it->second;
    e.kind       = 1;
    e.is_bm      = is_buyer_maker ? 1 : 0;
    if (!bitunix_q_.push(e)) {
        static std::atomic<uint64_t> drops{0};
        auto n = drops.fetch_add(1, std::memory_order_relaxed);
        if ((n & 0xFF) == 0)
            std::cerr << "[CROSS] bitunix trade queue full (drops=" << n << ")\n";
    }
}

void CrossVenueSignalEngine::apply_event(const CvEvent& e, uint64_t now) {
    if (e.symbol_idx >= states_.size()) return;
    auto& s = states_[e.symbol_idx];
    bool is_bybit = (e.kind & 0x80) == 0;   // not used; we tag by queue instead
    // NOTE: apply_book/trade distinguish by venue — we pass a flag.
    // Simpler: drain bybit first, then bitunix — but apply_* needs to know.
    // We use two separate drains; here the caller tells by venue.
    // (see drain_venue() in cpp below — replaced by explicit call)
    (void)s; (void)now;
}

// ── helper implementations ──
void CrossVenueSignalEngine::TradeFlow::add(uint64_t ts, double size, bool is_buy) {
    trades.emplace_back(ts, is_buy ? size : -size);
    if (is_buy) buy_vol += size;
    else        sell_vol += size;
    prune(ts);
}

void CrossVenueSignalEngine::TradeFlow::prune(uint64_t now) {
    while (!trades.empty() && now - trades.front().first > window_ms) {
        double s = trades.front().second;
        if (s > 0) buy_vol  -= s;
        else       sell_vol -= -s;
        trades.pop_front();
    }
}


double CrossVenueSignalEngine::compute_microprice(
        double bid, double ask, double bid_size, double ask_size) const {
    if (bid <= 0.0 || ask <= 0.0) return 0.0;
    double total = bid_size + ask_size;
    if (total <= 0.0) return (bid + ask) / 2.0;
    return (bid * ask_size + ask * bid_size) / total;
}

double CrossVenueSignalEngine::compute_ofi(
        SymbolState& s, double bid, double ask, double bid_size, double ask_size) {
    double ofi = 0.0;
    if (s.last_bid > 0.0) {
        if (bid >= s.last_bid) ofi += (bid_size - s.last_bid_size);
        else                   ofi -= s.last_bid_size;
        if (ask <= s.last_ask) ofi -= (ask_size - s.last_ask_size);
        else                   ofi += s.last_ask_size;
    }
    s.last_bid = bid; s.last_ask = ask;
    s.last_bid_size = bid_size; s.last_ask_size = ask_size;

    double norm = std::max(1.0, (bid_size + ask_size) / 2.0);
    ofi /= norm;
    s.ofi_buf.push_back(ofi);
    while (s.ofi_buf.size() > cfg_.ofi_window) s.ofi_buf.pop_front();

    double sum = 0.0;
    for (double v : s.ofi_buf) sum += v;
    return std::clamp(sum, -cfg_.ofi_clamp, cfg_.ofi_clamp);
}

double CrossVenueSignalEngine::compute_ret_500ms(
        const SymbolState& s, double cur_mid) const {
    if (s.mid_history.empty() || cur_mid <= 0.0) return 0.0;
    uint64_t cutoff = s.mid_history.back().first - 500;
    for (auto it = s.mid_history.rbegin(); it != s.mid_history.rend(); ++it) {
        if (it->first <= cutoff) {
            double old = it->second;
            if (old > 0.0) return (cur_mid - old) / old * 10000.0;
            break;
        }
    }
    return 0.0;
}

double CrossVenueSignalEngine::compute_median_delta(
        const std::deque<double>& buf) const {
    if (buf.empty()) return 0.0;
    std::vector<double> tmp(buf.begin(), buf.end());
    size_t n = tmp.size();
    std::nth_element(tmp.begin(), tmp.begin() + n / 2, tmp.end());
    return tmp[n / 2];
}

void CrossVenueSignalEngine::apply_book(
        SymbolState& s, const CvEvent& e, uint64_t now, bool is_bybit) {
    double bid = e.a, ask = e.b, bid_sz = e.c, ask_sz = e.d;
    if (bid <= 0.0 || ask <= 0.0) return;

    double micro = compute_microprice(bid, ask, bid_sz, ask_sz);
    double mid   = (bid + ask) / 2.0;

    if (is_bybit) {
        s.microprice_bybit = micro;
        s.last_mid_bybit   = mid;
        s.mid_history.emplace_back(e.ts_ms, mid);
        while (!s.mid_history.empty() && now - s.mid_history.front().first > 60'000)
            s.mid_history.pop_front();
        compute_ofi(s, bid, ask, bid_sz, ask_sz);
    } else {
        s.microprice_bitunix = micro;
    }

    // cross delta
    if (s.microprice_bybit > 0.0 && s.microprice_bitunix > 0.0) {
        double mid_cross = (s.microprice_bybit + s.microprice_bitunix) / 2.0;
        if (mid_cross > 0.0) {
            double delta = (s.microprice_bybit - s.microprice_bitunix)
                           / mid_cross * 10000.0;
            s.delta_buf.push_back(delta);
            while (s.delta_buf.size() > cfg_.delta_window) s.delta_buf.pop_front();
            s.delta_baseline = compute_median_delta(s.delta_buf);
        }
    }
}

void CrossVenueSignalEngine::apply_trade(
        SymbolState& s, const CvEvent& e, uint64_t now, bool is_bybit) {
    double px = e.a, sz = e.b;
    if (px <= 0 || sz <= 0) return;
    bool is_buy_aggressor = (e.is_bm == 0);
    if (is_bybit) s.bybit_flow.add(e.ts_ms, sz, is_buy_aggressor);
    else          s.bitunix_flow.add(e.ts_ms, sz, is_buy_aggressor);
    (void)now;
}

// ── drain split by venue ──
void CrossVenueSignalEngine::drain(uint64_t now) {
    CvEvent e;
    while (bybit_q_.pop(e)) {
        if (e.symbol_idx >= states_.size()) continue;
        auto& s = states_[e.symbol_idx];
        if (e.kind == 0) apply_book(s, e, now, /*is_bybit=*/true);
        else             apply_trade(s, e, now, /*is_bybit=*/true);
    }
    while (bitunix_q_.pop(e)) {
        if (e.symbol_idx >= states_.size()) continue;
        auto& s = states_[e.symbol_idx];
        if (e.kind == 0) apply_book(s, e, now, /*is_bybit=*/false);
        else             apply_trade(s, e, now, /*is_bybit=*/false);
    }
}

// ── get_signal: no lock, consumer-only ──
CrossVenueSignalEngine::Signal
CrossVenueSignalEngine::get_signal(const std::string& symbol) const {
    Signal out;
    const auto& m = sym_to_idx_;
    auto it = m.find(symbol);
    if (it == m.end()) return out;
    const auto& s = states_[it->second];

    out.microprice_bybit    = s.microprice_bybit;
    out.microprice_bitunix  = s.microprice_bitunix;
    out.delta_baseline_bps  = s.delta_baseline;

    if (s.microprice_bitunix > 0.0 && s.microprice_bybit > 0.0) {
        double mid = (s.microprice_bybit + s.microprice_bitunix) / 2.0;
        if (mid > 0.0)
            out.delta_bps = (s.microprice_bybit - s.microprice_bitunix)
                            / mid * 10000.0;
    }

    if (!s.ofi_buf.empty()) {
        double sum = 0.0;
        for (double v : s.ofi_buf) sum += v;
        out.ofi_bybit = std::clamp(sum, -cfg_.ofi_clamp, cfg_.ofi_clamp);   // ← clamp
    }

    out.ret_500ms_bybit = std::clamp(compute_ret_500ms(s, s.last_mid_bybit),
                                     -cfg_.ret_clamp, cfg_.ret_clamp);

    out.bybit_flow_imbalance   = s.bybit_flow.imbalance();
    out.bitunix_flow_imbalance = s.bitunix_flow.imbalance();
    out.bybit_flow_count       = s.bybit_flow.trades.size();      // ← NEW
    out.bitunix_flow_count     = s.bitunix_flow.trades.size();

    double flow_component = 0.0;
    if (std::abs(out.bybit_flow_imbalance) > cfg_.flow_min_abs)
        flow_component = out.bybit_flow_imbalance * cfg_.bybit_flow_weight;

    out.flow_component_bps = flow_component;

    double delta_excess = out.delta_bps - out.delta_baseline_bps;

    double raw_skew = out.ofi_bybit * cfg_.ofi_weight
                      + out.ret_500ms_bybit * cfg_.return_weight
                      + flow_component;

    out.target_skew_bps = std::clamp(raw_skew, -cfg_.max_skew_bps, cfg_.max_skew_bps);
    out.cross_skew_bps  = out.target_skew_bps;
    return out;
}