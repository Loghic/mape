# Multi-Asset Pricing Engine

[![CI](https://github.com/Loghic/mape/actions/workflows/ci.yml/badge.svg)](https://github.com/Loghic/mape/actions/workflows/ci.yml)
[![Static analysis](https://github.com/Loghic/mape/actions/workflows/static-analysis.yml/badge.svg)](https://github.com/Loghic/mape/actions/workflows/static-analysis.yml)
[![codecov](https://codecov.io/gh/Loghic/mape/branch/main/graph/badge.svg)](https://codecov.io/gh/Loghic/mape)

[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)](https://en.cppreference.com/w/cpp/20)
[![Rust](https://img.shields.io/badge/Rust-2021-000000?logo=rust&logoColor=white)](https://www.rust-lang.org/)
[![Python](https://img.shields.io/badge/Python-3.9%2B-3776AB?logo=python&logoColor=white)](https://www.python.org/)
[![CMake](https://img.shields.io/badge/CMake-%E2%89%A53.20-064F8C?logo=cmake&logoColor=white)](https://cmake.org/)
[![uv](https://img.shields.io/badge/packaging-uv-DE5FE9?logo=astral&logoColor=white)](https://docs.astral.sh/uv/)

A quantitative pricing engine in **C++20** with a Rust/egui desktop GUI and an
optional real-market-data layer. The C++ core is the "heart" (see
[`plan.md`](plan.md)); a stable C ABI exposes it to Rust, and a small Python
script can feed it live option quotes for implied-volatility analysis.

## What's implemented

| Area | Detail |
|------|--------|
| Domain types | `MarketData`, `Option` (European/American, call/put), `Bond`, `FxForward`, vanilla payoff callables, `std::variant`-based `Instrument` |
| Templated engine | `Pricer<Model>` constrained by C++20 **concepts** (`PricingModel`, `Payoff`, `StochasticProcess`) — one generic engine, no virtual dispatch |
| Models | Black–Scholes–Merton (closed form + analytic Greeks), Cox–Ross–Rubinstein **binomial tree** (American), **Monte Carlo** (templated on process + payoff), **Crank–Nicolson finite-difference PDE** (American) |
| Threading | Parallel Monte Carlo with **independent per-thread RNG streams**, a mutex-guarded **thread pool**, and concurrent **portfolio pricing** |
| Exotics | Path-dependent Monte Carlo (parallel) with **Asian**, **barrier** (knock in/out), and **lookback** payoffs as small `PathPayoff` callables |
| Fixed income | Analytic **fixed-coupon bond** and **FX forward** pricing (covered interest parity) |
| AD Greeks | Forward-mode **automatic differentiation** via dual numbers — exact delta/vega/rho, no bumping |
| Implied vol | Newton + bisection solver inverting Black-Scholes on a market price (uses the AD vega); returns "no solution" when undefined |
| Market data | Optional **SQLite** cache populated by a **yfinance** Python fetcher; GUI plots a **volatility smile** from real option chains |
| FFI bridge | `extern "C"` wrapper (`libmape`) exposing an opaque engine handle and flat C functions — the stable surface Rust binds |
| Rust GUI | `eframe`/`egui` desktop app: single-instrument pricing + Greeks, a threaded portfolio reprice tab, and a **convergence chart** (`egui_plot`) |
| Tests | Dependency-free harnesses (C++ core + a pure-C ABI smoke test) validating analytic benchmarks, Greeks, convergence, exotics parity, serial-vs-parallel agreement, and the C boundary |

The two required language features land as real design choices: **templates**
drive the generic `Pricer` and the Monte Carlo core; **threads** parallelise
Monte Carlo and portfolio pricing.

## Documentation

- [`docs/user-guide.md`](docs/user-guide.md) — plain-language guide to the GUI:
  what each tab does, every parameter explained, how the models work, plus an
  education-only disclaimer and real-world context.
- [`docs/study-guide.md`](docs/study-guide.md) — the math behind each model
  (Black-Scholes, binomial, Monte Carlo, Greeks, implied vol, exotics) mapped
  to the exact MAPE code that implements it.
- [`docs/architecture.md`](docs/architecture.md) — how the layers fit together
  (Mermaid diagrams of components, the pricing call, parallel Monte Carlo, the
  data schema, and the build flow).
- [`docs/cpp20-concepts.md`](docs/cpp20-concepts.md) — in-depth: what the
  `concept` keyword does and the four concepts in use, where the templates live,
  and how the threading works, with code references.
- [`docs/cpp-codemap.md`](docs/cpp-codemap.md) — navigation map of the C++:
  every header and where each feature lives.
- [`docs/cpp-design.md`](docs/cpp-design.md) — C++ design rationale: why
  header-only, concepts vs inheritance, the scalar-generic trick, threading, how
  modules connect, and how the FFI works.
- [`AGENTS.md`](AGENTS.md) — contributor/agent guide: commands, conventions, and
  the gotchas worth knowing before editing.

## Layout

```
core/
  include/mape/        public headers (header-only core)
    instruments.hpp    market_data.hpp  concepts.hpp  pricer.hpp  portfolio.hpp
    models/            black_scholes.hpp  binomial.hpp  monte_carlo.hpp
    threading/         parallel_mc.hpp    thread_pool.hpp
    mape.hpp           umbrella include
  tests/test_main.cpp  test harness
  CMakeLists.txt
ffi/                   extern "C" wrapper around the core -> libmape
  include/mape_c_api.h opaque handle + flat C API (the Rust-facing surface)
  src/mape_c_api.cpp   implementation (catches all exceptions at the boundary)
  tests/c_smoke_test.c pure-C driver proving the ABI links and behaves
  CMakeLists.txt
gui/                   Rust desktop front-end (eframe/egui)
  Cargo.toml
  build.rs             builds the C++ lib via CMake, emits link directives
  src/bridge.rs        safe wrapper over the C API (Drop frees the engine)
  src/main.rs          the egui app
CMakeLists.txt         top-level build
scripts/build_and_test.sh
```

## Build & test

With CMake (≥ 3.20):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Or use the helper script:

```bash
./scripts/build_and_test.sh            # build + run all tests
./scripts/build_and_test.sh --bench    # ...and also run the benchmarks
./scripts/build_and_test.sh -v         # verbose (full compiler/ctest output)
```

`--bench` builds the benchmarks and writes a timestamped CSV to
`bench/results/` (a summary prints to the terminal).

To reformat the C/C++ to the project style (CI enforces it):

```bash
./scripts/format.sh            # fix every file in place
./scripts/format.sh --check    # report-only; what CI runs
```

For a detailed walk-through
of every flag, building without CMake, generating `compile_commands.json` for
your editor, and running the sanitizers, see [`docs/building.md`](docs/building.md).

## Quick example

```cpp
#include "mape/mape.hpp"
using namespace mape;

MarketData mkt{/*spot*/100, /*rate*/0.05, /*vol*/0.20, /*div*/0.0};
Option call{OptionType::Call, Exercise::European, /*strike*/100, /*T*/1.0};

Pricer p{BlackScholes{}};
double price = p.value(call, mkt);   // ~10.45
```

## Using the C API

The FFI layer builds `libmape` and a header (`ffi/include/mape_c_api.h`). A
minimal C consumer:

```c
#include "mape_c_api.h"
MapeEngine* e = mape_create();
double px = mape_price(e, MAPE_MODEL_BLACK_SCHOLES, MAPE_CALL, MAPE_EUROPEAN,
                       /*spot*/100, /*strike*/100, /*rate*/0.05,
                       /*vol*/0.20, /*maturity*/1.0, /*dividend*/0.0);
mape_destroy(e);   /* whoever creates must destroy */
```

No C++ types cross the boundary, exceptions are caught inside the wrapper, and
invalid input returns `NaN` (or a `MapeStatus` code via the `*_ex` variants).

## Benchmarks

Performance benchmarks (plan §12) measure per-model latency, parallel Monte
Carlo scaling, portfolio throughput, the Monte Carlo accuracy/speed trade-off,
and autodiff vs bump-and-revalue Greeks. A representative 4-core run shows
parallel Monte Carlo reaching ~3.9× speedup (≈97% efficiency) at 4 threads and
flattening beyond the physical core count — the Amdahl-law behaviour the
threading design predicts. Build and run instructions are in
[`docs/building.md`](docs/building.md#benchmarks).

The GUI is a Cargo crate under `gui/`. Its `build.rs` invokes CMake to compile
the C++ library, so you need **CMake + a C++20 compiler + the Rust toolchain**
all on PATH. Then:

```bash
cd gui
cargo run --release
```

What happens: `build.rs` configures and builds the `mape_ffi` target via the
`cmake` crate, links the resulting static `libmape`, and pulls in the C++
standard library for your platform (`libstdc++` on Linux, `libc++` on macOS,
MSVC runtime on Windows). The window has six tabs — single-instrument pricing
with closed-form **and exact AD Greeks**, a portfolio tab whose **Reprice all**
button exercises the threaded path, a **Convergence** chart, a **Vol smile** tab
driven by real market data (see below), a **Fixed income** tab (bond + FX
forward), and an **Exotics** tab (Asian / barrier / lookback via parallel Monte
Carlo).

> Crate versions in `Cargo.toml` (`eframe`/`egui` 0.27) are a known-good
> baseline; pin them in `Cargo.lock` (committed by `cargo build`). If you bump
> egui, the small UI API surface used here may need minor adjustments.

## Real market data (optional)

The **Vol smile** tab plots implied volatility extracted from real option
quotes. The data path is deliberately decoupled so the C++ core never touches
the network or a database:

```
yfinance (Python)  →  data/market.db (SQLite)  →  Rust GUI  →  C++ implied_vol
```

To populate it, use [uv](https://docs.astral.sh/uv/) (the Python dependencies
are declared in `pyproject.toml` and locked in `uv.lock`):

```bash
uv sync                                       # create .venv, install locked deps
uv run fetch-data AAPL MSFT --max-expiries 3  # fetch + write data/market.db
```

`uv sync` reads `pyproject.toml`, resolves `yfinance` (and its deps) into a
project-local `.venv`, and writes/uses `uv.lock` for reproducibility. `uv run`
executes the `fetch-data` entry point inside that environment, so nothing is
installed globally. (Plain `pip install yfinance && python scripts/fetch_data.py
...` still works if you prefer.)

This writes `data/market.db` (schema in `data/schema.sql`): a `snapshots` table
(spot, risk-free rate, dividend per ticker+timestamp) and an `option_quotes`
table (strike, expiry, type, market mid-price). History is time-stamped, so
repeated fetches accumulate rather than overwrite.

Then run the GUI **from the repo root** so it finds the DB:

```bash
cargo run --release --manifest-path gui/Cargo.toml
```

In the **Vol smile** tab, pick a ticker and expiry; the GUI loads that chain,
solves Black-Scholes backwards for the implied vol at each strike, and plots the
smile. Strikes with no valid implied vol (price below intrinsic, deep ITM/OTM
noise) are skipped, with a count shown.

> yfinance has no SLA and its schema drifts; the fetcher is best-effort and
> skips bad rows. The risk-free rate defaults to a flat proxy (`--rate` to
> override); a bootstrapped curve is a future enhancement. If `data/market.db`
> is absent, the GUI simply shows setup instructions and the other tabs work
> normally.

## Status

Core, FFI, and the Rust GUI are implemented (plan phases 1–6), plus the §12
stretch goals: exotic path-dependent payoffs, bond/FX pricing, AD Greeks
(delta/vega/rho via dual numbers, gamma via second-order duals), the
implied-vol solver, compile-time pricing (`constexpr`/`consteval`), and the GUI
convergence + volatility-smile charts. The C↔Rust binding signatures are
verified consistent against `mape_c_api.h` (16 functions). The C++ core passes
104 checks and is clean under ThreadSanitizer; the compile-time and C ABI smoke
tests pass.

Remaining ideas (not yet done): yield-curve bootstrapping / multi-curve
discounting, continuous (vs discrete) barrier monitoring, and a counter-based
RNG (Philox) for fully reproducible parallel streams.

## License

[MIT](LICENSE).
