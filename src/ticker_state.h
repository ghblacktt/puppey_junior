#include "mean_reversion_strategy.h"

struct TickerState {
    std::string symbol;
    OrderBook orderbook;
    ClusterEngine cluster;
    TimeWindowVWMA vwma;
    DynamicBBEngine bb;
    std::unique_ptr<MeanReversionStrategy> strategy;  // изменено
    RegimeFilter regime_filter;
    TickerState(const std::string& sym, BybitApiClient& api, int positionIdx = 1)
            : symbol(sym)
            , vwma(200)
            , bb(200, 0.85, 6.0, 200, 2.0, 60, 90.0)
            , regime_filter(14, 25.0, 20.0, 200, 50)
    {
        strategy = std::make_unique<MeanReversionStrategy>(
                orderbook, cluster, vwma, bb
        );
    }
};