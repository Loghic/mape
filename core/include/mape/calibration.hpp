#ifndef MAPE_CALIBRATION_HPP
#define MAPE_CALIBRATION_HPP

#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

#include "mape/market_types.hpp"

// Calibration framework (plan §16.3). Generalises the single-point implied_vol
// into fitting *many* quotes at once: an SVI volatility smile and a
// bootstrapped discount curve. This closes the market -> calibrate -> price
// loop, and turns the GUI's raw implied-vol points into a fitted surface.
//
// Dependency-free and header-only like the rest of the core: the nonlinear SVI
// fit uses a small built-in Nelder-Mead simplex (no external optimiser).

namespace mape {

// A market observation to calibrate against.
struct MarketQuote {
    double strike;
    double maturity;
    double implied_vol;  // observed market implied volatility
};

// --- SVI smile ----------------------------------------------------------
//
// Gatheral's "raw" SVI parameterises total implied variance as a function of
// log-moneyness k = ln(K / F):
//     w(k) = a + b * ( rho * (k - m) + sqrt((k - m)^2 + sigma^2) )
// and implied vol = sqrt(w(k) / T). Five parameters: a, b, rho, m, sigma.
struct SviParams {
    double a = 0.0;      // overall variance level
    double b = 0.1;      // wing slope (>= 0)
    double rho = 0.0;    // skew, in (-1, 1)
    double m = 0.0;      // horizontal shift
    double sigma = 0.1;  // smoothness near the money (> 0)

