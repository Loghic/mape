#ifndef MAPE_MARKET_TYPES_HPP
#define MAPE_MARKET_TYPES_HPP

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

// Market value types (plan §16.2): small, dependency-free abstractions that
// replace bare doubles where a term structure or a surface is meaningful.
// These are the foundation calibration (§16.3) and bucketed risk (§16.4) build
// on. They degrade gracefully to the flat-scalar case, so existing code that
// passes a single rate / vol keeps working unchanged (see market_data.hpp).

namespace mape {

// A continuously-compounded discount curve. Constructed either flat (one rate
// for all maturities) or from (maturity, zero-rate) pivots with linear
// interpolation on the zero rate. `rate_at(T)` returns the zero rate; the
// discount factor is exp(-rate_at(T) * T).
class YieldCurve {
public:
    YieldCurve() = default;

    // Flat curve: a single rate at all maturities.
    static YieldCurve flat(double rate) {
        YieldCurve c;
        c.flat_ = rate;
        return c;
    }

    // Term structure from sorted (maturity, zero-rate) pivots. Assumes the
    // input is sorted by maturity (the common bootstrap output); we sort
    // defensively anyway.
    static YieldCurve from_pivots(std::vector<double> maturities,
                                  std::vector<double> rates) {
        YieldCurve c;
        const std::size_t n = std::min(maturities.size(), rates.size());
        maturities.resize(n);
        rates.resize(n);
        c.t_ = std::move(maturities);
        c.r_ = std::move(rates);
        // Sort the (t, r) pairs by t.
        for (std::size_t i = 0; i < c.t_.size(); ++i)
            for (std::size_t j = i + 1; j < c.t_.size(); ++j)
                if (c.t_[j] < c.t_[i]) {
                    std::swap(c.t_[i], c.t_[j]);
                    std::swap(c.r_[i], c.r_[j]);
                }
        return c;
    }

    // Zero rate at maturity T (flat outside the pivot range — no extrapolation
    // slope, just clamp to the nearest pivot).
    double rate_at(double T) const {
        if (t_.empty()) return flat_;
        if (T <= t_.front()) return r_.front();
        if (T >= t_.back()) return r_.back();
        // Linear interpolation between bracketing pivots.
        for (std::size_t i = 1; i < t_.size(); ++i) {
            if (T <= t_[i]) {
                const double w = (T - t_[i - 1]) / (t_[i] - t_[i - 1]);
                return r_[i - 1] + w * (r_[i] - r_[i - 1]);
            }
        }
        return r_.back();
    }

    double discount(double T) const { return std::exp(-rate_at(T) * T); }

    bool is_flat() const noexcept { return t_.empty(); }

private:
    double flat_ = 0.0;
    std::vector<double> t_;  // maturities (sorted)
    std::vector<double> r_;  // zero rates
};

// A volatility surface. Flat (one vol for all strikes/maturities) or a function
// of strike (a smile/skew) via interpolation over strike pivots at a single
// maturity slice — enough to back the implied-vol smile (§16.3) without a full
// 2-D surface. `vol_at(strike, maturity)` is the lookup the models call.
class VolSurface {
public:
    VolSurface() = default;

    static VolSurface flat(double vol) {
        VolSurface s;
        s.flat_ = vol;
        return s;
    }

    // A smile: (strike, vol) pivots at one maturity (or maturity-independent).
    static VolSurface smile(std::vector<double> strikes,
                            std::vector<double> vols) {
        VolSurface s;
        const std::size_t n = std::min(strikes.size(), vols.size());
        strikes.resize(n);
        vols.resize(n);
        s.k_ = std::move(strikes);
        s.v_ = std::move(vols);
        for (std::size_t i = 0; i < s.k_.size(); ++i)
            for (std::size_t j = i + 1; j < s.k_.size(); ++j)
                if (s.k_[j] < s.k_[i]) {
                    std::swap(s.k_[i], s.k_[j]);
                    std::swap(s.v_[i], s.v_[j]);
                }
        return s;
    }

    // Vol at (strike, maturity). Maturity is accepted for interface
    // completeness; the current implementation is a strike smile (flat in T).
    double vol_at(double strike, double /*maturity*/ = 0.0) const {
        if (k_.empty()) return flat_;
        if (strike <= k_.front()) return v_.front();
        if (strike >= k_.back()) return v_.back();
        for (std::size_t i = 1; i < k_.size(); ++i) {
            if (strike <= k_[i]) {
                const double w = (strike - k_[i - 1]) / (k_[i] - k_[i - 1]);
                return v_[i - 1] + w * (v_[i] - v_[i - 1]);
            }
        }
        return v_.back();
    }

    bool is_flat() const noexcept { return k_.empty(); }

private:
    double flat_ = 0.0;
    std::vector<double> k_;  // strikes (sorted)
    std::vector<double> v_;  // vols
};

// A spot quote — a thin wrapper so a spot can carry provenance later (currency,
// timestamp) without changing call sites. For now it's value-only.
struct SpotQuote {
    double value = 0.0;
    constexpr SpotQuote() = default;
    constexpr SpotQuote(double v) : value(v) {}
    constexpr operator double() const { return value; }
};

}  // namespace mape

#endif  // MAPE_MARKET_TYPES_HPP
