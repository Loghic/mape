# Multi-Asset Pricing Engine — Project Plan

A quantitative pricing engine written in **C++** that values multiple asset
classes using multiple numerical models, with a thin **Rust** desktop GUI on
top. The C++ library is the *core*; the GUI is a front-end that talks to it
over a C ABI.

---

## 1. Goals

- Build a reusable C++ pricing library (the heart of the project).
- Showcase two required language features as real, load-bearing design choices,
  not decoration:
  - **Templates** — a single generic engine prices any (model, payoff) pair in
    a type-safe way.
  - **Threads** — Monte Carlo simulation and portfolio pricing run in parallel.
- Add a small **Rust GUI** (egui) so a user can punch in parameters and see a
  price, the Greeks, and a portfolio table.
- Keep a clean boundary: the engine knows nothing about the GUI.

Non-goals (at least for v1): a production-grade market-data feed, calibration to
live quotes, and exotic path-dependent products beyond a couple of examples.
These are listed as stretch goals.

---

## 2. Architecture

```
┌──────────────────────────┐
│      Rust GUI (egui)     │   pricing-gui
│  inputs · results · grid │
└────────────┬─────────────┘
             │  FFI (extern "C")
┌────────────▼─────────────┐
│   C ABI wrapper layer    │   pricing-ffi
│   (stable, opaque types) │
└────────────┬─────────────┘
             │  C++ calls
┌────────────▼─────────────┐
│   C++ pricing core       │   pricing-core   ◄── the "heart"
│  templates · threads     │
│  models · instruments    │
└──────────────────────────┘
```

Three components:

1. **`pricing-core`** (C++20) — instruments, market data, pricing models, the
   templated engine, and the threading utilities. Has no knowledge of C or Rust.
2. **`pricing-ffi`** (C++) — a thin `extern "C"` wrapper exposing a stable,
   flat C API over opaque handles. This is the only surface Rust sees.
3. **`pricing-gui`** (Rust) — an `eframe`/`egui` desktop app that links the C++
   library and renders a UI.

Rationale for Rust GUI over Python: a single compiled binary, no runtime to
ship, and clean FFI to C/C++. Python (PyBind11) would be the fallback if rapid
prototyping mattered more than packaging.

---

## 3. Tech stack

| Layer        | Choice                                   |
|--------------|------------------------------------------|
| Core         | C++20 (concepts, `<thread>`, `<random>`) |
| Build (C++)  | CMake ≥ 3.20                             |
| Tests (C++)  | GoogleTest (or Catch2)                    |
| FFI          | `extern "C"` + a hand-written C header   |
| GUI          | Rust + `eframe`/`egui`                    |
| Build (Rust) | Cargo + `build.rs` (links the C++ lib)   |

> Pin exact crate/library versions when scaffolding — they move; this plan
> stays version-neutral on purpose.

---

## 4. Domain scope

### Asset classes (start with two, grow the list)
- European option (call/put)
- American option (call/put)
- Vanilla bond (fixed coupon)
- FX forward
- *(stretch)* Asian / barrier options

### Pricing models
- **Black–Scholes–Merton** — closed-form, the analytical benchmark.
- **Binomial / CRR tree** — handles American exercise.
- **Monte Carlo** — path simulation; the workhorse for the threading feature.
- *(stretch)* Finite-difference PDE solver.

Each model implements the same conceptual contract so the engine can treat them
uniformly (see §5.1).

---

## 5. Feature deep-dives

### 5.1 Feature #1 — Templates

The engine is generic over the *model* and the *payoff*, constrained by C++20
**concepts** so misuse is a compile error rather than a runtime surprise.

```cpp
// concepts.hpp
template <typename T>
concept PricingModel = requires(T m, const Instrument& inst, const MarketData& mkt) {
    { m.price(inst, mkt) } -> std::convertible_to<double>;
};

template <typename T>
concept Payoff = requires(T p, double spot) {
    { p(spot) } -> std::convertible_to<double>;
};
```