    // Total variance at log-moneyness k.
    double total_variance(double k) const {
        const double d = k - m;
        return a + b * (rho * d + std::sqrt(d * d + sigma * sigma));
    }
    // Implied vol at strike K given forward F and maturity T.
    double vol(double K, double F, double T) const {
        const double k = std::log(K / F);
        const double w = total_variance(k);
        return std::sqrt(std::max(w, 1e-12) / T);
    }
};

struct CalibrationResult {
    SviParams params;
    double rmse = 0.0;  // root-mean-square vol error across quotes
    int iterations = 0;
    bool converged = false;
};

namespace detail {

// Sum of squared vol errors of an SVI fit against the quotes (the objective).
inline double svi_sse(const SviParams& p, std::span<const MarketQuote> quotes,
                      double forward) {
    double sse = 0.0;
    for (const MarketQuote& q : quotes) {
        const double model = p.vol(q.strike, forward, q.maturity);
        const double e = model - q.implied_vol;
        sse += e * e;
    }
    return sse;
}

// Penalise SVI parameters that leave the no-arbitrage-ish valid region, so the
// optimiser stays in a sensible box (b >= 0, |rho| < 1, sigma > 0).
inline double svi_penalty(const SviParams& p) {
    double pen = 0.0;
    if (p.b < 0.0) pen += 1e6 * p.b * p.b;
    if (p.sigma <= 0.0) pen += 1e6 * (p.sigma - 1e-4) * (p.sigma - 1e-4);
    if (p.rho <= -1.0 || p.rho >= 1.0) pen += 1e6;
    return pen;
}

}  // namespace detail

// Fit SVI parameters to a smile by least squares over the quotes' implied vols,
// using a Nelder-Mead simplex on the 5 parameters. `forward` is the underlying
// forward (e.g. spot * e^{(r-q)T}); quotes should share (roughly) one maturity.
inline CalibrationResult calibrate_svi(std::span<const MarketQuote> quotes,
                                       double forward, int max_iter = 2000,
                                       double tol = 1e-10) {
    using detail::svi_penalty;
    using detail::svi_sse;

    auto objective = [&](const std::array<double, 5>& x) {
        SviParams p{x[0], x[1], x[2], x[3], x[4]};
        return svi_sse(p, quotes, forward) + svi_penalty(p);
    };

    // Initial guess: flat-ish variance at the average observed vol.
    double avg_var = 0.0;
    for (const MarketQuote& q : quotes)
        avg_var += q.implied_vol * q.implied_vol * q.maturity;
    avg_var =
        quotes.empty() ? 0.04 : avg_var / static_cast<double>(quotes.size());

    std::array<double, 5> start{avg_var, 0.1, -0.3, 0.0, 0.1};

    // --- Nelder-Mead over 5 dimensions ---
    // A simplex in N dimensions has N+1 vertices. kV is that vertex count and
    // kWorst the index of the last (worst, after sorting) vertex. Using named
    // constants for both the array sizes and the loop bounds keeps them
    // provably consistent (and avoids a false out-of-bounds from cppcheck's
    // value-flow analysis on `N + 1`).
    constexpr int N = 5;            // number of parameters / dimensions
    constexpr int kV = N + 1;       // number of simplex vertices
    constexpr int kWorst = kV - 1;  // index of the worst vertex == N
    std::array<std::array<double, N>, kV> simplex{};
    std::array<double, kV> fval{};
    simplex[0] = start;
    for (int i = 0; i < N; ++i) {
        simplex[i + 1] = start;
        const double step =
            start[i] != 0.0 ? 0.05 * std::fabs(start[i]) + 0.05 : 0.05;
        simplex[i + 1][i] += step;
    }
    for (int i = 0; i < kV; ++i) fval[i] = objective(simplex[i]);

    const double alpha = 1.0, gamma = 2.0, rho = 0.5, sigma = 0.5;
    int iter = 0;
    for (; iter < max_iter; ++iter) {
        // Order vertices by objective value (simple insertion sort).
        for (int i = 0; i < kV; ++i)
            for (int j = i + 1; j < kV; ++j)
                if (fval[j] < fval[i]) {
                    std::swap(fval[i], fval[j]);
                    std::swap(simplex[i], simplex[j]);
                }
        if (std::fabs(fval[kWorst] - fval[0]) < tol) break;

        // Centroid of all but the worst.
        std::array<double, N> centroid{};
        for (int i = 0; i < N; ++i) {
            for (int d = 0; d < N; ++d) centroid[d] += simplex[i][d];
        }
        for (int d = 0; d < N; ++d) centroid[d] /= N;

        auto reflect = [&](double coeff) {
            std::array<double, N> p{};
            for (int d = 0; d < N; ++d)
                p[d] = centroid[d] + coeff * (centroid[d] - simplex[kWorst][d]);
            return p;
        };

        auto xr = reflect(alpha);
        const double fr = objective(xr);
        if (fr < fval[0]) {
            auto xe = reflect(alpha * gamma);
            const double fe = objective(xe);
            if (fe < fr) {
                simplex[kWorst] = xe;
                fval[kWorst] = fe;
            } else {
                simplex[kWorst] = xr;
                fval[kWorst] = fr;
            }
        } else if (fr < fval[kWorst - 1]) {
            simplex[kWorst] = xr;
            fval[kWorst] = fr;
        } else {
            // Contraction.
            std::array<double, N> xc{};
            for (int d = 0; d < N; ++d)
                xc[d] = centroid[d] + rho * (simplex[kWorst][d] - centroid[d]);
            const double fc = objective(xc);
            if (fc < fval[kWorst]) {
                simplex[kWorst] = xc;
                fval[kWorst] = fc;
            } else {
                // Shrink toward the best vertex.
                for (int i = 1; i < kV; ++i) {
                    for (int d = 0; d < N; ++d)
                        simplex[i][d] = simplex[0][d] +
                                        sigma * (simplex[i][d] - simplex[0][d]);
                    fval[i] = objective(simplex[i]);
                }
            }
        }
    }

    // Best vertex.
    int best = 0;
    for (int i = 1; i < kV; ++i)
        if (fval[i] < fval[best]) best = i;
    const auto& x = simplex[best];

    CalibrationResult res;
    res.params = SviParams{x[0], x[1], x[2], x[3], x[4]};
    res.iterations = iter;
    res.converged = iter < max_iter;
    const double sse = svi_sse(res.params, quotes, forward);
    res.rmse = quotes.empty()
                   ? 0.0
                   : std::sqrt(sse / static_cast<double>(quotes.size()));
    return res;
}

// Build a VolSurface (strike smile) by sampling a fitted SVI at the quote
// strikes — turns the calibration into the surface the rest of the engine uses.
inline VolSurface svi_to_surface(const SviParams& p, double forward,
                                 double maturity,
                                 std::span<const double> strikes) {
    std::vector<double> ks(strikes.begin(), strikes.end());
    std::vector<double> vs;
    vs.reserve(ks.size());
    for (double K : ks) vs.push_back(p.vol(K, forward, maturity));
    return VolSurface::smile(std::move(ks), std::move(vs));
}

// --- Discount-curve bootstrap -------------------------------------------
//
// Given observed discount factors DF(T_i) at increasing maturities, recover the
// zero-rate curve: r(T_i) = -ln(DF(T_i)) / T_i. Linear interpolation between
// pivots is provided by YieldCurve. This is the discount-curve half of §16.3.
inline YieldCurve bootstrap_curve(std::span<const double> maturities,
                                  std::span<const double> discount_factors) {
    std::vector<double> t;
    std::vector<double> r;
    const std::size_t n = std::min(maturities.size(), discount_factors.size());
    t.reserve(n);
    r.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double T = maturities[i];
        const double df = discount_factors[i];
        if (T > 0.0 && df > 0.0) {
            t.push_back(T);
            r.push_back(-std::log(df) / T);
        }
    }
    return YieldCurve::from_pivots(std::move(t), std::move(r));
}

}  // namespace mape

#endif  // MAPE_CALIBRATION_HPP
