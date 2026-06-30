#ifndef MAPE_MODELS_BLACK_SCHOLES_AD_HPP
#define MAPE_MODELS_BLACK_SCHOLES_AD_HPP

#include <cmath>

#include "mape/autodiff.hpp"
#include "mape/ct_math.hpp"
#include "mape/instruments.hpp"
#include "mape/market_data.hpp"

namespace mape {

// Scalar-generic Black-Scholes (plan §12: AD Greeks). The formula is written
// once over a scalar type `T`; instantiating with `double` gives the price,
// instantiating with `Dual` gives the price AND an exact derivative w.r.t.
// whichever input was seeded with derivative 1.
//
// ADL note: `exp`, `log`, `sqrt`, `erfc` resolve to mape::Dual overloads when
// T == Dual (automatic differentiation), to mape::ct overloads when
// T == ct::CtDouble (compile-time pricing, plan §5.4), and to std:: when
// T == double (runtime). `constexpr` lets the CtDouble instantiation fold
// inside a constant expression / static_assert.
template <typename T>
constexpr T bs_price_generic(OptionType type, T S, T K, T r, T q, T sigma, T T_exp) {
    using std::erfc;
    using std::exp;
    using std::log;
    using std::sqrt;

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

    // gamma = d^2(price)/d(spot)^2: seed S as a second-order dual (1st deriv 1,
    // 2nd deriv 0) and read the accumulated second derivative.
    double gamma(const Option& opt, const MarketData& mkt) const {
        Dual2 S{mkt.spot, 1.0, 0.0};
        return bs_price_generic<Dual2>(opt.type, S, Dual2(opt.strike),
                                       Dual2(mkt.rate), Dual2(mkt.dividend),
                                       Dual2(mkt.vol), Dual2(opt.maturity)).dd;
    }
};

}  // namespace mape

#endif  // MAPE_MODELS_BLACK_SCHOLES_AD_HPP
