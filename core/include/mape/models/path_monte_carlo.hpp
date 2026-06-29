#ifndef MAPE_MODELS_PATH_MONTE_CARLO_HPP
#define MAPE_MODELS_PATH_MONTE_CARLO_HPP

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <span>
#include <vector>

#include "mape/concepts.hpp"
#include "mape/market_data.hpp"

namespace mape {

// GBM path generator: walks a spot forward over `steps` equal time slices,
// filling a buffer with the spot at each step (excluding t=0, including
// maturity at the end). One reusable buffer avoids per-path allocation.
class GbmPathGenerator {
public:
    GbmPathGenerator(const MarketData& mkt, double maturity, std::size_t steps)
        : steps_(steps), spot0_(mkt.spot) {
        const double dt = maturity / static_cast<double>(steps);
        drift_ = (mkt.rate - mkt.dividend - 0.5 * mkt.vol * mkt.vol) * dt;
        diffusion_ = mkt.vol * std::sqrt(dt);
    }

    std::size_t steps() const noexcept { return steps_; }

    // Fill `out` (length == steps()) with one simulated path.
    template <typename Rng>
    void sample(Rng& rng, std::vector<double>& out) const {
        std::normal_distribution<double> norm(0.0, 1.0);
        out.resize(steps_);
        double s = spot0_;
        for (std::size_t i = 0; i < steps_; ++i) {
            s *= std::exp(drift_ + diffusion_ * norm(rng));
            out[i] = s;
        }
    }

private:
    std::size_t steps_;
    double      spot0_;
    double      drift_;
    double      diffusion_;
};

// Single-threaded path Monte Carlo: average a PathPayoff over simulated GBM
// paths, then discount. The payoff is any callable satisfying PathPayoff
// (Asian, barrier, lookback, ...).
template <PathPayoff Pay>
double monte_carlo_path_price(const GbmPathGenerator& gen, const Pay& payoff,
                              std::size_t paths, double discount,
                              std::uint64_t seed = 12345ULL) {
    std::mt19937_64 rng(seed);
    std::vector<double> path;
    path.reserve(gen.steps());
    double sum = 0.0;
    for (std::size_t i = 0; i < paths; ++i) {
        gen.sample(rng, path);
        sum += payoff(std::span<const double>(path));
    }
    return discount * (sum / static_cast<double>(paths));
}

}  // namespace mape

#endif  // MAPE_MODELS_PATH_MONTE_CARLO_HPP
