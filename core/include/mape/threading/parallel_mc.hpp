#ifndef MAPE_THREADING_PARALLEL_MC_HPP
#define MAPE_THREADING_PARALLEL_MC_HPP

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <random>
#include <span>
#include <thread>
#include <vector>

#include "mape/concepts.hpp"
#include "mape/counter_rng.hpp"
#include "mape/models/path_monte_carlo.hpp"

namespace mape {

// Derive an independent RNG seed per thread. The subtlety the plan calls out
// (§5.2): each thread MUST own a disjoint random stream or the estimate is
// statistically corrupt. We mix the base seed with the thread index using a
// SplitMix64 step so nearby indices produce well-separated streams.
inline std::uint64_t seed_for(std::uint64_t base, unsigned thread_index) {
    std::uint64_t z = base + 0x9E3779B97F4A7C15ULL * (thread_index + 1);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// Sum payoff over `n` simulated terminal values using the supplied RNG.
template <StochasticProcess Process, Payoff Pay, typename Rng>
double simulate_chunk(const Process& process, const Pay& payoff,
                      std::size_t n, Rng& rng) {
    std::normal_distribution<double> norm(0.0, 1.0);
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        sum += payoff(process.terminal(norm(rng)));
    }
    return sum;
}

// Parallel Monte Carlo: split paths across threads (fan-out via std::async),
// each with an independent stream, then reduce (fan-in via future::get).
// plan §5.2. Returns the discounted mean over ALL paths.
template <StochasticProcess Process, Payoff Pay>
double monte_carlo_parallel(const Process& process, const Pay& payoff,
                            std::size_t total_paths, double discount,
                            unsigned n_threads = 0,
                            std::uint64_t base_seed = 12345ULL) {
    if (n_threads == 0)
        n_threads = std::max(1u, std::thread::hardware_concurrency());
    if (total_paths == 0) return 0.0;
    n_threads = static_cast<unsigned>(
        std::min<std::size_t>(n_threads, total_paths));

    const std::size_t chunk     = total_paths / n_threads;
    const std::size_t remainder = total_paths % n_threads;

    std::vector<std::future<double>> futures;
    futures.reserve(n_threads);

    for (unsigned t = 0; t < n_threads; ++t) {
        // Spread the remainder over the first few threads so we use exactly
        // total_paths samples (not chunk * n_threads).
        const std::size_t this_chunk = chunk + (t < remainder ? 1 : 0);
        const std::uint64_t seed = seed_for(base_seed, t);
        futures.push_back(std::async(std::launch::async,
            [&process, &payoff, this_chunk, seed] {
                std::mt19937_64 rng(seed);  // independent stream per thread
                return simulate_chunk(process, payoff, this_chunk, rng);
            }));
    }

    double sum = 0.0;
    for (auto& f : futures) sum += f.get();  // reduce
    return discount * (sum / static_cast<double>(total_paths));
}

// Sum a PathPayoff over `n` simulated GBM paths using the supplied RNG. Each
// worker keeps its own reusable path buffer (no shared state -> no races).
template <PathPayoff Pay, typename Rng>
double simulate_path_chunk(const GbmPathGenerator& gen, const Pay& payoff,
                           std::size_t n, Rng& rng) {
    std::vector<double> path;
    path.reserve(gen.steps());
    double sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        gen.sample(rng, path);
        sum += payoff(std::span<const double>(path));
    }
    return sum;
}

// Parallel path Monte Carlo for exotic, path-dependent payoffs. Same fan-out /
// reduce structure as monte_carlo_parallel, with an independent RNG stream per
// thread (plan §5.2 / §12).
template <PathPayoff Pay>
double monte_carlo_path_parallel(const GbmPathGenerator& gen, const Pay& payoff,
                                 std::size_t total_paths, double discount,
                                 unsigned n_threads = 0,
                                 std::uint64_t base_seed = 12345ULL) {
    if (n_threads == 0)
        n_threads = std::max(1u, std::thread::hardware_concurrency());
    if (total_paths == 0) return 0.0;
    n_threads = static_cast<unsigned>(
        std::min<std::size_t>(n_threads, total_paths));

    const std::size_t chunk     = total_paths / n_threads;
    const std::size_t remainder = total_paths % n_threads;

    std::vector<std::future<double>> futures;
    futures.reserve(n_threads);
    for (unsigned t = 0; t < n_threads; ++t) {
        const std::size_t this_chunk = chunk + (t < remainder ? 1 : 0);
        const std::uint64_t seed = seed_for(base_seed, t);
        futures.push_back(std::async(std::launch::async,
            [&gen, &payoff, this_chunk, seed] {
                std::mt19937_64 rng(seed);
                return simulate_path_chunk(gen, payoff, this_chunk, rng);
            }));
    }

    double sum = 0.0;
    for (auto& f : futures) sum += f.get();
    return discount * (sum / static_cast<double>(total_paths));
}

// Deterministic parallel Monte Carlo (plan §16.5). Yields a *bit-identical*
// price regardless of thread count, for a fixed key.
//
// Two ingredients are needed for true bit-exactness:
//   1. Each path draws from a counter-based RNG keyed by its GLOBAL index, so
//      the value path `i` sees is independent of how paths are partitioned.
//   2. The reduction is over a FIXED number of fixed-size blocks (not one
//      partial sum per thread). Floating-point addition isn't associative, so
//      the *grouping* must not depend on thread count either — otherwise the
//      last bit wobbles. We sum each block, then sum the block totals in block
//      order; that ordering is the same whether 1 thread or 8 compute them.
template <StochasticProcess Process, Payoff Pay>
double monte_carlo_parallel_deterministic(const Process& process,
                                          const Pay& payoff,
                                          std::size_t total_paths,
                                          double discount, unsigned n_threads = 0,
                                          std::uint64_t key = 0x5EED) {
    if (n_threads == 0)
        n_threads = std::max(1u, std::thread::hardware_concurrency());
    if (total_paths == 0) return 0.0;
    n_threads = static_cast<unsigned>(
        std::min<std::size_t>(n_threads, total_paths));

    // Fixed block size, independent of thread count, so the reduction grouping
    // is identical for any n_threads. The number of blocks depends only on
    // total_paths.
    constexpr std::size_t kBlock = 65536;
    const std::size_t n_blocks = (total_paths + kBlock - 1) / kBlock;
    std::vector<double> block_sums(n_blocks, 0.0);

    // Workers fill block_sums[b] independently (disjoint indices — no races).
    std::vector<std::future<void>> futures;
    futures.reserve(n_threads);
    std::atomic<std::size_t> next_block{0};
    for (unsigned t = 0; t < n_threads; ++t) {
        futures.push_back(std::async(std::launch::async,
            [&process, &payoff, &block_sums, &next_block, key, total_paths,
             n_blocks] {
                for (;;) {
                    const std::size_t b = next_block.fetch_add(1);
                    if (b >= n_blocks) break;
                    const std::size_t lo = b * kBlock;
                    const std::size_t hi = std::min(lo + kBlock, total_paths);
                    double s = 0.0;
                    for (std::size_t i = lo; i < hi; ++i) {
                        const double z = counter_normal(key, i);
                        s += payoff(process.terminal(z));
                    }
                    block_sums[b] = s;  // each block written once
                }
            }));
    }
    for (auto& f : futures) f.get();

    // Reduce in fixed block order — identical grouping at any thread count.
    double sum = 0.0;
    for (double bs : block_sums) sum += bs;
    return discount * (sum / static_cast<double>(total_paths));
}

}  // namespace mape

#endif  // MAPE_THREADING_PARALLEL_MC_HPP
