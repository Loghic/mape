#ifndef MAPE_IMPLIED_VOL_HPP
#define MAPE_IMPLIED_VOL_HPP

#include <algorithm>
#include <cmath>
#include <optional>

#include "mape/instruments.hpp"
#include "mape/market_data.hpp"
#include "mape/models/black_scholes.hpp"
#include "mape/models/black_scholes_ad.hpp"

namespace mape {

// Invert Black-Scholes for the volatility that reproduces a given market price
// (plan §12 territory: using the engine *backwards*). This is the heart of the
// real-data work — a quoted option price implies a vol, and a chain of strikes
// gives a volatility smile.
//
// Returns std::nullopt when no valid vol exists: a price below intrinsic value,
// at/above the no-arbitrage upper bound, or non-finite. Callers (the smile
// chart) simply skip those strikes rather than fabricate a number.
//
// Method: Newton-Raphson seeded with a Brenner-Subrahmanyam ATM guess, using
// the exact AD vega as the derivative. Falls back to bisection if Newton leaves
// the bracket or stalls (vega -> 0 deep ITM/OTM), which keeps it robust.
inline std::optional<double> implied_vol(OptionType type, double market_price,
                                         double spot, double strike,
                                         double rate, double maturity,
                                         double dividend = 0.0,
                                         double tol = 1e-8,
                                         int max_iter = 100) {
    if (!std::isfinite(market_price) || market_price <= 0.0 || spot <= 0.0 ||
        strike <= 0.0 || maturity <= 0.0) {
        return std::nullopt;
    }

    const double df_r = std::exp(-rate * maturity);
    const double df_q = std::exp(-dividend * maturity);

    // No-arbitrage bounds. Below intrinsic or above the trivial upper bound,
    // no real positive vol can reproduce the price.
    const double fwd = spot * df_q;
    double lower_bound, upper_bound;
    if (type == OptionType::Call) {
        lower_bound = std::max(fwd - strike * df_r, 0.0);
        upper_bound = fwd;  // call <= S e^{-qT}
    } else {
        lower_bound = std::max(strike * df_r - fwd, 0.0);
        upper_bound = strike * df_r;  // put <= K e^{-rT}
    }
    // Reject prices at/beyond the no-arbitrage boundary. The tolerance is
    // relative to the price scale: a deep in-the-money option legitimately
    // trades a hair above intrinsic, so an absolute epsilon would wrongly
    // reject it. We only bail when the price is *at or past* the boundary.
    const double scale = std::max(1.0, upper_bound);
    const double boundary_eps = 1e-10 * scale;
    if (market_price <= lower_bound - boundary_eps ||
        market_price >= upper_bound + boundary_eps) {
        return std::nullopt;
    }
    // Clamp a price marginally outside the bracket back onto it so the solver
    // has a valid target (handles floating-point dust at the boundary).
    const double target = std::clamp(market_price, lower_bound + boundary_eps,
                                     upper_bound - boundary_eps);

    BlackScholesAD ad;  // gives exact vega
    auto price_at = [&](double sigma) {
        Option opt{type, Exercise::European, strike, maturity};
        MarketData mkt{spot, rate, sigma, dividend};
        return ad.price(opt, mkt);
    };
    auto vega_at = [&](double sigma) {
        Option opt{type, Exercise::European, strike, maturity};
        MarketData mkt{spot, rate, sigma, dividend};
        return ad.vega(opt, mkt);
    };

    // Brenner-Subrahmanyam closed-form ATM approximation as a starting guess.
    double sigma = std::sqrt(2.0 * M_PI / maturity) * (target / spot);
    if (!(sigma > 0.0) || !std::isfinite(sigma)) sigma = 0.2;
    sigma = std::clamp(sigma, 1e-4, 5.0);

    // Bracket for the bisection fallback.
    double lo = 1e-6, hi = 10.0;

    for (int i = 0; i < max_iter; ++i) {
        const double v = vega_at(sigma);
        const double diff = price_at(sigma) - target;

        if (std::fabs(diff) < tol) {
            return sigma;
        }
        // Maintain the bracket as Newton explores.
        if (diff > 0.0)
            hi = sigma;
        else
            lo = sigma;

        double next;
        if (v > 1e-12) {
            next = sigma - diff / v;  // Newton step
        } else {
            next = 0.5 * (lo + hi);  // vega vanished -> bisect
        }
        // If Newton jumps outside the bracket, fall back to bisection.
        if (!(next > lo && next < hi) || !std::isfinite(next)) {
            next = 0.5 * (lo + hi);
        }
        sigma = next;
    }

    // No convergence. This happens when the option price is effectively flat
    // in vol — deep in/out-of-the-money, where the price equals intrinsic (or
    // zero) to machine precision for a whole range of vols. The implied vol is
    // then genuinely undefined, so we report no solution rather than invent a
    // number. Callers (the smile chart) simply skip the strike.
    return std::nullopt;
}

}  // namespace mape

#endif  // MAPE_IMPLIED_VOL_HPP