```cpp
// pricer.hpp — one engine, any conforming model
template <PricingModel Model>
class Pricer {
public:
    explicit Pricer(Model model) : model_(std::move(model)) {}

    double value(const Instrument& inst, const MarketData& mkt) const {
        return model_.price(inst, mkt);
    }
private:
    Model model_;
};
```

Where templates earn their place:
- **Generic engine** — `Pricer<BlackScholes>`, `Pricer<BinomialTree>`,
  `Pricer<MonteCarlo>` with no shared base class or virtual dispatch.
- **Templated Monte Carlo** — parameterised on the stochastic *process* and the
  *payoff*, so a new payoff is just a small callable:
  ```cpp
  template <typename Process, typename Payoff>
  double monte_carlo_price(const Process& process, const Payoff& payoff,
                           std::size_t paths, double discount);
  ```
- **Policy-based design** — day-count and discounting conventions as template
  policies, swapped without touching pricing logic.
- *(optional)* **CRTP** for a static-polymorphism model hierarchy if a shared
  interface is wanted without virtual-call overhead.

### 5.2 Feature #2 — Threads

Monte Carlo is embarrassingly parallel, which makes it the natural showcase.

```cpp
// parallel_mc.hpp
template <typename Process, typename Payoff>
double monte_carlo_parallel(const Process& process, const Payoff& payoff,
                            std::size_t total_paths, double discount,
                            unsigned n_threads) {
    std::vector<std::future<double>> futures;
    const std::size_t chunk = total_paths / n_threads;

    for (unsigned t = 0; t < n_threads; ++t) {
        futures.push_back(std::async(std::launch::async, [&, t] {
            std::mt19937_64 rng(seed_for(t));   // independent stream per thread
            return simulate_chunk(process, payoff, chunk, rng);
        }));
    }

    double sum = 0.0;
    for (auto& f : futures) sum += f.get();      // reduce
    return discount * (sum / total_paths);
}
```

Where threads earn their place:
- **Parallel Monte Carlo** — split paths across threads, accumulate, reduce.
  Each thread owns an **independent RNG stream** (separate seeds / a counter-based
  generator) so results stay statistically valid — this is the easy bug to get
  wrong and worth calling out in code review.
- **Thread pool for portfolio pricing** — a queue of instruments dispatched to a
  fixed worker pool (`std::thread` + a mutex-guarded queue, or a small custom
  `ThreadPool`), so a 1,000-instrument book prices concurrently.
- **Parallel Greeks** — bump-and-revalue (delta, gamma, vega) where each bumped
  scenario runs on its own thread.
- **Concurrency primitives** — `std::async`/`std::future` for fan-out/fan-in,
  `std::mutex`/`std::atomic` for shared accumulation, thread-local buffers to
  avoid false sharing.

### 5.3 Additional C++ features (nice-to-have, not required)
- RAII + smart pointers (`std::unique_ptr`) for engine/resource ownership.
- Move semantics on heavy result/path objects.
- `std::variant` to model a heterogeneous instrument set; `std::optional` for
  results that can fail.
- `std::chrono` for day-count / maturity handling.
- `<algorithm>` + ranges for payoff aggregation.

### 5.4 Compile-time pricing (`constexpr` / `consteval`)

> **Implemented.** `core/include/mape/ct_math.hpp` (constexpr sqrt/exp/log and
> the Abramowitz–Stegun normal CDF, with a `consteval` coefficient table) and
> `core/include/mape/compile_time.hpp` (constexpr Black–Scholes reusing
> `bs_price_generic`, constexpr day-count/discounting, and a `consteval`
> contract validator). Compile-time `static_assert` tests live in
> `core/tests/test_compile_time.cpp`.

Closed-form models are pure functions of their inputs, so anything with known
parameters can be priced *at compile time* — which turns a class of regression
tests into `static_assert`s that cost nothing at runtime and can never silently
rot.

