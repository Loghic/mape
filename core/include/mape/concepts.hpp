#ifndef MAPE_CONCEPTS_HPP
#define MAPE_CONCEPTS_HPP

#include <concepts>
#include <span>

#include "mape/instruments.hpp"
#include "mape/market_data.hpp"

namespace mape {

// A PricingModel can value an Option against a MarketData snapshot and return
// something convertible to a price. Constraining the engine this way turns
// misuse into a compile error rather than a runtime surprise (plan §5.1).
template <typename T>
concept PricingModel =
    requires(const T m, const Option& opt, const MarketData& mkt) {
        { m.price(opt, mkt) } -> std::convertible_to<double>;
    };

// A Payoff maps a terminal spot to a cashflow.
template <typename T>
concept Payoff = requires(const T p, double spot) {
    { p(spot) } -> std::convertible_to<double>;
};

// A Process can evolve a spot to a terminal value given a draw from a unit
// normal source. Used by the templated Monte Carlo engine (plan §5.2).
template <typename T>
concept StochasticProcess = requires(const T proc, double normal_draw) {
    { proc.terminal(normal_draw) } -> std::convertible_to<double>;
};

// A PathPayoff maps a whole price path (a span of spots, one per time step) to
// a cashflow. This is what lets exotic, path-dependent products (Asian,
// barrier, lookback) drop in as small callables (plan §12).
template <typename T>
concept PathPayoff = requires(const T p, std::span<const double> path) {
    { p(path) } -> std::convertible_to<double>;
};

}  // namespace mape

#endif  // MAPE_CONCEPTS_HPP
