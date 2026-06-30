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
- *(stretch)* Finite-difference PDE solver. **Implemented:** `FdPde` in
  `models/finite_difference.hpp` — Crank-Nicolson on the Black-Scholes PDE
  (uniform spot grid, Thomas tridiagonal solve), European + American (early-
  exercise projection), satisfies `PricingModel`. Converges to Black-Scholes
  (~1e-3 on a 400×400 grid). See `test_finite_difference`.

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

> **All implemented.** Each item below has shipped (the FD PDE solver from §4
> too — see its note there).

- Greeks via automatic/algorithmic differentiation instead of bumping.
  *(autodiff.hpp / black_scholes_ad.hpp)*
- Yield-curve bootstrapping and proper discounting. *(calibration.hpp
  `bootstrap_curve`, market_types.hpp `YieldCurve`)*
- Exotic payoffs (Asian, barrier, lookback) — these fall out almost for free
  thanks to the templated payoff design. *(exotic.hpp)*
- Convergence and payoff charts in the GUI. *(Convergence tab)*
- A counter-based RNG (e.g. Philox) for reproducible parallel streams.
  *(counter_rng.hpp; deterministic parallel MC, §16.5)*

---

## 14. Near-term improvements

Three concrete, high-value items from a design review — planned work, not
stretch goals.

### 14.1 CI matrix (g++ + AppleClang, sanitizers, ABI check)

> **Implemented.** `.github/workflows/ci.yml` builds the core+FFI across a
> `{gcc-libstdc++, clang-libc++, appleclang/macOS}` matrix, runs TSan + ASan/UBSan
> over the concurrency tests, and an `abi` job runs
> `scripts/check_abi_consistency.py` (parses `mape_c_api.h` vs the `extern "C"`
> block in `bridge.rs`; 16 functions must match).

Gotchas #3 and #4 (libstdc++ pulling in headers libc++ doesn't; Clang stricter
than g++) are exactly what CI catches for free. A pipeline that builds the core
+ FFI on **both g++ (libstdc++) and AppleClang (libc++)**, runs the test harness
+ the compile-time tests + the C smoke test, runs **TSan and ASan** over the
concurrency tests, and runs the **header-vs-Rust consistency check** (parse
`mape_c_api.h` against the `extern "C"` block in `bridge.rs`) turns three
"learned the hard way" gotchas into a red X before merge. A GitHub Actions
`{compiler} × {sanitizer}` matrix; the header-only core keeps it cheap.

### 14.2 Monte Carlo variance reduction

> **Implemented.** `monte_carlo_price_antithetic` in `models/monte_carlo.hpp`
> and `monte_carlo_control_variate` in `variance_reduction.hpp` (control = the
> closed-form BS price). Tested: antithetic lands near analytic; the control
> variate drives the self-control error to ~0. See `test_variance_reduction`.

MC error shrinks only as 1/√N, which is why §12 leans on threads. Two cheap
techniques cut the constant in front:
- **Antithetic variates** — for each draw `Z`, also use `−Z`; roughly halves
  variance for symmetric payoffs, a few lines in `monte_carlo.hpp`.
- **Control variates** — reuse the closed-form Black–Scholes (already here) as
  the control: price the vanilla with both MC and BS and correct the MC estimate
  by the known BS error. For vanillas the correlation is near-perfect, often an
  order-of-magnitude error cut, and it's essentially free given BS exists.

Both feed the accuracy-vs-time line in §12 and the Convergence tab. Validated
the usual way: same price within tolerance, lower standard error at equal paths.

### 14.3 Dividend yield / cost-of-carry

> **Already implemented.** `MarketData` carries a continuous dividend yield `q`,
> and it flows through Black–Scholes (`df_q = e^{-qT}`, the `(r−q)` drift),
> the binomial tree, the Monte Carlo GBM drift, and the FFI. The FX forward uses
> the foreign rate as the carry. Default `q = 0` keeps simple call sites working.