The catch in C++20: the `<cmath>` transcendentals (`std::exp`, `std::sqrt`,
`std::erf`) are **not** `constexpr` until C++23. That gap is exactly why these
helpers are worth writing ourselves — it makes `constexpr`/`consteval`
load-bearing here rather than decorative.

- **`constexpr` math primitives** — a small set of compile-time-evaluable
  building blocks the closed-form models sit on (`sqrt`, `exp`, normal CDF):
  ```cpp
  constexpr double sqrt_ct(double x) {            // Newton–Raphson (illustrative)
      double g = x;
      for (int i = 0; i < 30; ++i) g = 0.5 * (g + x / g);
      return g;
  }

  // standard normal CDF via Abramowitz–Stegun, fully constexpr
  constexpr double norm_cdf(double x);            // polynomial approximation
  ```
- **`constexpr` Black–Scholes** — with the primitives above, the European
  closed form evaluates inside a constant expression:
  ```cpp
  constexpr double bs_call(double s, double k, double r, double vol, double t);

  // a regression test that runs at compile time, for free:
  static_assert(approx(bs_call(100, 100, 0.05, 0.20, 1.0), 10.4506, 1e-3));
  ```
- **`consteval` for must-happen-at-compile-time work** — guaranteed
  compile-time evaluation, with no silent runtime fallback:
  - a `consteval` factory/validator that rejects nonsensical specs (negative
    strike, vol, or maturity) at compile time, so an invalid *literal* contract
    simply won't compile;
  - `consteval`-built coefficient tables (e.g. the Abramowitz–Stegun constants,
    or Sobol direction numbers) materialised once at compile time instead of on
    first use.
- **`constexpr` day-count / discounting** — year fractions and discount factors
  (`exp(-r·t)` via the `constexpr` exp) computed in constant expressions
  wherever the schedule is known.

This slots in alongside the templated models (§5.1): the same Black–Scholes
logic the runtime `Pricer<BlackScholes>` uses can *be* the `constexpr`
function, keeping a single source of truth rather than a parallel
compile-time copy.

---

## 6. The FFI bridge (C++ ↔ Rust)

Expose a flat C API over **opaque handles**. No C++ types cross the boundary.

```cpp
// mape_c_api.h
typedef struct PricingEngine PricingEngine;

#ifdef __cplusplus
extern "C" {
#endif
PricingEngine* pe_create(void);
double pe_price_european_call(PricingEngine* e,
                              double spot, double strike, double rate,
                              double vol, double maturity);
double pe_delta(PricingEngine* e, /* same params */ ...);
void   pe_destroy(PricingEngine* e);
#ifdef __cplusplus
}
#endif
```

```rust
// bridge.rs — Rust side
#[repr(C)] pub struct PricingEngine { _private: [u8; 0] }

extern "C" {
    fn pe_create() -> *mut PricingEngine;
    fn pe_price_european_call(e: *mut PricingEngine, spot: f64, strike: f64,
                              rate: f64, vol: f64, maturity: f64) -> f64;
    fn pe_destroy(e: *mut PricingEngine);
}
// wrap these in a safe Rust struct with Drop calling pe_destroy.
```

Ownership rule: whoever calls `pe_create` must call `pe_destroy`. The Rust
wrapper enforces this via `Drop`. Consider the `cxx` crate later for a safer,
less manual bridge; raw `extern "C"` + a hand-written header is the simplest
starting point.

---

## 7. Rust GUI (egui)

A single window:
- **Inputs** — spot, strike, rate, volatility, maturity; dropdowns for asset
  class and model.
- **Results** — price, plus Greeks (delta, gamma, vega, theta, rho).
- **Portfolio tab** — a table of instruments with a "Reprice all" button that
  exercises the threaded path.
- *(stretch)* a small payoff or convergence chart (`egui_plot`).

The GUI does no math — it formats inputs, calls the FFI, and renders what comes
back.

---

## 8. Project structure

