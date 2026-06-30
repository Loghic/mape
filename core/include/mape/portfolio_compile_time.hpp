#ifndef MAPE_PORTFOLIO_COMPILE_TIME_HPP
#define MAPE_PORTFOLIO_COMPILE_TIME_HPP

#include <cstddef>
#include <tuple>
#include <utility>

#include "mape/instruments.hpp"
#include "mape/market_data.hpp"
#include "mape/models/black_scholes.hpp"
#include "mape/models/fixed_income.hpp"

namespace mape {

// Compile-time, heterogeneous multi-leg portfolio (plan §15.1). A
// `Portfolio<Insts...>` holds one leg of each given type in a std::tuple, and
// its total value folds a per-leg pricing function over the parameter pack via
// a fold expression. Leg types are checked at compile time, and a multi-leg
// strategy (spread, straddle, a bond + an option, ...) becomes one typed
// object with no runtime type tags or virtual dispatch.

// --- per-leg pricing: one overload per instrument type ------------------
// `leg_value` is the customization point; add an overload to support a new leg
// type. Each maps an instrument + market snapshot to a present value.
inline double leg_value(const Option& opt, const MarketData& mkt) {
    return BlackScholes{}.price(opt, mkt);
}
inline double leg_value(const Bond& bond, const MarketData& mkt) {
    return price_bond(bond, mkt);
}
inline double leg_value(const FxForward& fwd, const MarketData& mkt) {
    return price_fx_forward(fwd, mkt);
}

// --- the variadic portfolio ---------------------------------------------
template <typename... Legs>
class Portfolio {
public:
    explicit Portfolio(Legs... legs) : legs_(std::move(legs)...) {}

    // Total value: fold leg_value over every leg (fold expression).
    double value(const MarketData& mkt) const {
        return std::apply(
            [&](const Legs&... l) { return (leg_value(l, mkt) + ... + 0.0); },
            legs_);
    }

    // Number of legs, known at compile time.
    static constexpr std::size_t size() { return sizeof...(Legs); }

    const std::tuple<Legs...>& legs() const noexcept { return legs_; }

private:
    std::tuple<Legs...> legs_;
};

// Deduction guide so `Portfolio p{opt, bond}` infers the pack.
template <typename... Legs>
Portfolio(Legs...) -> Portfolio<Legs...>;

}  // namespace mape

#endif  // MAPE_PORTFOLIO_COMPILE_TIME_HPP
