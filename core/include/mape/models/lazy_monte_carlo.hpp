#ifndef MAPE_MODELS_LAZY_MONTE_CARLO_HPP
#define MAPE_MODELS_LAZY_MONTE_CARLO_HPP

#include <cstddef>
#include <cstdint>
#include <random>

#include "mape/concepts.hpp"
#include "mape/generator.hpp"

namespace mape {

// Lazy Monte Carlo payoff stream (plan §15.4). A coroutine that yields the
// payoff of one simulated terminal value at a time via `co_yield`, so the
// caller can consume an arbitrarily large number of paths in O(1) memory — no
// vector of payoffs is ever materialised. Because `generator<double>` exposes a
// standard input iterator, the result composes with range-based for and with
// std::ranges / views.
//
// Note: the RNG and distribution are captured by value into the coroutine frame
// so they live for the stream's lifetime (a dangling reference would be a
// classic coroutine bug).
template <StochasticProcess Process, Payoff Pay>
generator<double> mc_payoff_stream(Process process, Pay payoff,
                                   std::size_t paths, double discount,
                                   std::uint64_t seed = 12345ULL) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> norm(0.0, 1.0);
    for (std::size_t i = 0; i < paths; ++i) {
        co_yield discount * payoff(process.terminal(norm(rng)));
    }
}

// Convenience: mean of a lazily-streamed payoff sequence. Demonstrates that the
// streaming form gives the same estimate as the eager monte_carlo_price while
// never holding more than one payoff at a time.
template <StochasticProcess Process, Payoff Pay>
double monte_carlo_price_lazy(const Process& process, const Pay& payoff,
                              std::size_t paths, double discount,
                              std::uint64_t seed = 12345ULL) {
    double sum = 0.0;
    std::size_t n = 0;
    for (double disc_payoff :
         mc_payoff_stream(process, payoff, paths, discount, seed)) {
        sum += disc_payoff;
        ++n;
    }
    return n == 0 ? 0.0 : sum / static_cast<double>(n);
}

}  // namespace mape

#endif  // MAPE_MODELS_LAZY_MONTE_CARLO_HPP
