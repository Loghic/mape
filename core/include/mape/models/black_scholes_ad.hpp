#ifndef MAPE_MODELS_BLACK_SCHOLES_AD_HPP
#define MAPE_MODELS_BLACK_SCHOLES_AD_HPP

#include <cmath>

#include "mape/autodiff.hpp"
#include "mape/instruments.hpp"
#include "mape/market_data.hpp"

namespace mape {

// Scalar-generic Black-Scholes (plan §12: AD Greeks). The formula is written
// once over a scalar type `T`; instantiating with `double` gives the price,
// instantiating with `Dual` gives the price AND an exact derivative w.r.t.
// whichever input was seeded with derivative 1.
//
// ADL note: `exp`, `log`, `sqrt`, `erfc` resolve to mape::Dual overloads when
// T == Dual, and to std:: when T == double (via the using-declarations).
template <typename T>
T bs_price_generic(OptionType type, T S, T K, T r, T q, T sigma, T T_exp) {
    using std::exp;
    using std::log;
    using std::sqrt;
    using std::erfc;

    const T sqrtT = sqrt(T_exp);
    const T d1 = (log(S / K) + (r - q + T(0.5) * sigma * sigma) * T_exp) /
                 (sigma * sqrtT);
    const T d2 = d1 - sigma * sqrtT;

    // Standard normal CDF via erfc: N(x) = 0.5 * erfc(-x / sqrt(2)).
    const T inv_sqrt2 = T(0.7071067811865476);
    auto N = [&](T x) { return T(0.5) * erfc(-x * inv_sqrt2); };

    const T df_q = exp(-q * T_exp);
    const T df_r = exp(-r * T_exp);

    if (type == OptionType::Call)
        return S * df_q * N(d1) - K * df_r * N(d2);
    return K * df_r * N(-d2) - S * df_q * N(-d1);
}

// AD-derived Greeks. Each seeds one input's derivative to 1 and reads `.d`.
struct BlackScholesAD {
    double price(const Option& opt, const MarketData& mkt) const {
        return bs_price_generic<double>(opt.type, mkt.spot, opt.strike,
                                        mkt.rate, mkt.dividend, mkt.vol,
                                        opt.maturity);
    }

    // delta = d(price)/d(spot): seed S with derivative 1.
    double delta(const Option& opt, const MarketData& mkt) const {
        Dual S{mkt.spot, 1.0};
        return bs_price_generic<Dual>(opt.type, S, Dual(opt.strike),
                                      Dual(mkt.rate), Dual(mkt.dividend),
                                      Dual(mkt.vol), Dual(opt.maturity)).d;
    }

    // vega = d(price)/d(sigma): seed sigma with derivative 1.
    double vega(const Option& opt, const MarketData& mkt) const {
        Dual sigma{mkt.vol, 1.0};
        return bs_price_generic<Dual>(opt.type, Dual(mkt.spot),
                                      Dual(opt.strike), Dual(mkt.rate),
                                      Dual(mkt.dividend), sigma,
                                      Dual(opt.maturity)).d;
    }

    // rho = d(price)/d(rate): seed r with derivative 1.
    double rho(const Option& opt, const MarketData& mkt) const {
        Dual r{mkt.rate, 1.0};
        return bs_price_generic<Dual>(opt.type, Dual(mkt.spot),
                                      Dual(opt.strike), r, Dual(mkt.dividend),
                                      Dual(mkt.vol), Dual(opt.maturity)).d;
    }
};

}  // namespace mape

#endif  // MAPE_MODELS_BLACK_SCHOLES_AD_HPP
