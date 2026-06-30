#ifndef MAPE_BENCH_UTIL_HPP
#define MAPE_BENCH_UTIL_HPP

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <vector>

// Dependency-free micro-benchmark helpers (plan §12), in the same spirit as the
// test harness: no Google Benchmark, just <chrono> done carefully.
//
// Methodology baked in (plan §12 "so the numbers mean something"):
//   * monotonic clock (steady_clock), never wall-clock;
//   * a warm-up run before timing so caches / CPU governor settle;
//   * several repetitions, reporting the MEDIAN plus min/max spread, never a
//     single run;
//   * a do-not-optimize barrier so the compiler can't delete the work measured.

namespace mape::bench {

using Clock = std::chrono::steady_clock;

// Prevent the optimiser from discarding a computed value whose result is
// otherwise unused. Forces `value` to be treated as observable.
template <typename T>
inline void do_not_optimize(T const& value) {
    asm volatile("" : : "r,m"(value) : "memory");
}

struct Stats {
    double median_ms = 0.0;
    double min_ms = 0.0;
    double max_ms = 0.0;
    int reps = 0;
};

// Time `fn` `reps` times after `warmup` untimed runs; return median/min/max in
// milliseconds. `fn` should perform one full unit of work per call.
inline Stats measure(const std::function<void()>& fn, int reps = 7,
                     int warmup = 1) {
    for (int i = 0; i < warmup; ++i) fn();

    std::vector<double> samples;
    samples.reserve(reps);
    for (int i = 0; i < reps; ++i) {
        const auto t0 = Clock::now();
        fn();
        const auto t1 = Clock::now();
        samples.push_back(
            std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    std::sort(samples.begin(), samples.end());
    Stats s;
    s.reps = reps;
    s.min_ms = samples.front();
    s.max_ms = samples.back();
    s.median_ms = samples[samples.size() / 2];
    return s;
}

// --- CSV emitter --------------------------------------------------------
// One row per measurement so results are diffable across machines/commits.
// Schema (plan §12): model, threads, paths, median_ms, speedup, efficiency,
// std_error.
class CsvWriter {
public:
    CsvWriter() {
        std::printf(
            "model,threads,paths,median_ms,speedup,efficiency,std_error\n");
    }
    void row(const char* model, unsigned threads, std::size_t paths,
             double median_ms, double speedup, double efficiency,
             double std_error) const {
        std::printf("%s,%u,%zu,%.4f,%.4f,%.4f,%.6g\n", model, threads, paths,
                    median_ms, speedup, efficiency, std_error);
    }
};

}  // namespace mape::bench

#endif  // MAPE_BENCH_UTIL_HPP
