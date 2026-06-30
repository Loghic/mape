#ifndef MAPE_MODELS_BINOMIAL_HPP
#define MAPE_MODELS_BINOMIAL_HPP

#include <algorithm>
#include <cmath>
#include <vector>

#include "mape/instruments.hpp"
#include "mape/market_data.hpp"

namespace mape {

// Cox–Ross–Rubinstein binomial tree. Handles American exercise (the reason it
// exists alongside Black–Scholes) and converges to BS for European options as
// the step count grows (plan §11). Satisfies the PricingModel concept.
class BinomialTree {
public:
    explicit BinomialTree(int steps = 512) : steps_(steps) {}

    double price(const Option& opt, const MarketData& mkt) const {
        const int n = steps_;
        const double T = opt.maturity;
        const double dt = T / n;
        const double u = std::exp(mkt.vol * std::sqrt(dt));
        const double d = 1.0 / u;
        const double a = std::exp((mkt.rate - mkt.dividend) * dt);
        const double p = (a - d) / (u - d);  // risk-neutral up prob
        const double disc = std::exp(-mkt.rate * dt);

        const VanillaPayoff payoff = opt.payoff();

        // Terminal layer: spot at node j (j up-moves) = S * u^j * d^(n-j).
        std::vector<double> values(n + 1);
        for (int j = 0; j <= n; ++j) {
            const double spot = mkt.spot * std::pow(u, j) * std::pow(d, n - j);
            values[j] = payoff(spot);
        }

        // Roll back. At each node, optionally compare to early-exercise value.
        const bool american = opt.exercise == Exercise::American;
        for (int i = n - 1; i >= 0; --i) {
            for (int j = 0; j <= i; ++j) {
                values[j] = disc * (p * values[j + 1] + (1.0 - p) * values[j]);
                if (american) {
                    const double spot =
                        mkt.spot * std::pow(u, j) * std::pow(d, i - j);
                    values[j] = std::max(values[j], payoff(spot));
                }
            }
        }
        return values[0];
    }

    int steps() const noexcept { return steps_; }

private:
    int steps_;
};

}  // namespace mape

#endif  // MAPE_MODELS_BINOMIAL_HPP
