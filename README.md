# Multi-Asset Pricing Engine

A quantitative pricing engine in **C++20** with a Rust/egui desktop GUI and an
optional real-market-data layer. The C++ core is the "heart" (see
[`plan.md`](plan.md)); a stable C ABI exposes it to Rust, and a small Python
script can feed it live option quotes for implied-volatility analysis.

## What's implemented

| Area | Detail |
|------|--------|
| Domain types | `MarketData`, `Option` (European/American, call/put), `Bond`, `FxForward`, vanilla payoff callables, `std::variant`-based `Instrument` |
| Templated engine | `Pricer<Model>` constrained by C++20 **concepts** (`PricingModel`, `Payoff`, `StochasticProcess`) — one generic engine, no virtual dispatch |
| Models | Black–Scholes–Merton (closed form + analytic Greeks), Cox–Ross–Rubinstein **binomial tree** (American exercise), **Monte Carlo** (templated on process + payoff) |
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

- [`docs/architecture.md`](docs/architecture.md) — how the layers fit together
  (Mermaid diagrams of components, the pricing call, parallel Monte Carlo, the
  data schema, and the build flow).
- [`docs/cpp20-concepts.md`](docs/cpp20-concepts.md) — in-depth: what the
  `concept` keyword does and the four concepts in use, where the templates live,
  and how the threading works, with code references.
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

### What each command does

CMake works in two stages: a **configure** stage that reads the
`CMakeLists.txt` files and generates a build system (e.g. Makefiles), and a
**build** stage that actually compiles.

`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` — the *configure* step.

- `-S .` — **S**ource directory: where the top-level `CMakeLists.txt` lives
  (`.` = the current folder).
- `-B build` — **B**uild directory: where CMake writes all generated files and
  compiled output. Keeping it separate from the source ("out-of-source build")
  means you can delete `build/` anytime to start clean without touching code.
- `-DCMAKE_BUILD_TYPE=Release` — sets a CMake cache variable. `-D<NAME>=<value>`
  is how you pass options. `Release` turns on optimisation (`-O3`) and disables
  debug assertions; use `Debug` instead for debugging symbols and no
  optimisation.

`cmake --build build -j` — the *build* step.

- `--build build` — compile the project that was configured into the `build`
  directory. (This calls the underlying make/ninja for you, so the commands are
  the same on every platform.)
- `-j` — build in parallel using all available CPU cores. You can also write
  `-j8` to cap it at 8 jobs.

`ctest --test-dir build --output-on-failure` — run the tests.

- `--test-dir build` — point ctest at the build directory (where the test
  registry was generated).
- `--output-on-failure` — print a test's full output only if it fails; passing
  tests stay quiet. Add `-V` if you want output from every test, pass or fail.

> If ctest ever reports **"No tests were found"**, delete the build directory
> and reconfigure (`rm -rf build` then re-run the configure step). ctest reads
> a cached test registry, so it must be regenerated after CMake changes.

### Without CMake

You can compile the tests directly — the core is header-only, so one `g++`
call is enough:

```bash
g++ -std=c++20 -O2 -pthread -Icore/include core/tests/test_main.cpp -o mape_tests
./mape_tests
```

- `-std=c++20` — the core uses C++20 concepts, so this is required.
- `-O2` — optimisation (Monte Carlo runs much faster).
- `-pthread` — links the threading runtime (`std::thread`, `std::async`).
- `-Icore/include` — adds the header search path so `#include "mape/..."`
  resolves.

Either path is wrapped by `scripts/build_and_test.sh`.

### Editing the code: generating `compile_commands.json`

Editors and language servers (VS Code C/C++, clangd, CLion) understand your
include paths and compiler flags by reading a `compile_commands.json` file — a
"compilation database". Generate it by adding one flag at the configure step:

```bash
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

This writes `build/compile_commands.json`. Most tools look for it at the project
root, so symlink it there once:

```bash
ln -sf build/compile_commands.json compile_commands.json
```

After that, code navigation, autocomplete, and inline error checking will work
in your editor. Re-run the configure step whenever you add new source files so
the database stays current.

> Note: the core is header-only, so the database is most useful once the FFI
> source files (`ffi/src/*.cpp`) are part of the build. If you use **clangd**,
> that's all you need; for the **VS Code C/C++ extension**, point
> `C_Cpp.default.compileCommands` at the file in your settings.

### Sanitizers

The concurrency tests are clean under ThreadSanitizer:

```bash
g++ -std=c++20 -O1 -g -fsanitize=thread -pthread -Icore/include \
    core/tests/test_main.cpp -o mape_tsan && ./mape_tsan
```

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

## Running the GUI (Rust)

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
MSVC runtime on Windows). The window has four tabs — single-instrument pricing
with Greeks, a portfolio tab whose **Reprice all** button exercises the threaded
path, a **Convergence** chart, and a **Vol smile** tab driven by real market
data (see below).

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
stretch goals: exotic path-dependent payoffs, bond/FX pricing, AD Greeks, and
the GUI convergence chart. The C↔Rust binding signatures are verified
consistent against `mape_c_api.h` (15 functions). The C++ core passes 28 checks
and is clean under ThreadSanitizer; the C ABI smoke test passes.

Remaining ideas (not yet done): yield-curve bootstrapping / multi-curve
discounting, continuous (vs discrete) barrier monitoring, and a counter-based
RNG (Philox) for fully reproducible parallel streams.
