# AGENTS.md

Guidance for AI agents and contributors working in this repository. Read this
before making changes; it captures the architecture, the commands that matter,
and the non-obvious gotchas that have bitten us.

## What this project is

A multi-asset quantitative **pricing engine** in C++20, exposed to a Rust/egui
desktop GUI over a C ABI, with an optional Python market-data fetcher. The C++
core is the heart; everything else is a consumer of it. See
[`plan.md`](plan.md) for the original design and [`docs/`](docs/) for in-depth
documentation.

## Layout

```
core/        C++20 pricing engine (header-only). The "heart". Knows nothing
             about C, Rust, the GUI, or the database.
  include/mape/   public headers: concepts, pricer, portfolio,
                  portfolio_compile_time (variadic), instruments, market_data,
                  autodiff (Dual/Dual2), ct_math, compile_time, implied_vol,
                  exotic, variance_reduction, greeks_mixin (CRTP), generator
                  (coroutine); models/ (black_scholes[_ad], binomial,
                  monte_carlo, path_monte_carlo, lazy_monte_carlo,
                  fixed_income); threading/ (thread_pool, parallel_mc,
                  sync_primitives). See docs/cpp-codemap.md.
  tests/          dependency-free harnesses: test_main.cpp (runtime, 66 checks)
                  and test_compile_time.cpp (static_asserts)
ffi/         extern "C" wrapper -> libmape. The only surface Rust sees.
  include/mape_c_api.h   flat C API over an opaque handle (16 functions)
  src/, tests/           implementation + a pure-C smoke test
gui/         Rust eframe/egui app. Links libmape via build.rs.
  src/bridge.rs   safe wrapper over the C API (Drop frees the engine)
  src/data.rs     read-only rusqlite access to the market-data cache
  src/main.rs     the egui app (tabs: Single, Portfolio, Convergence, Smile,
                  Fixed income, Exotics)
bench/       performance benchmarks (plan §12); off unless -DMAPE_BUILD_BENCH=ON
data/        schema.sql for the SQLite market-data cache
src/mape_data/   the Python fetcher package (yfinance -> SQLite)
scripts/     build_and_test.sh; fetch_data.py (thin shim into src/mape_data)
docs/        user-guide, study-guide, architecture, cpp-design, cpp-codemap,
             cpp20-concepts (Mermaid diagrams throughout)
pyproject.toml   uv-managed Python project for the data fetcher
```

## Architecture in one sentence

`Rust GUI → C ABI (libmape) → C++ core`, with a side channel
`Python (yfinance) → SQLite → Rust GUI`. The C++ core never touches C, Rust,
the network, or the DB — that separation is load-bearing, keep it.

## Build & test commands

C++ core + FFI (the part most changes touch):

```bash
rm -rf build                 # see gotcha #1 — do this after any CMake change
./scripts/build_and_test.sh        # or: ./scripts/build_and_test.sh -v
```

Expected: `66 checks, 0 failures` (core), `PASS` (compile-time tests), and
`PASS (failures: 0)` (C smoke).

Quick verify without CMake (header-only core, sandbox-friendly):

```bash
g++ -std=c++20 -O2 -pthread -Icore/include core/tests/test_main.cpp -o /tmp/t && /tmp/t
```

Sanitizers (the concurrency code must stay clean):

```bash
g++ -std=c++20 -O1 -g -fsanitize=thread -pthread -Icore/include \
    core/tests/test_main.cpp -o /tmp/tsan && /tmp/tsan
```

Rust GUI (needs CMake + C++20 compiler + Rust on PATH):

```bash
cargo run --release --manifest-path gui/Cargo.toml   # run from repo root
```

Python data fetcher (uv):

```bash
uv sync
uv run fetch-data AAPL MSFT --max-expiries 3
```

## Conventions

- **C++**: header-only core under `core/include/mape/`. Each header has an
  include guard and includes exactly what it uses (don't rely on transitive
  includes — see gotcha #3). Models satisfy the `PricingModel` concept; payoffs
  satisfy `Payoff`/`PathPayoff`. New code that crosses the FFI must catch all
  exceptions at the boundary (an exception unwinding into C/Rust is UB).
- **C ABI**: every C function is mirrored by an `extern "C"` declaration in
  `gui/src/bridge.rs`. After changing the C API, re-verify both sides agree —
  there is a small Python consistency check used in development that parses
  `mape_c_api.h` against the Rust `extern "C"` block; keep them in lockstep
  (same name, arity, parameter types). Invalid inputs return `NaN` (or a
  `MapeStatus`); the Rust wrappers map those to `Option`/`Result`.
- **Errors**: prefer "no result" over a fabricated number. `implied_vol`
  returns `std::optional` / `NaN` / `None` when the vol is genuinely undefined.
- **Tests**: add to `core/tests/test_main.cpp` (C++) and
  `ffi/tests/c_smoke_test.c` (C ABI). The harness is a tiny macro set, not
  GoogleTest, so it runs anywhere.

## Gotchas (learned the hard way)

1. **Stale CMake cache.** After editing any `CMakeLists.txt`, `rm -rf build`
   before reconfiguring. CMake caches target properties and the test registry;
   symptoms include "No tests were found" from ctest and link errors that
   persist after a fix. The root `CMakeLists.txt` calls `enable_testing()` at
   the top level (required for ctest to discover subdir tests) and declares
   `LANGUAGES CXX C` (the C is needed to compile the C-side smoke test — without
   it CMake silently drops `c_smoke_test.c` and the link fails with undefined
   `_main`).

2. **Fetcher layout & paths.** The fetcher is a proper package at
   `src/mape_data/` (with a thin `scripts/fetch_data.py` shim for the
   plain-Python path). `uv sync` installs it editable, so source edits take
   effect without a reinstall. It resolves `data/` relative to the current
   working directory (run from the repo root), with an embedded schema fallback
   baked into the module. If you ever see a stale install, `rm -rf .venv &&
   uv sync` is the reset.

3. **libc++ vs libstdc++ includes.** g++ (libstdc++) pulls in many headers
   transitively; AppleClang (libc++) does not. Always include `<algorithm>`
   for `std::min`/`std::max`, `<cstdint>` for `std::uint64_t`, etc. directly.
   A header that compiles on Linux can fail on macOS for this reason.

4. **AppleClang is stricter than g++.** e.g. `new T{}` aggregate-initializing a
   member with an `explicit` constructor is rejected by Clang; give such members
   an explicit `{}` initializer.

5. **`build.rs` and friends are easy to delete by accident.** Commit early.

## Where the required features live (for reviewers)

- **C++20 concepts**: `core/include/mape/concepts.hpp` — `PricingModel`,
  `Payoff`, `StochasticProcess`, `PathPayoff`.
- **Templates**: `core/include/mape/pricer.hpp` (`Pricer<Model>`),
  `core/include/mape/models/monte_carlo.hpp`,
  `core/include/mape/models/black_scholes_ad.hpp` (scalar-generic for AD).
- **Threading**: `core/include/mape/threading/parallel_mc.hpp` (parallel MC,
  `std::async`/`std::future`) and `thread_pool.hpp` (`std::thread` +
  `std::mutex` + `std::condition_variable`).

For navigating the C++: [`docs/cpp-codemap.md`](docs/cpp-codemap.md) (where each
feature lives) and [`docs/cpp-design.md`](docs/cpp-design.md) (why it's built
this way + how the FFI works). [`docs/cpp20-concepts.md`](docs/cpp20-concepts.md)
is the language-feature deep-dive; [`docs/study-guide.md`](docs/study-guide.md)
is the math.
