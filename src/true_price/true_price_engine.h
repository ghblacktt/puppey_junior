#pragma once
#include <array>
#include <cstdint>
#include <cmath>
#include <algorithm>

namespace tp {

// Venue: 0=bybit, 1=binance (расширяемо)
// Side:  0=buy,  1=sell
    struct HawkesState {
        double value       = 0.0;   // накопленный impact (bps)
        double last_ts_ms  = 0.0;
        double amplitude   = 0.0;   // a_ij  (tick: adjacency)
        double decay       = 0.0;   // b_ij  (tick: decays, 1/sec)
    };

    class TruePriceEngine {
    public:
        static constexpr int kVenues = 2;
        static constexpr int kSides  = 2;

        struct Config {
            // a[venue][side] — амплитуда (bps на единицу normalized size)
            // Калибруется через tick.HawkesExpKern -> learner.adjacency
            double amplitude[kVenues][kSides] = {
                    {2.0, 2.0},
                    {1.5, 1.5},
            };
            // b[venue][side] — decay (1/sec)
            // tick.HawkesExpKern -> learner.decays
            double decay[kVenues][kSides] = {
                    {50.0, 50.0},
                    {30.0, 30.0},
            };
            double max_skew_bps = 8.0;
        };

        void set_config(const Config& c) {
            cfg_ = c;
            for (int v = 0; v < kVenues; ++v)
                for (int s = 0; s < kSides; ++s) {
                    state_[v][s].amplitude = c.amplitude[v][s];
                    state_[v][s].decay     = c.decay[v][s];
                }
        }

        // sz_norm = trade_size / best_size_of_that_venue  (уже нормализовано)
        void on_trade(int venue, int side, double sz_norm, uint64_t ts_ms) {
            if (venue < 0 || venue >= kVenues || side < 0 || side >= kSides) return;
            auto& st = state_[venue][side];

            double dt = (ts_ms > st.last_ts_ms) ? (ts_ms - st.last_ts_ms) / 1000.0 : 0.0;
            st.value *= std::exp(-st.decay * dt);

            double contrib = st.amplitude * std::min(1.0, std::max(0.0, sz_norm));
            st.value += contrib;
            st.last_ts_ms = static_cast<double>(ts_ms);
        }
        // Для momentum score — текущий decay по venue
        double decay_for_venue(int venue) const {
            if (venue < 0 || venue >= kVenues) return 1.0;
            return state_[venue][0].decay;
        }
        double target_skew_bps(uint64_t now_ms) const {
            double total = 0.0;
            for (int v = 0; v < kVenues; ++v) {
                for (int s = 0; s < kSides; ++s) {
                    const auto& st = state_[v][s];
                    double dt = (now_ms > st.last_ts_ms)
                                ? (now_ms - st.last_ts_ms) / 1000.0 : 0.0;
                    double decayed = st.value * std::exp(-st.decay * dt);
                    double sign = (s == 0) ? +1.0 : -1.0;
                    total += sign * decayed;
                }
            }
            return std::clamp(total, -cfg_.max_skew_bps, cfg_.max_skew_bps);
        }

        struct Snapshot {
            double value[kVenues][kSides];
            double skew_bps;
        };
        Snapshot snapshot(uint64_t now_ms) const {
            Snapshot s{};
            for (int v = 0; v < kVenues; ++v)
                for (int k = 0; k < kSides; ++k) {
                    const auto& st = state_[v][k];
                    double dt = (now_ms > st.last_ts_ms)
                                ? (now_ms - st.last_ts_ms) / 1000.0 : 0.0;
                    s.value[v][k] = st.value * std::exp(-st.decay * dt);
                }
            s.skew_bps = target_skew_bps(now_ms);
            return s;
        }

    private:
        Config      cfg_;
        HawkesState state_[kVenues][kSides];
    };

} // namespace tp