The current BS signature is `(spot, strike, rate, vol, maturity)` — no carry.
Adding a continuous yield `q` (forward `F = S·e^((r−q)T)`) makes pricing correct
for dividend-paying equities and indices, and — reading `q` as the foreign rate
— for FX, which §4 already lists. Small, localized change with a real
correctness payoff; default `q = 0` keeps existing call sites working.

---

## 15. Techniques from the C++ course (where they actually fit)

Mapping the course syllabus onto the engine, keeping the project's own rule:
**load-bearing, not decoration**. Several topics are already exercised (concepts,
templates, threads, `async`/`future`, atomics, condition variables, thread-local
state, move semantics, ranges-lite). The items below are the ones worth
*adding*; deliberately skipped topics are at the end with reasons.

### 15.1 Variadic templates + fold expressions — compile-time portfolio

> **Implemented.** `Portfolio<Legs...>` in `portfolio_compile_time.hpp` holds a
> `std::tuple` of legs and folds a per-leg `leg_value` over the pack via
> `std::apply`. Heterogeneous legs (options, bonds, FX) in one typed object;
> `size()` is a compile-time constant. See `test_variadic_portfolio`.

A `Portfolio<Insts...>` whose total value and Greeks fold over the parameter
pack:
```cpp
template <class... Insts>
struct Portfolio {
    std::tuple<Insts...> legs;
    double value(const MarketData& m) const {
        return std::apply([&](auto&... l){ return (price(l, m) + ...); }, legs);
    }
};
```
Earns its place: a multi-leg strategy (spread, straddle) becomes one typed
object, aggregation is a fold, leg types are checked at compile time. The same
fold pattern also combines variance-reduction estimators (§14.2).

### 15.2 CRTP Greeks mixin

> **Implemented** (with §15.3). `BumpGreeks<Model>` in `greeks_mixin.hpp` — a
> CRTP mixin giving any model bump-and-revalue delta/gamma/vega through the
> derived `price`, no virtual call. The base ctor is protected + `friend Model`
> to prevent CRTP misuse.

Make §5.1's "optional CRTP" concrete — a static-polymorphism mixin that gives
any model bump-and-revalue Greeks through the derived `price`, no virtual call:
```cpp
template <class Model>
struct Greeks {
    double delta(double s, double h) const {
        const auto& m = static_cast<const Model&>(*this);
        return (m.price(s + h) - m.price(s - h)) / (2 * h);
    }
};
struct BlackScholes : Greeks<BlackScholes> { double price(double s) const; };
```
Earns its place: one definition of bumped Greeks reused across models, and it
drops straight into the parallel-Greeks path in §5.2.

### 15.3 Capability detection (concepts / SFINAE)

> **Implemented.** `HasAnalyticDelta` concept + `best_delta()` in
> `greeks_mixin.hpp`: dispatches (via `if constexpr`) to a model's analytic
> delta when present, falling back to the CRTP bump otherwise. See
> `test_crtp_greeks`.

Some models expose an analytic Greek (BS), others don't. Detect it and dispatch:
use the analytic form when present, fall back to the §15.2 bump otherwise.
```cpp
template <class M>
concept HasAnalyticDelta =
    requires(M m){ { m.analytic_delta() } -> std::convertible_to<double>; };
```
In C++20 this is a concept; the syllabus's **SFINAE** is the same idea written
the pre-concepts way (the `std::void_t` detection idiom). Earns its place: the
engine always uses the most accurate Greek available, transparently to callers.

### 15.4 Lazy path generation — coroutine + STL iterator + ranges/views

> **Implemented.** `generator<T>` (a C++20 coroutine with a standard input
> iterator) in `generator.hpp`, and `mc_payoff_stream` / `monte_carlo_price_lazy`
> in `models/lazy_monte_carlo.hpp` — yields discounted payoffs lazily in O(1)
> memory, composes with range-for/views, and matches the eager engine exactly
> for the same seed. See `test_lazy_monte_carlo`.

