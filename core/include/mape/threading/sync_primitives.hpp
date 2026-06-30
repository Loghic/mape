#ifndef MAPE_THREADING_SYNC_PRIMITIVES_HPP
#define MAPE_THREADING_SYNC_PRIMITIVES_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <future>
#include <latch>
#include <random>
#include <semaphore>
#include <thread>
#include <vector>

#include "mape/concepts.hpp"
#include "mape/threading/parallel_mc.hpp"

namespace mape {

// Concurrency primitives where they actually pull weight (plan §15.6).
// `std::packaged_task` already powers the ThreadPool (thread_pool.hpp); this
// header adds the latch- and semaphore-based variants.

// --- std::latch: synchronized-start parallel Monte Carlo ----------------
//
// In a plain fan-out the worker threads start whenever the OS schedules them,
// so early workers begin computing while later ones are still spawning. For a
// *benchmark* that staggered startup pollutes the timing. A std::latch lets
// every worker block until all of them are ready, then release at the same
// instant — so the measured interval reflects concurrent work, not spawn skew.
//
// Functionally identical result to monte_carlo_parallel; the latch only governs
// *when* the workers begin the timed region.
template <StochasticProcess Process, Payoff Pay>
double monte_carlo_parallel_synced(const Process& process, const Pay& payoff,
                                   std::size_t total_paths, double discount,
                                   unsigned n_threads = 0,
                                   std::uint64_t base_seed = 12345ULL) {
    if (n_threads == 0)
        n_threads = std::max(1u, std::thread::hardware_concurrency());
    if (total_paths == 0) return 0.0;
    n_threads = static_cast<unsigned>(
        std::min<std::size_t>(n_threads, total_paths));

    const std::size_t chunk = total_paths / n_threads;
    const std::size_t remainder = total_paths % n_threads;

    // All workers wait on this latch, then begin together.
    std::latch start_gate(static_cast<std::ptrdiff_t>(n_threads));
    std::vector<std::future<double>> futures;
    futures.reserve(n_threads);

    for (unsigned t = 0; t < n_threads; ++t) {
        const std::size_t this_chunk = chunk + (t < remainder ? 1 : 0);
        const std::uint64_t seed = seed_for(base_seed, t);
        futures.push_back(std::async(std::launch::async,
            [&start_gate, &process, &payoff, this_chunk, seed] {
                start_gate.arrive_and_wait();  // release all workers together
                std::mt19937_64 rng(seed);
                return simulate_chunk(process, payoff, this_chunk, rng);
            }));
    }

    double sum = 0.0;
    for (auto& f : futures) sum += f.get();
    return discount * (sum / static_cast<double>(total_paths));
}

// --- std::counting_semaphore: bounded in-flight tasks -------------------
//
// When a huge book is repriced at once, launching one async task per
// instrument can spawn thousands of threads and blow up peak memory (each
// holds a path buffer). A counting semaphore caps how many tasks run
// concurrently: a worker acquires a permit before computing and releases it
// after, so at most `max_in_flight` are ever active regardless of book size.
template <typename Job>
std::vector<double> run_bounded(const std::vector<Job>& jobs,
                                std::ptrdiff_t max_in_flight) {
    if (max_in_flight < 1) max_in_flight = 1;
    // A reasonable upper bound for the template parameter; the runtime value is
    // what actually limits concurrency.
    std::counting_semaphore<1024> permits(max_in_flight);

    std::vector<std::future<double>> futures;
    futures.reserve(jobs.size());
    for (const Job& job : jobs) {
        futures.push_back(std::async(std::launch::async, [&permits, &job] {
            permits.acquire();        // wait for a free slot
            const double result = job();
            permits.release();        // free the slot for the next task
            return result;
        }));
    }

    std::vector<double> out;
    out.reserve(jobs.size());
    for (auto& f : futures) out.push_back(f.get());
    return out;
}

}  // namespace mape

#endif  // MAPE_THREADING_SYNC_PRIMITIVES_HPP
