#pragma once
#include <cstdint>

namespace mm {

    enum class SymMode : uint8_t {
        Normal   = 0,
        Tier1    = 1,
        Toxic    = 2,
        Illiquid = 3,
    };

    inline const char* to_str(SymMode m) {
        switch (m) {
            case SymMode::Tier1:    return "Tier1";
            case SymMode::Toxic:    return "Toxic";
            case SymMode::Illiquid: return "Illiquid";
            default:                return "Normal";
        }
    }

    struct SymModeSide {
        SymMode  mode            = SymMode::Normal;
        double   med_mk_30s      = 0.0;   // bps
        double   med_mk_1s       = 0.0;   // bps
        double   hit_pct_1s      = 0.0;   // %
        int      n_markouts_4h   = 0;
        uint64_t entered_mode_ms = 0;     // для гистерезиса
    };

    struct SymModeSnapshot {
        SymModeSide bid;    // performance нашего BID (= mm_markouts WHERE side='Buy')
        SymModeSide ask;    // performance нашего ASK (= mm_markouts WHERE side='Sell')

        // Общe-символьные (market-derived)
        double   fill_rate_1h = 0.0;
        double   depth_usdt   = 0.0;
        double   spread_frac  = 0.0;      // рыночный спред в долях, НЕ наша котировка
        double   vpin_now     = 0.0;

        uint64_t updated_ms   = 0;
        bool     has_data     = false;    // хоть одна сторона набрала >= 20 markouts
    };

// Классификация одной стороны
    SymMode classify_side(const SymModeSide& s,
                          double depth_usdt,
                          double spread_frac,
                          double vpin_now);

// Гистерезис + «вход быстрый, выход медленный»
    SymModeSide apply_hysteresis(const SymModeSide& prev,
                                 SymModeSide        candidate,
                                 uint64_t           now_ms);

} // namespace mm