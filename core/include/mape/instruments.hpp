#ifndef MAPE_INSTRUMENTS_HPP
#define MAPE_INSTRUMENTS_HPP

#include <algorithm>
#include <variant>

namespace mape {

enum class OptionType { Call, Put };
enum class Exercise   { European, American };

// --- Payoff callables ---------------------------------------------------
// A Payoff maps a terminal spot to a cashflow. These are tiny callables so a
// new payoff is a few lines (plan §5.1). They satisfy the `Payoff` concept.

struct CallPayoff {
    double strike;
    double operator()(double spot) const { return std::max(spot - strike, 0.0); }
};

struct PutPayoff {
    double strike;
    double operator()(double spot) const { return std::max(strike - spot, 0.0); }
};

// Type-erased vanilla payoff selected at runtime from an OptionType.
struct VanillaPayoff {
    OptionType type;
    double     strike;
    double operator()(double spot) const {
        return type == OptionType::Call ? std::max(spot - strike, 0.0)
                                        : std::max(strike - spot, 0.0);
    }
};

// --- Instruments --------------------------------------------------------

struct Option {
    OptionType type     = OptionType::Call;
    Exercise   exercise = Exercise::European;
    double     strike   = 0.0;
    double     maturity = 0.0;  // year fraction to expiry

    VanillaPayoff payoff() const { return {type, strike}; }
};

// Fixed-coupon vanilla bond. Coupons paid `frequency` times per year.
struct Bond {
    double face       = 100.0;
    double coupon     = 0.0;    // annual coupon rate (e.g. 0.05 = 5%)
    double maturity   = 0.0;    // years
    int    frequency  = 2;      // coupons per year
};

// FX forward: agree today to exchange at `strike` at maturity.
struct FxForward {
    double strike       = 0.0;  // agreed forward rate K
    double maturity     = 0.0;  // years
    double foreign_rate = 0.0;  // foreign risk-free rate r_f
};

// Heterogeneous instrument set (plan §5.3). Lets a portfolio hold a mix.
using Instrument = std::variant<Option, Bond, FxForward>;

}  // namespace mape

#endif  // MAPE_INSTRUMENTS_HPP