One feature that lands three syllabus topics at once and fits Monte Carlo
naturally. A **coroutine generator** yields per-path payoffs lazily and exposes
a **standard-conforming input iterator**, so it composes with `<algorithm>` and
**ranges/views** without ever materialising a giant array:
```cpp
generator<double> payoffs(Process proc, std::size_t n, Rng rng);  // co_yield per path
double sum = 0; std::size_t k = 0;
for (double pay : payoffs(proc, N, rng)) { sum += pay; ++k; }      // streamed, O(1) memory
double price = discount * sum / k;
```
With views it reads declaratively —
`payoffs(...) | std::views::filter(itm) | std::views::transform(disc)` for "mean
discounted payoff over in-the-money paths." Earns its place: streaming caps
memory regardless of path count and replaces hand-written accumulation loops.
(`std::generator` is C++23; a hand-rolled C++20 generator coroutine works today.
This also satisfies the syllabus's "STL-conforming container/iterator" exercise
far more usefully than a bespoke linked list — see skipped items.)

### 15.5 Bridge / pimpl at the FFI seam

The C ABI's opaque `PricingEngine*` is, in GoF terms, a **Bridge**: a stable
abstraction (the handle the GUI holds) whose *implementor* — which concrete model
and configuration — varies at runtime via the GUI's model dropdown. The
compile-time `Pricer<Model>` templates can't cross the C boundary directly, so a
small runtime-polymorphic implementor (held by pimpl behind the handle) bridges
the templated core to the flat C API. Earns its place: it's the actual seam
between the compile-time core and the runtime selection the GUI needs.

### 15.6 Concurrency primitives: latch, packaged_task, semaphore

> **Implemented.** `std::packaged_task` already powers the `ThreadPool`
> (`thread_pool.hpp`). `threading/sync_primitives.hpp` adds
> `monte_carlo_parallel_synced` (a `std::latch` releases all workers together
> for clean benchmark timing) and `run_bounded` (a `std::counting_semaphore`
> caps in-flight tasks). See `test_sync_primitives`.

Extends §5.2 with the remaining thread-block topics where they pull weight:
- **`std::latch`** — a one-shot countdown to release all benchmark worker threads
  at the same instant, so §12's timing isn't polluted by staggered startup.
- **`std::packaged_task`** — express the portfolio thread pool's jobs as
  `packaged_task`s that hand back one `future` per instrument.
- **`std::counting_semaphore`** — cap in-flight tasks (and thus peak path-buffer
  memory) when a huge book is repriced at once.

### 15.7 Deliberately skipped (and why)

- **C++20 modules** — would fight the header-only, "compiles anywhere with
  `-Icore/include`", sandbox-friendly design the project leans on (and gotcha
  #3). Not worth a disruptive rewrite; an isolated experiment at most.
- **A standalone STL-style linked list** — you'd use `std::vector`; a bespoke
  list is decoration here. The underlying skill (a conforming container/iterator)
  is better spent on the lazy path view in §15.4.
- **`std::barrier`** — lock-step sync only pays off in a staged solver (e.g. a
  parallel finite-difference PDE, syncing per time level). It rides along *if*
  the FD model (a §4 stretch) gets built; not worth adding alone, since MC paths
  are independent and need no barrier.

---

## 16. Toward a production-shaped analytics engine

A second design review, aimed at moving from "pricing library + GUI" toward the
shape of a professional quant analytics stack — without breaking the core's
clean separation. Same rule as §15: **load-bearing, not decoration**.
Recommended sequence: **property tests → market abstraction → calibration →
risk/scenario → deterministic MC**, with the quality/docs touches (§16.7)
threaded in alongside. Items deliberately *not* taken (or scoped down) are in
§16.8.

### 16.1 Property / invariant testing (do first)

> **Implemented.** `test_invariants` in `core/tests/test_main.cpp` sweeps a grid
> of markets/contracts and asserts: price ≥ 0, put–call parity, call ≥ discounted
> intrinsic, call delta ∈ [0,1], American put ≥ European put, and monotonicity in
> spot/vol/maturity.


