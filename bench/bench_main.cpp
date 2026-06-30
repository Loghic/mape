// Performance benchmarks for the pricing engine (plan §12).
//
// Measures latency per model, parallel Monte Carlo scaling (speedup +
// efficiency), portfolio throughput, Monte Carlo accuracy-vs-paths, and
// autodiff-vs-bump Greeks. Emits a CSV (see bench_util.hpp for the schema) plus
// a human-readable summary on stderr so a casual run is still readable while
// the CSV stays diffable.
//
// Methodology notes (also in §12): strong scaling (problem size fixed across
// thread counts), median of several reps after warm-up, monotonic clock. Thread
// *affinity* pinning is deliberately NOT done here — it's platform-specific and
// the plan says to keep it behind the harness; capping n_threads bounds worker
// count, which is enough to show the scaling trend.

#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

#include "bench_util.hpp"
#include "mape/mape.hpp"  // IWYU pragma: keep — umbrella for all models used below

using namespace mape;
using mape::bench::do_not_optimize;
using mape::bench::measure;

namespace {

// A textbook ATM European call to price everywhere.
Option make_call() {
    return Option{OptionType::Call, Exercise::European, 100.0, 1.0};
}
MarketData make_market() { return MarketData{100.0, 0.05, 0.20, 0.0}; }

void log(const char* fmt, double a) { std::fprintf(stderr, fmt, a); }

}  // namespace

