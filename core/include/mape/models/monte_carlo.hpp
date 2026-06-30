#ifndef MAPE_MODELS_MONTE_CARLO_HPP
#define MAPE_MODELS_MONTE_CARLO_HPP

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>

#include "mape/concepts.hpp"
#include "mape/instruments.hpp"
#include "mape/market_data.hpp"

namespace mape {

// Geometric Brownian Motion: maps a single unit-normal draw to a terminal spot
// under the risk-neutral measure. Satisfies the StochasticProcess concept.
struct GbmProcess {
    double spot;
    double drift;   // (r - q - 0.5*sigma^2) * T
    double diffusion;  // sigma * sqrt(T)

    static GbmProcess from_market(const MarketData& mkt, double T) {
        return GbmProcess{
            mkt.spot,
            (mkt.rate - mkt.dividend - 0.5 * mkt.vol * mkt.vol) * T,
            mkt.vol * std::sqrt(T)};
    }

    double terminal(double z) const {
        return spot * std::exp(drift + diffusion * z);
    }
};

// Templated Monte Carlo core (plan §5.1/§5.2): parameterised on the stochastic
// process and the payoff. A new payoff is just a small callable. This is the
// single-threaded reference; the parallel version lives in threading/.
template <StochasticProcess Process, Payoff Pay>
double monte_carlo_price(const Process& process, const Pay& payoff,
                         std::size_t paths, double discount,
                         std::uint64_t seed = 12345ULL) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> norm(0.0, 1.0);
    double sum = 0.0;
    for (std::size_t i = 0; i < paths; ++i) {
        sum += payoff(process.terminal(norm(rng)));
    }
    return discount * (sum / static_cast<double>(paths));
}

// Antithetic-variates Monte Carlo (plan §14.2). For each standard-normal draw
// Z it also uses -Z, averaging the two payoffs. Because the two paths are
// negatively correlated for monotone payoffs, the variance of the estimator
// drops (often ~halves) at the same number of RNG draws. `pairs` is the number
// of antithetic *pairs*, so the effective sample size is 2*pairs.
template <StochasticProcess Process, Payoff Pay>
double monte_carlo_price_antithetic(const Process& process, const Pay& payoff,
                                    std::size_t pairs, double discount,
                                    std::uint64_t seed = 12345ULL) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> norm(0.0, 1.0);
    double sum = 0.0;
    for (std::size_t i = 0; i < pairs; ++i) {
        const double z = norm(rng);
        sum += 0.5 * (payoff(process.terminal(z)) +
                      payoff(process.terminal(-z)));
    }
    return discount * (sum / static_cast<double>(pairs));
}

// Model wrapper so MonteCarlo plugs into Pricer<MonteCarlo> like the others.
class MonteCarlo {
public:
    explicit MonteCarlo(std::size_t paths = 200000, std::uint64_t seed = 12345ULL)
        : paths_(paths), seed_(seed) {}

    double price(const Option& opt, const MarketData& mkt) const {
        const auto process = GbmProcess::from_market(mkt, opt.maturity);
        const double discount = std::exp(-mkt.rate * opt.maturity);
        return monte_carlo_price(process, opt.payoff(), paths_, discount, seed_);
    }

    std::size_t   paths() const noexcept { return paths_; }
    std::uint64_t seed()  const noexcept { return seed_; }

private:
    std::size_t   paths_;
    std::uint64_t seed_;
};

}  // namespace mape

#endif  // MAPE_MODELS_MONTE_CARLO_HPP
