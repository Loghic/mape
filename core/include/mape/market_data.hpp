#ifndef MAPE_MARKET_DATA_HPP
#define MAPE_MARKET_DATA_HPP

namespace mape {

// A flat snapshot of the market state needed to price the v1 instrument set.
// Kept deliberately small and POD-like; richer term structures are a stretch
// goal (see plan §12).
struct MarketData {
    double spot      = 0.0;  // current underlying price S
    double rate      = 0.0;  // continuously-compounded risk-free rate r
    double vol       = 0.0;  // volatility sigma (annualised)
    double dividend  = 0.0;  // continuous dividend yield q (0 for non-dividend)
};

}  // namespace mape

#endif  // MAPE_MARKET_DATA_HPP
