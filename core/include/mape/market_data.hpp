#ifndef MAPE_MARKET_DATA_HPP
#define MAPE_MARKET_DATA_HPP

#include <optional>

#include "mape/market_types.hpp"

namespace mape {

// A snapshot of the market state needed to price the v1 instrument set.
//
// Backward-compatible market abstraction (plan §16.2). The flat scalar fields
// (spot/rate/vol/dividend) remain, so existing aggregate initialisation
// `MarketData{spot, rate, vol, div}` and direct `.rate`/`.vol` access keep
// working unchanged across the codebase and the FFI. *Optionally*, a richer
// `YieldCurve` and/or `VolSurface` can be attached; when present they override
// the flat scalar via the `rate_at` / `vol_at` accessors. New code
// (calibration, bucketed risk) uses the accessors; old code keeps using the
// fields. MarketData stays an aggregate (no user-declared constructors), so
// brace-init is preserved.
struct MarketData {
    double spot      = 0.0;  // current underlying price S
    double rate      = 0.0;  // continuously-compounded risk-free rate r (flat)
    double vol       = 0.0;  // volatility sigma (annualised, flat)
    double dividend  = 0.0;  // continuous dividend yield q (0 for non-dividend)

    // Optional richer market objects. Unset by default -> the flat scalars are
    // authoritative. Set one to use a term structure / smile instead. The
    // explicit `{}` default initialisers keep `MarketData{spot, rate, vol, div}`
    // brace-init working without tripping -Wmissing-field-initializers.
    std::optional<YieldCurve> curve{};
    std::optional<VolSurface> surface{};

    // Risk-free zero rate for maturity T: the curve if attached, else the flat
    // `rate`. Models that want term-structure-aware discounting call this.
    double rate_at(double T) const {
        return curve ? curve->rate_at(T) : rate;
    }

    // Volatility for a given strike/maturity: the surface if attached, else the
    // flat `vol`. Lets a model price off a smile rather than a single number.
    double vol_at(double strike, double maturity = 0.0) const {
        return surface ? surface->vol_at(strike, maturity) : vol;
    }
};

}  // namespace mape

#endif  // MAPE_MARKET_DATA_HPP