Highest ROI, lowest risk, and it drops straight into the existing macro harness.
Assert structural truths instead of single values: put–call parity,
`call ≥ intrinsic`, `American ≥ European`, `price ≥ 0`, `delta ∈ [0,1]` for a
call, and monotonicity in spot, vol, and maturity (European call, no dividends).
Earns its place: catches whole classes of regressions that point-value tests
sail past — exactly the safety net to have *before* the refactors below.

### 16.2 Market abstraction layer (the enabler)

> **Implemented (backward-compatible).** `market_types.hpp` adds `YieldCurve`
> (flat or interpolated term structure, `rate_at`/`discount`), `VolSurface`
> (flat or strike smile, `vol_at`), and `SpotQuote`. `MarketData` gains optional
> `curve`/`surface` members plus `rate_at(T)` / `vol_at(K,T)` accessors that fall
> back to the flat scalars when unset — so it stays an aggregate and the existing
> ~60 `MarketData{...}` call sites and the 16 FFI functions are untouched. New
> code (calibration §16.3, bucketed risk §16.4) can attach a curve/surface; old
> code keeps using the flat fields. See `test_market_types`. (Chose this over a
> destabilising big-bang replacement of the scalars.)


Replace primitive doubles (rate, vol, `q`) with small value types inside the
core — `SpotQuote`, `YieldCurve`, `VolSurface` — instead of passing scalars
everywhere. Earns its place: it's the foundation calibration and risk both build
on (bucketed vega and DV01 need curves, not scalars), and it opens the door to
local/stochastic vol, term structures, and multiple curves later. Honest cost:
a refactor that ripples through the 16 FFI functions — do it deliberately, and
*before* more double-passing call sites accrete. Keep the types dependency-free
so the core stays header-only.

### 16.3 Calibration framework

> **Implemented.** `calibration.hpp`: `calibrate_svi` fits Gatheral raw-SVI
> (5 params) to a smile by least squares via a built-in Nelder-Mead simplex
> (dependency-free); `svi_to_surface` turns the fit into a `VolSurface`;
> `bootstrap_curve` recovers a `YieldCurve` from discount factors. Verified by
> recovering a synthetic SVI smile to RMSE ~3e-6 and round-tripping a zero curve.
> See `test_calibration`. (Short-rate model calibration remains out of scope, as
> the plan notes — the engine has no such models yet.)

Generalise the existing `implied_vol` (already single-point calibration) to fit
many quotes at once — a least-squares
`CalibrationResult calibrate(const Model&, std::span<const MarketQuote>)`. v1
scope: fit a vol smile/surface (e.g. SVI) and bootstrap a discount curve. Earns
its place: it closes the market → calibrate → price loop that real engines run
on, and turns the **Smile tab** from raw points into a *fitted* surface. Scoped
down on purpose: short-rate model calibration (Hull–White et al.) is out of v1
— it needs models the engine doesn't have yet.

### 16.4 Risk + scenario engine (one module)

> **Implemented.** `risk.hpp`: `run_scenarios(model, opt, base, scenarios, pool)`
> perturbs the market (spot/rate/vol/dividend shifts), reprices each scenario
> concurrently on the `ThreadPool`, and tabulates P&L vs the base. Helpers:
> `greek_bump_scenarios` + `scenario_greeks` (finite-difference delta/gamma/vega/
> rho through the same engine, matching the analytic Greeks) and
> `stress_scenarios` (large spot/vol shocks incl. a crash combo). Verified:
> scenario reprice == manual, scenario Greeks ~ closed-form, parallel == serial,
> clean under TSan. See `test_risk_scenarios`.

Perturb spot/rate/vol/`q`/FX, reprice, tabulate P&L —
`run_scenarios(portfolio) -> table`. The same machinery serves portfolio
delta/gamma, bucketed vega, DV01, and stress tests. Earns its place: it's where
real desks spend most of their time, it's embarrassingly parallel so it reuses
the thread pool directly, and it's a clean templates + concurrency showcase.
Bucketed/curve-based risk depends on §16.2, so sequence it after.

### 16.5 Deterministic parallel Monte Carlo

