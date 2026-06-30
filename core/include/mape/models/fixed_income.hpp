#ifndef MAPE_MODELS_FIXED_INCOME_HPP
#define MAPE_MODELS_FIXED_INCOME_HPP

#include <cmath>

#include "mape/instruments.hpp"
#include "mape/market_data.hpp"

namespace mape {

// Analytic pricers for the non-option instruments declared in instruments.hpp.
// These use the flat continuously-compounded rate in MarketData as the
// discount curve (a single-curve world; a bootstrapped term structure is a
// further stretch goal).

// Vanilla fixed-coupon bond: present value of the coupon stream plus the
// redemption of face at maturity, discounted continuously at MarketData.rate.
inline double price_bond(const Bond& bond, const MarketData& mkt) {
    const double r = mkt.rate;
    const int f = bond.frequency > 0 ? bond.frequency : 1;
    const double coupon_amt = bond.face * bond.coupon / static_cast<double>(f);
    const double dt = 1.0 / static_cast<double>(f);

    double pv = 0.0;
    // Number of coupon periods (round to nearest to tolerate fp maturity).
    const int n = static_cast<int>(std::lround(bond.maturity * f));
    for (int i = 1; i <= n; ++i) {
        const double t = i * dt;
        pv += coupon_amt * std::exp(-r * t);
    }
    pv += bond.face * std::exp(-r * bond.maturity);  // principal at maturity
    return pv;
}

// Present value of an FX forward to the long side of the contract, per unit of
// foreign currency notional. Using covered interest parity, the fair forward
// is F = S * exp((r_dom - r_for) * T); the contract struck at K is worth
// (F - K) discounted at the domestic rate:
//     PV = S * exp(-r_for * T) - K * exp(-r_dom * T)
// where MarketData.rate is the domestic rate and MarketData.spot is the spot
// FX rate (domestic per foreign).
inline double price_fx_forward(const FxForward& fwd, const MarketData& mkt) {
    const double S = mkt.spot;
    const double r_dom = mkt.rate;
    const double r_for = fwd.foreign_rate;
    const double T = fwd.maturity;
    return S * std::exp(-r_for * T) - fwd.strike * std::exp(-r_dom * T);
}

// Fair (par) forward FX rate implied by covered interest parity.
inline double fx_forward_rate(const FxForward& fwd, const MarketData& mkt) {
    return mkt.spot * std::exp((mkt.rate - fwd.foreign_rate) * fwd.maturity);
}

}  // namespace mape

#endif  // MAPE_MODELS_FIXED_INCOME_HPP