```
multi-asset-pricing-engine/
├── CMakeLists.txt
├── README.md
├── plan.md
├── core/                          # C++ pricing engine — the heart
│   ├── include/mape/
│   │   ├── instruments.hpp
│   │   ├── market_data.hpp
│   │   ├── concepts.hpp
│   │   ├── pricer.hpp             # templated engine
│   │   ├── models/
│   │   │   ├── black_scholes.hpp
│   │   │   ├── binomial.hpp
│   │   │   └── monte_carlo.hpp
│   │   └── threading/
│   │       ├── thread_pool.hpp
│   │       └── parallel_mc.hpp
│   ├── src/
│   └── tests/
├── ffi/                           # C ABI wrapper around core
│   ├── include/mape_c_api.h
│   └── src/mape_c_api.cpp
├── gui/                           # Rust desktop front-end
│   ├── Cargo.toml
│   ├── build.rs                   # links the C++ library
│   └── src/
│       ├── main.rs
│       └── bridge.rs
└── scripts/
```

---

## 9. Build & integration

1. **CMake** builds `pricing-core` and `pricing-ffi` into one library
   (`libmape`), static or shared.
2. **Cargo** builds the GUI; `build.rs` either invokes CMake (via the `cmake`
   crate) or points the linker at a pre-built `libmape`, then emits
   `cargo:rustc-link-lib` / `cargo:rustc-link-search`.
3. On Windows link the `.lib`/`.dll`, on Linux `.a`/`.so`, on macOS `.a`/`.dylib`
   — keep this isolated in `build.rs`.

---

## 10. Roadmap

| Phase | Deliverable                                                            |
|-------|------------------------------------------------------------------------|
| 0     | Repo scaffolding, CMake + Cargo skeletons, CI building both sides       |
| 1     | Black–Scholes for a single European option (no templates, no threads)   |
| 2     | Generalise into the templated `Pricer` + concepts; add binomial tree    |
| 3     | Monte Carlo model, then parallelise it; add the thread pool             |
| 4     | C ABI wrapper + safe Rust bridge; price one instrument from Rust        |
| 5     | egui GUI: single-instrument pricing + Greeks                            |
| 6     | Portfolio view + threaded reprice; more assets/models; polish           |

Phases 2 and 3 are where the two required features land, so they get the most
care and the most tests.

---

## 11. Testing

- **Unit tests** per model (GoogleTest).
- **Analytical benchmarks** — Monte Carlo and the binomial tree must converge to
  the Black–Scholes closed form within tolerance; this validates all three at
  once.
- **Concurrency tests** — same inputs, single-threaded vs multi-threaded, must
  agree within Monte Carlo error; run under TSan/ASan to catch races and
  use-after-free across the FFI boundary.
- **Greeks** — finite-difference Greeks checked against known signs/bounds.

(Note: the "analytical benchmarks" above check *correctness*. Section 12 below
is about *speed* — a separate concern.)

---

## 12. Performance benchmarking

> **Implemented.** `bench/bench_util.hpp` (steady-clock timing: warm-up, median
> + spread, do-not-optimize barrier) and `bench/bench_main.cpp` (all measures
> below, CSV on stdout + summary on stderr). Built with `-DMAPE_BUILD_BENCH=ON`.
> See the README "Benchmarks" section. Sample 4-core run: parallel Monte Carlo
> hits ~3.9× speedup / 97% efficiency at 4 threads, flat beyond (Amdahl, as
> predicted). One honest deviation from the prediction below: for *Black–Scholes*
> Greeks, AD ≈ bump-and-revalue (~1×) rather than a clear AD win — a closed-form
> reprice is so cheap that AD's per-op derivative overhead cancels the "N+1
> repricings" cost. AD's advantage materialises when the base price is expensive
> (Monte Carlo / trees), not for a closed form.

The threading work in §5.2 only earns its keep if we can *show* it pays off.
This section measures, per model, how long a pricing run takes and — for the
parallel models — how that time scales from 1 core to N cores. It naturally
follows Phase 3 of the roadmap, once parallel Monte Carlo exists.

