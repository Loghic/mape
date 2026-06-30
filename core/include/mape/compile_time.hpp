#ifndef MAPE_COMPILE_TIME_HPP
#define MAPE_COMPILE_TIME_HPP

#include "mape/ct_math.hpp"
#include "mape/instruments.hpp"
#include "mape/models/black_scholes_ad.hpp"

// Compile-time pricing surface (plan §5.4). These wrappers let closed-form
// prices, discount factors, and contract validation happen inside constant
// expressions — so a class of regression tests becomes static_asserts that
// cost nothing at runtime and can never silently rot.

namespace mape::ct {

// --- constexpr Black-Scholes -------------------------------------------
//
// Reuses the SAME bs_price_generic the runtime model and the AD Greeks use —
// no parallel copy of the formula. Instantiating it on CtDouble makes every
// transcendental resolve to the constexpr overloads in ct_math.hpp.
constexpr double bs_price(OptionType type, double spot, double strike,
                          double rate, double dividend, double vol,
                          double maturity) {
    const CtDouble px = bs_price_generic<CtDouble>(
        type, CtDouble(spot), CtDouble(strike), CtDouble(rate),
        CtDouble(dividend), CtDouble(vol), CtDouble(maturity));
    return static_cast<double>(px);
}

constexpr double bs_call(double spot, double strike, double rate, double vol,
                         double maturity, double dividend = 0.0) {
    return bs_price(OptionType::Call, spot, strike, rate, dividend, vol,
                    maturity);
}

constexpr double bs_put(double spot, double strike, double rate, double vol,
                        double maturity, double dividend = 0.0) {
    return bs_price(OptionType::Put, spot, strike, rate, dividend, vol,
                    maturity);
}

// --- constexpr day-count / discounting ---------------------------------

// ACT/365 year fraction between two integer day counts (never negative).
constexpr double year_fraction(long start_day, long end_day) {
    const long days = end_day - start_day;
    return static_cast<double>(days > 0 ? days : 0) / 365.0;
}

// Continuously-compounded discount factor exp(-r * t), via the constexpr exp.
constexpr double discount_factor(double rate, double t) {
    return exp_ct(-rate * t);
}

// --- consteval contract validation -------------------------------------
//
// A consteval factory: guaranteed to run at compile time, so an invalid option
// *literal* (negative strike, vol, or maturity) simply fails to compile rather
// than producing a runtime error. There is no runtime fallback.
struct OptionSpec {
    OptionType type;
    double     strike;
    double     vol;
    double     maturity;
};

consteval OptionSpec make_option(OptionType type, double strike, double vol,
                                 double maturity) {
    // In a consteval context, throwing makes the call ill-formed — the compiler
    // reports it at the call site. (A throw is never reached at runtime.)
    if (strike <= 0.0) throw "strike must be positive";
    if (vol < 0.0) throw "volatility must be non-negative";
    if (maturity <= 0.0) throw "maturity must be positive";
    return OptionSpec{type, strike, vol, maturity};
}

// Price a compile-time-validated spec at compile time.
constexpr double price_spec(const OptionSpec& s, double spot, double rate,
                            double dividend = 0.0) {
    return bs_price(s.type, spot, s.strike, rate, dividend, s.vol, s.maturity);
}

}  // namespace mape::ct

#endif  // MAPE_COMPILE_TIME_HPP
