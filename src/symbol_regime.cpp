#include "symbol_regime.h"
#include <algorithm>

namespace mm {

    SymMode classify_side(const SymModeSide& s,
                          double depth_usdt,
                          double spread_frac,
                          double vpin_now)
    {
        // ── Warm-up: экстремальные случаи ловим раньше ──
        if (s.n_markouts_4h < 10) return SymMode::Normal;

        if (s.n_markouts_4h < 20) {
            // 10..19 fills — только абсолютно очевидные
            if (s.hit_pct_1s < 15.0)  return SymMode::Toxic;   // hit 0-14% — катастрофа
            if (s.med_mk_1s  < -10.0) return SymMode::Toxic;   // mk -10 bps — fat tail
            return SymMode::Normal;
        }

        // ── n >= 20: полная логика ──
        const double spread_bps = spread_frac * 10000.0;

        if (depth_usdt < 50'000.0 || spread_bps > 15.0) {
            return SymMode::Illiquid;
        }

        const bool toxic =
                s.hit_pct_1s  < 30.0
                || s.med_mk_1s   < -2.0
                || s.med_mk_30s  < -2.0
                || vpin_now      > 0.75;
        if (toxic) return SymMode::Toxic;

        if (depth_usdt > 500'000.0
            && spread_bps < 5.0
            && s.hit_pct_1s >= 40.0
            && s.med_mk_30s >= -0.5
            && s.n_markouts_4h >= 30) {
            return SymMode::Tier1;
        }

        return SymMode::Normal;
    }

    SymModeSide apply_hysteresis(const SymModeSide& prev,
                                 SymModeSide        candidate,
                                 uint64_t           now_ms)
    {
        constexpr uint64_t kMinHoldMs    = 30ULL * 60 * 1000;    // 30 мин до любого перехода
        constexpr uint64_t kExitToxicMs  = 2ULL * 60 * 60 * 1000; // 2 часа чтобы выйти из Toxic
        constexpr uint64_t kExitTier1Ms  = 4ULL * 60 * 60 * 1000; // 4 часа чтобы выйти из Tier1

        if (prev.mode == candidate.mode) {
            candidate.entered_mode_ms = prev.entered_mode_ms;
            return candidate;
        }

        // Ещё не сидели в предыдущем режиме min_hold — не переключаемся
        if (prev.entered_mode_ms > 0
            && (now_ms - prev.entered_mode_ms) < kMinHoldMs) {
            candidate.mode = prev.mode;
            candidate.entered_mode_ms = prev.entered_mode_ms;
            return candidate;
        }

        // Выход из Toxic — медленный (автор: «токсик вернётся через 48ч»)
        if (prev.mode == SymMode::Toxic
            && prev.entered_mode_ms > 0
            && (now_ms - prev.entered_mode_ms) < kExitToxicMs) {
            candidate.mode = prev.mode;
            candidate.entered_mode_ms = prev.entered_mode_ms;
            return candidate;
        }

        // Выход из Tier1 — медленный (не дёргаемся)
        if (prev.mode == SymMode::Tier1
            && prev.entered_mode_ms > 0
            && (now_ms - prev.entered_mode_ms) < kExitTier1Ms) {
            candidate.mode = prev.mode;
            candidate.entered_mode_ms = prev.entered_mode_ms;
            return candidate;
        }

        candidate.entered_mode_ms = now_ms;
        return candidate;
    }

} // namespace mm