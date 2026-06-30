#ifndef MAPE_PRICING_RESULT_HPP
#define MAPE_PRICING_RESULT_HPP

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "mape/concepts.hpp"
#include "mape/models/monte_carlo.hpp"

namespace mape {

// A richer pricing result (plan §16.6): the price plus optional diagnostics.
//
// The fields that are only meaningful for a *stochastic* method — standard
// error, path count — are std::optional, consistent with the project's "no
// fabricated number" rule: a closed-form Black-Scholes price has no Monte Carlo
// standard error, so we report std::nullopt rather than a fake 0. Callers (the
// benchmark harness, the GUI) read what's present.
struct PricingResult {
    double price = 0.0;
    std::string_view model = "";       // which model produced it
    std::optional<std::size_t> paths;  // MC only
    std::optional<double> std_error;   // MC only: standard error of the mean
    std::optional<unsigned> threads;   // parallel runs only

    // Convenience: a 95%-ish confidence half-width (1.96 * std_error), when an
    // error estimate exists.
    std::optional<double> confidence_95() const {
        if (std_error) return 1.96 * *std_error;
        return std::nullopt;
    }
};

// Monte Carlo with diagnostics: returns the price AND its standard error.
// The standard error of the mean is s / sqrt(N), where s is the sample standard
// deviation of the per-path discounted payoffs (computed in one pass via the
// running sum and sum-of-squares).
template <StochasticProcess Process, Payoff Pay>
PricingResult monte_carlo_result(const Process& process, const Pay& payoff,
                                 std::size_t paths, double discount,
                                 std::uint64_t seed = 12345ULL) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> norm(0.0, 1.0);
    double sum = 0.0;
    double sum_sq = 0.0;
    for (std::size_t i = 0; i < paths; ++i) {
        const double x = discount * payoff(process.terminal(norm(rng)));
        sum += x;
        sum_sq += x * x;
    }
    const double n = static_cast<double>(paths);
    const double mean = paths ? sum / n : 0.0;
    // Sample variance (unbiased) of the discounted payoff, then SE = s/sqrt(N).
    double std_err = 0.0;
    if (paths > 1) {
        const double var = (sum_sq - n * mean * mean) / (n - 1.0);
        std_err = std::sqrt(std::max(var, 0.0) / n);
    }
    PricingResult r;
    r.price = mean;
    r.model = "monte_carlo";
    r.paths = paths;
    r.std_error = std_err;
    return r;
}

}  // namespace mape

#endif  // MAPE_PRICING_RESULT_HPP
