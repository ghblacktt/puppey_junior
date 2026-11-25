#pragma once
#include <deque>
#include <cmath>
#include <cstdint>
#include <vector>
#include <algorithm>

namespace mm {

    template<typename T>
    struct Event {
        uint64_t timestamp_ms;
        T value;
    };

    class TimeDecayStats {
    public:
        explicit TimeDecayStats(uint64_t tau_ms = 900'000) : tau_ms_(tau_ms) {}

        void add(double value, uint64_t timestamp_ms) {
            events_.push_back({timestamp_ms, value});
            prune_old_events(timestamp_ms);
        }

        double weighted_mean(uint64_t current_time_ms) const {
            if (events_.empty()) return 0.0;
            double sum_weights = 0.0;
            double sum_values = 0.0;
            for (const auto& evt : events_) {
                double age_ms = current_time_ms - evt.timestamp_ms;
                double weight = std::exp(-age_ms / tau_ms_);
                sum_weights += weight;
                sum_values += weight * evt.value;
            }
            return sum_weights > 0.0 ? sum_values / sum_weights : 0.0;
        }

        double weighted_sum(uint64_t current_time_ms) const {
            double sum = 0.0;
            for (const auto& evt : events_) {
                double age_ms = current_time_ms - evt.timestamp_ms;
                double weight = std::exp(-age_ms / tau_ms_);
                sum += weight;
            }
            return sum;
        }

        double weighted_percentile(double percentile, uint64_t current_time_ms) const {
            if (events_.empty()) return 0.0;
            std::vector<std::pair<double, double>> weighted_values;
            weighted_values.reserve(events_.size());
            for (const auto& evt : events_) {
                double age_ms = current_time_ms - evt.timestamp_ms;
                double weight = std::exp(-age_ms / tau_ms_);
                weighted_values.emplace_back(evt.value, weight);
            }
            std::sort(weighted_values.begin(), weighted_values.end());
            double total_weight = 0.0;
            for (const auto& [val, w] : weighted_values) {
                total_weight += w;
            }
            double cumulative = 0.0;
            double target = percentile * total_weight;
            for (const auto& [val, weight] : weighted_values) {
                cumulative += weight;
                if (cumulative >= target) return val;
            }
            return weighted_values.back().first;
        }

        void prune_old_events(uint64_t current_time_ms) {
            uint64_t cutoff = current_time_ms - 3 * tau_ms_;
            while (!events_.empty() && events_.front().timestamp_ms < cutoff) {
                events_.pop_front();
            }
        }

    private:
        std::deque<Event<double>> events_;
        uint64_t tau_ms_;
    };

} // namespace mm