#pragma once
#include <cstdint>
#include <cstddef>
#include <functional>
#include <map>

namespace scanner {

// ============================================================
// LocalBook — локальный LOB для одного символа.
// Применяет snapshot (reset + bulk) и delta (по уровням).
//
// Bybit V5 semantics:
//   - snapshot: полный набор уровней (до depth), reset перед apply
//   - delta:   только изменённые уровни, sz=0 = удалить уровень
//
// BBO = max(bids) / min(asks). Гарантированно корректный после
// любого количества snapshot+delta в произвольном порядке.
// ============================================================
    class LocalBook {
    public:
        void reset();

        // sz == 0 (или < eps) → удалить уровень. Иначе — заменить.
        void apply_bid(double px, double sz);
        void apply_ask(double px, double sz);

        double best_bid()  const;
        double best_ask()  const;
        double bid_size()  const;
        double ask_size()  const;

        // Проверка что top-of-book корректен (bid < ask)
        bool is_valid_bbo() const;

        uint64_t last_update_id() const         { return last_update_id_; }
        void     set_update_id(uint64_t id)     { last_update_id_ = id; }

        bool   empty()      const { return bids_.empty() || asks_.empty(); }
        size_t bid_levels() const { return bids_.size(); }
        size_t ask_levels() const { return asks_.size(); }

    private:
        std::map<double, double, std::greater<double>> bids_;   // px desc
        std::map<double, double, std::less<double>>    asks_;   // px asc
        uint64_t last_update_id_ = 0;
    };

} // namespace scanner