### What we measure

- **Latency** — wall-clock time for one pricing task at a fixed problem size
  (e.g. a 1,000,000-path Monte Carlo, a 1,000-step binomial tree, one
  Black–Scholes evaluation).
- **Parallel scaling** — Monte Carlo time at thread counts 1, 2, 4, 8, … up to
  and beyond the physical core count.
- **Throughput** — prices per second for a whole portfolio (the thread-pool
  path), which is the more realistic workload.
- **Accuracy-vs-time** — Monte Carlo standard error against runtime / path
  count, since MC explicitly trades precision for speed.
- **Greeks cost** — autodiff Greeks (the `Dual`/`Dual2` path in `autodiff.hpp`)
  against bump-and-revalue, now that both exist: AD gets all sensitivities in
  roughly one augmented evaluation, bumping reprices once per Greek.

### Key metrics

- **Speedup** `S(n) = T(1) / T(n)`
- **Parallel efficiency** `E(n) = S(n) / n` (1.0 = perfect linear scaling)
- Framed by **Amdahl's law**: MC has a small serial part (seeding, the final
  reduction), so efficiency should stay high up to the physical core count, then
  flatten — hyperthreads add little for compute-bound floating-point work.

### What we expect to see (and why it's worth showing)

- **Black–Scholes** — sub-microsecond, single-threaded, and flat across core
  counts: a closed form has nothing to parallelise. It's the floor everything
  else is compared against.
- **Binomial tree** — sequential backward induction; benchmarked for latency,
  not scaling.
- **Monte Carlo** — the headline result: near-linear speedup to the physical
  core count (e.g. ~3.7× on 4 cores), tailing off afterwards. This is the chart
  that justifies the whole threading effort.
- **Path-dependent / exotic MC** — `path_monte_carlo` (Asian, barrier) should
  scale like vanilla MC, since the paths stay independent; benchmarked to
  confirm the thread-pool path holds up under heavier per-path work.
- **AD vs bump Greeks** — the autodiff path should win clearly: one augmented
  evaluation versus N+1 repricings for N Greeks.

### Methodology (so the numbers mean something)

- **Warm up** before timing (let caches and the CPU governor settle), then take
  several repetitions and report the **median** plus spread — never a single
  run.
- **Strong scaling** — hold the problem size fixed across thread counts so the
  speedup curve is apples-to-apples.
- **Control the lever** — cap the worker count via the `n_threads` parameter
  already in `parallel_mc` / the thread pool; record physical vs logical core
  count and the CPU model alongside every result.
- **Pin to cores for clean numbers** — capping `n_threads` only bounds how many
  workers spawn; the OS scheduler can still migrate them between cores. For
  rigorous per-core figures, set thread affinity (`pthread_setaffinity_np` on
  Linux, `SetThreadAffinityMask` on Windows). It's platform-specific, so keep it
  behind the bench harness rather than in the core.
- **Watch for frequency scaling** — turbo/boost inflates low-core runs and
  distorts the curve; pin the frequency if possible, otherwise record it.
- Use a monotonic clock (`std::chrono::steady_clock`), not wall-clock time.

### Tooling & output

- A small `bench/` harness built on `std::chrono`, in the same dependency-free
  spirit as the test harness (Google Benchmark is the heavier alternative if we
  later want built-in statistical rigour).
- Emit a CSV — `model, threads, paths, median_ms, speedup, efficiency,
  std_error` — so results are diffable across machines and across commits.
- Surface the speedup curve in the GUI as a sibling to the convergence chart
  (§7), or simply plot the CSV.

---

## 13. Stretch goals

- Greeks via automatic/algorithmic differentiation instead of bumping.
- Yield-curve bootstrapping and proper discounting.
- Exotic payoffs (Asian, barrier, lookback) — these fall out almost for free
  thanks to the templated payoff design.
- Convergence and payoff charts in the GUI.
- A counter-based RNG (e.g. Philox) for reproducible parallel streams.
