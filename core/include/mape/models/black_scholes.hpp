#ifndef MAPE_MODELS_BLACK_SCHOLES_HPP
#define MAPE_MODELS_BLACK_SCHOLES_HPP

#include <algorithm>
#include <cmath>
#include <utility>

#include "mape/instruments.hpp"
#include "mape/market_data.hpp"

namespace mape {

// Standard normal CDF via erfc (no <numbers> dependency needed for this).
inline double norm_cdf(double x) {
    return 0.5 * std::erfc(-x * M_SQRT1_2);
}

// Standard normal PDF.
inline double norm_pdf(double x) {
    static const double inv_sqrt_2pi = 0.3989422804014327;
    return inv_sqrt_2pi * std::exp(-0.5 * x * x);
}

// Black–Scholes–Merton closed form (with continuous dividend yield q).
// This is the analytical benchmark every other model is validated against
// (plan §11). Satisfies the PricingModel concept.
class BlackScholes {
public:
    double price(const Option& opt, const MarketData& mkt) const {
        const double S = mkt.spot, K = opt.strike, r = mkt.rate;
        const double q = mkt.dividend, sigma = mkt.vol, T = opt.maturity;

        // Degenerate: zero time or vol -> discounted intrinsic on the forward.
        if (T <= 0.0 || sigma <= 0.0) {
            const double fwd = S * std::exp((r - q) * T);
            const double disc = std::exp(-r * T);
            const double intrinsic = opt.type == OptionType::Call
                                         ? std::max(fwd - K, 0.0)
                                         : std::max(K - fwd, 0.0);
            return disc * intrinsic;
        }

        const double d1 = (std::log(S / K) + (r - q + 0.5 * sigma * sigma) * T) /
                          (sigma * std::sqrt(T));
        const double d2 = d1 - sigma * std::sqrt(T);
        const double df_q = std::exp(-q * T);
        const double df_r = std::exp(-r * T);

        if (opt.type == OptionType::Call)
            return S * df_q * norm_cdf(d1) - K * df_r * norm_cdf(d2);
        return K * df_r * norm_cdf(-d2) - S * df_q * norm_cdf(-d1);
    }

    // --- Closed-form Greeks (per unit, not bumped) ----------------------
    double delta(const Option& opt, const MarketData& mkt) const {
        const double d1 = d1d2(opt, mkt).first;
        const double df_q = std::exp(-mkt.dividend * opt.maturity);
        return opt.type == OptionType::Call ? df_q * norm_cdf(d1)
                                            : df_q * (norm_cdf(d1) - 1.0);
    }

    double gamma(const Option& opt, const MarketData& mkt) const {
        const double d1 = d1d2(opt, mkt).first;
        const double df_q = std::exp(-mkt.dividend * opt.maturity);
        return df_q * norm_pdf(d1) /
               (mkt.spot * mkt.vol * std::sqrt(opt.maturity));
    }

    double vega(const Option& opt, const MarketData& mkt) const {
        const double d1 = d1d2(opt, mkt).first;
        const double df_q = std::exp(-mkt.dividend * opt.maturity);
        return mkt.spot * df_q * norm_pdf(d1) * std::sqrt(opt.maturity);
    }

private:
    std::pair<double, double> d1d2(const Option& opt, const MarketData& mkt) const {
        const double S = mkt.spot, K = opt.strike, r = mkt.rate;
        const double q = mkt.dividend, sigma = mkt.vol, T = opt.maturity;
        const double d1 = (std::log(S / K) + (r - q + 0.5 * sigma * sigma) * T) /
                          (sigma * std::sqrt(T));
        return {d1, d1 - sigma * std::sqrt(T)};
    }
};

}  // namespace mape

#endif  // MAPE_MODELS_BLACK_SCHOLES_HPP