> **Implemented.** `counter_rng.hpp` (a stateless counter-based RNG +
> inverse-normal) keys each path's draw by its *global* index, and
> `monte_carlo_parallel_deterministic` in `threading/parallel_mc.hpp` reduces
> over fixed-size blocks (not per-thread partials) so the float summation order
> is also thread-count-independent. Result is **bit-identical** at 1/2/4/8
> threads — verified in `test_deterministic_mc`, clean under TSan.


Make a fixed seed produce identical prices at 1, 2, and 8 threads. Today the
per-chunk `mt19937_64` ties results to the thread count; a counter-based RNG
(Philox/Threefry — already the §13 stretch) or deterministic per-path substreams
fixes it. Earns its place: reproducible tests and benchmarks, and it removes the
one place where changing the thread count can change a number.

### 16.6 Richer `PricingResult`

> **Implemented.** `PricingResult` in `pricing_result.hpp` carries the price
> plus *optional* diagnostics (paths, std_error, threads, model) — `std::optional`
> so a closed-form price reports no fake error, per the "no fabricated number"
> rule. `monte_carlo_result` populates the standard error from path variance.
> See `test_pricing_result`.


Return diagnostics alongside the price: runtime, paths, standard error,
iterations, thread count, model. Earns its place: low effort, and both the bench
harness and the GUI already want it. One caveat, consistent with the "no
fabricated number" rule: standard error / path count are meaningful for Monte
Carlo, not for a closed form — make those fields optional / model-dependent
rather than reporting a fake error for Black–Scholes.

### 16.7 Quality & docs touches (bolt onto existing CI)

> **Implemented.** ADRs (`docs/adr/0001-0003`); execution sequence diagram in
> `docs/architecture.md` / `cpp-design.md`; `-Werror -Wall -Wextra` on all three
> CI compiler legs (gcc, clang/libc++, AppleClang); a **clang-format** gate
> (`.clang-format` + a CI job, whole codebase reformatted to match); a
> **benchmark regression gate** (`scripts/check_bench_regression.py` against the
> committed `bench/results/baseline.csv`); and **C-API fuzzing**
> (`ffi/tests/fuzz_c_api.cpp`, libFuzzer + ASan/UBSan, run in CI) — verified to
> survive 20k+ random inputs with no crash or boundary-escaping exception.


The CI in §14.1 already exists, so these are extensions, not new infrastructure:
- **ADRs** — `docs/adr/0001-header-only.md`, `0002-c-abi.md`, `0003-rust-gui.md`:
  a paragraph each on *why* the decision was made. Near-zero cost, invaluable
  months later.
- **Execution sequence diagram** — GUI → FFI → Pricer → Model → Result, in
  Mermaid like the existing architecture diagrams.
- **Warnings-as-errors** — `-Werror -Wall -Wextra` (plus compiler-specific sets)
  on both GCC and Clang.
- **Lint/format gate** — clang-format + clang-tidy, rustfmt + clippy.
- **Benchmark history** — keep `bench/results/v0.x.csv` (the bench already emits
  CSV) and regression-gate it in CI.
- **C-API fuzzing** — fuzz the flat C surface against malformed input; this
  directly exercises the "catch all exceptions at the boundary" rule.

### 16.8 Considered but deferred / scoped (and why)

- **Serialization (JSON save/load)** of instruments, portfolios, and market
  snapshots — useful for reproducible demos, but it stays *out of the core*
  (which is deliberately dependency-free and header-only). Put it in the FFI/GUI
  layer or an optional module.
- **Composable MC pipeline** (RNG → Brownian bridge → process → path transform →
  payoff → estimator) — elegant, but premature given the working
  `variance_reduction` and `lazy_monte_carlo`. Revisit when a third MC variant
  (Sobol/QMC or stochastic vol) actually arrives, so the policy seams are driven
  by real need rather than speculation.
- **Dynamic plugin / `ModelFactory`** — fights the compile-time `Pricer<Model>`
  + concepts design for little gain (recompiling to add a model is fine). The
  only warranted slice is a small *static* name → model registry at the FFI seam
  — which is already the Bridge in §15.5, not a dynamic-loading system.
