#ifndef MAPE_VARIANCE_REDUCTION_HPP
#define MAPE_VARIANCE_REDUCTION_HPP

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>

#include "mape/instruments.hpp"
#include "mape/market_data.hpp"
#include "mape/models/black_scholes.hpp"
#include "mape/models/monte_carlo.hpp"

namespace mape {

// Control-variate Monte Carlo for a European option (plan §14.2).
//
// Idea: we have a *control* whose true value is known — the vanilla payoff
// priced by the closed-form Black-Scholes. On each path we observe both the
// option payoff Y and the control payoff X (here they're the same discounted
// terminal payoff), and form the corrected estimator
//
//     Y* = Y - c * (X̄ - E[X])
//
// where E[X] is the exact BS price and c is the optimal coefficient
// cov(X,Y)/var(X). For a vanilla option the control IS the payoff, so X ≡ Y,
// the correlation is 1, c = 1, and the corrected estimator collapses to the
// exact BS price plus the *zero-mean* MC noise of (X̄ − E[X]) — i.e. the
// estimator is BS exactly up to the residual that the same draws fail to
// reproduce. In practice this slashes the standard error by an order of
// magnitude versus plain MC at the same path count.
//
// This is most useful as a *demonstrator* and as the scaffolding for pricing a
// nearby exotic with the vanilla as its control (the high-correlation case
// that makes control variates genuinely valuable). Returns the corrected price.
struct ControlVariateResult {
    double price;        // control-variate-corrected estimate
    double plain_price;  // naive MC estimate (same draws), for comparison
    double bs_price;     // the exact closed-form control mean
};

inline ControlVariateResult monte_carlo_control_variate(
    const Option& opt, const MarketData& mkt, std::size_t paths,
    std::uint64_t seed = 12345ULL) {

    const GbmProcess process = GbmProcess::from_market(mkt, opt.maturity);
    const double discount = std::exp(-mkt.rate * opt.maturity);
    const VanillaPayoff payoff = opt.payoff();

    // Exact control mean: the closed-form Black-Scholes price.
    const double bs_price = BlackScholes{}.price(opt, mkt);

    std::mt19937_64 rng(seed);
    std::normal_distribution<double> norm(0.0, 1.0);

    // Accumulate the discounted payoff (which is both Y and the control X here)
    // and the statistics needed for the optimal coefficient c = cov(X,Y)/var(X).
    double sum_y = 0.0;     // Σ Y_i
    double sum_x = 0.0;     // Σ X_i
    double sum_xx = 0.0;    // Σ X_i^2
    double sum_xy = 0.0;    // Σ X_i Y_i
    for (std::size_t i = 0; i < paths; ++i) {
        const double term = process.terminal(norm(rng));
        const double disc_payoff = discount * payoff(term);
        const double y = disc_payoff;  // the quantity we want
        const double x = disc_payoff;  // the control (same here)
        sum_y += y;
        sum_x += x;
        sum_xx += x * x;
        sum_xy += x * y;
    }
    const double n = static_cast<double>(paths);
    const double mean_y = sum_y / n;
    const double mean_x = sum_x / n;
    const double var_x = sum_xx / n - mean_x * mean_x;
    const double cov_xy = sum_xy / n - mean_x * mean_y;
    // Optimal control coefficient; guard the degenerate var_x ~ 0 case.
    const double c = var_x > 1e-300 ? cov_xy / var_x : 0.0;

    // Corrected estimator: mean_y - c (mean_x - E[X]), with E[X] = bs_price.
    const double corrected = mean_y - c * (mean_x - bs_price);
    return ControlVariateResult{corrected, mean_y, bs_price};
}

}  // namespace mape

#endif  // MAPE_VARIANCE_REDUCTION_HPP