int main() {
    const bench::CsvWriter csv;
    const Option call = make_call();
    const MarketData mkt = make_market();
    const double discount = std::exp(-mkt.rate * call.maturity);
    const auto proc = GbmProcess::from_market(mkt, call.maturity);
    const VanillaPayoff payoff = call.payoff();

    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    std::fprintf(stderr, "# hardware_concurrency = %u\n", hw);

    // --- 1. Latency per model (single task, fixed size) -----------------
    {
        BlackScholes bs;
        auto s = measure(
            [&] {
                double p = bs.price(call, mkt);
                do_not_optimize(p);
            },
            15, 2);
        csv.row("black_scholes", 1, 0, s.median_ms, 1.0, 1.0, 0.0);
        log("black_scholes: %.5f ms\n", s.median_ms);

        BinomialTree tree(1000);
        auto sb = measure(
            [&] {
                double p = tree.price(call, mkt);
                do_not_optimize(p);
            },
            9, 1);
        csv.row("binomial_1000steps", 1, 0, sb.median_ms, 1.0, 1.0, 0.0);
        log("binomial(1000): %.5f ms\n", sb.median_ms);
    }

    // --- 2. Parallel Monte Carlo scaling (strong scaling) ---------------
    // Fixed total paths; vary worker count 1,2,4,...,>=hw. Speedup vs 1 thread.
    {
        const std::size_t paths = 4'000'000;
        std::vector<unsigned> thread_counts;
        for (unsigned t = 1; t <= hw * 2; t *= 2) {
            thread_counts.push_back(t);
        }
        if (thread_counts.back() != hw * 2) {
            thread_counts.push_back(hw * 2);
        }

        double t1 = 0.0;
        std::fprintf(stderr, "# parallel MC, %zu paths:\n", paths);
        for (unsigned t : thread_counts) {
            auto s = measure(
                [&] {
                    double p = monte_carlo_parallel(proc, payoff, paths,
                                                    discount, t, 12345ULL);
                    do_not_optimize(p);
                },
                5, 1);
            if (t == 1) {
                t1 = s.median_ms;
            }
            const double speedup = t1 / s.median_ms;
            const double eff = speedup / static_cast<double>(t);
            csv.row("monte_carlo_parallel", t, paths, s.median_ms, speedup, eff,
                    0.0);
            std::fprintf(stderr,
                         "  threads=%2u  %.2f ms  speedup=%.2fx  eff=%.0f%%\n",
                         t, s.median_ms, speedup, eff * 100.0);
        }
    }

    // --- 3. Path-dependent (exotic) MC scaling --------------------------
    {
        const std::size_t paths = 1'000'000;
        GbmPathGenerator gen(mkt, call.maturity, 50);
        AsianPayoff asian{OptionType::Call, call.strike};
        double t1 = 0.0;
        std::fprintf(stderr, "# Asian path-MC, %zu paths x 50 steps:\n", paths);
        for (unsigned t : {1u, hw}) {
            auto s = measure(
                [&] {
                    double p = monte_carlo_path_parallel(gen, asian, paths,
                                                         discount, t, 7ULL);
                    do_not_optimize(p);
                },
                5, 1);
            if (t == 1) {
                t1 = s.median_ms;
            }
            const double speedup = t1 / s.median_ms;
            csv.row("asian_path_mc", t, paths, s.median_ms, speedup,
                    speedup / t, 0.0);
            std::fprintf(stderr, "  threads=%2u  %.2f ms  speedup=%.2fx\n", t,
                         s.median_ms, speedup);
        }
    }

    // --- 4. Portfolio throughput (thread-pool path) ---------------------
    {
        BlackScholes bs;
        constexpr int kBookSize = 2000;
        std::vector<Option> book;
        book.reserve(kBookSize);
        for (int i = 0; i < kBookSize; ++i) {
            book.push_back(Option{OptionType::Call, Exercise::European,
                                  60.0 + (i * 0.05), 1.0});
        }
        ThreadPool pool(hw);
        auto s = measure(
            [&] {
                auto prices = price_portfolio(bs, book, mkt, pool);
                do_not_optimize(prices[0]);
            },
            9, 1);
        const double per_sec =
            static_cast<double>(book.size()) / (s.median_ms / 1000.0);
        csv.row("portfolio_threadpool", hw, book.size(), s.median_ms, 0.0, 0.0,
                0.0);
        std::fprintf(stderr,
                     "# portfolio: %zu instruments in %.3f ms (%.0f/s)\n",
                     book.size(), s.median_ms, per_sec);
    }

    // --- 5. Monte Carlo accuracy vs paths -------------------------------
    // Std error should shrink like 1/sqrt(N); record it against the analytic
    // price so the precision/speed trade-off is visible.
    {
        BlackScholes bs;
        const double exact = bs.price(call, mkt);
        std::fprintf(stderr, "# MC accuracy (analytic = %.4f):\n", exact);
        for (std::size_t n :
             {std::size_t(10'000), std::size_t(100'000), std::size_t(1'000'000),
              std::size_t(10'000'000)}) {
            double price = 0.0;
            auto s = measure(
                [&] {
                    price = monte_carlo_price(proc, payoff, n, discount, 99ULL);
                    do_not_optimize(price);
                },
                3, 0);
            const double abs_err = std::fabs(price - exact);
            csv.row("monte_carlo_serial", 1, n, s.median_ms, 0.0, 0.0, abs_err);
            std::fprintf(stderr,
                         "  paths=%9zu  %.2f ms  price=%.4f  |err|=%.5f\n", n,
                         s.median_ms, price, abs_err);
        }
    }

    // --- 6. AD Greeks vs bump-and-revalue -------------------------------
    // AD gets delta+vega+rho+gamma from a few augmented evaluations; bumping
    // reprices ~2x per Greek. Time both for the same set of sensitivities.
    {
        BlackScholesAD ad;
        BlackScholes bs;
        const int iters = 200000;

        auto s_ad = measure(
            [&] {
                double acc = 0.0;
                for (int i = 0; i < iters; ++i) {
                    acc += ad.delta(call, mkt) + ad.gamma(call, mkt) +
                           ad.vega(call, mkt) + ad.rho(call, mkt);
                }
                do_not_optimize(acc);
            },
            5, 1);

        auto s_bump = measure(
            [&] {
                double acc = 0.0;
                const double h = 1e-4;
                for (int i = 0; i < iters; ++i) {
                    // delta, gamma (central), vega, rho via finite differences.
                    MarketData up = mkt;
                    MarketData dn = mkt;
                    up.spot += h;
                    dn.spot -= h;
                    const double pu = bs.price(call, up);
                    const double pd = bs.price(call, dn);
                    const double p0 = bs.price(call, mkt);
                    const double delta = (pu - pd) / (2 * h);
                    const double gamma = (pu - (2 * p0) + pd) / (h * h);
                    MarketData uv = mkt;
                    uv.vol += h;
                    const double vega = (bs.price(call, uv) - p0) / h;
                    MarketData ur = mkt;
                    ur.rate += h;
                    const double rho = (bs.price(call, ur) - p0) / h;
                    acc += delta + gamma + vega + rho;
                }
                do_not_optimize(acc);
            },
            5, 1);

        csv.row("greeks_ad", 1, iters, s_ad.median_ms, 0.0, 0.0, 0.0);
        csv.row("greeks_bump", 1, iters, s_bump.median_ms, 0.0, 0.0, 0.0);
        std::fprintf(stderr,
                     "# Greeks x%d: AD %.2f ms vs bump %.2f ms (%.2fx)\n",
                     iters, s_ad.median_ms, s_bump.median_ms,
                     s_bump.median_ms / s_ad.median_ms);
    }

    return 0;
}
