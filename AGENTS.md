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
  include/mape/   public headers (models/, threading/, concepts, pricer, ...)
  tests/          dependency-free test harness (test_main.cpp)
ffi/         extern "C" wrapper -> libmape. The only surface Rust sees.
  include/mape_c_api.h   flat C API over an opaque handle
  src/, tests/           implementation + a pure-C smoke test
gui/         Rust eframe/egui app. Links libmape via build.rs.
  src/bridge.rs   safe wrapper over the C API (Drop frees the engine)
  src/data.rs     read-only rusqlite access to the market-data cache
  src/main.rs     the egui app (tabs: Single, Portfolio, Convergence, Smile)
data/        schema.sql for the SQLite market-data cache
scripts/     fetch_data.py (yfinance -> SQLite), build_and_test.sh
docs/        architecture + C++20 concepts deep-dive (Mermaid diagrams)
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

Expected: `35 checks, 0 failures` (core) and `PASS (failures: 0)` (C smoke).

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

2. **uv won't rebuild a code-only change.** `uv sync` compares lockfile
   metadata, not file contents, so editing `scripts/fetch_data.py` may not
   trigger a rebuild of the installed module. If you see stale behavior or an
   import/path error referencing `.venv/.../site-packages/`, run
   `rm -rf .venv && uv sync`. The fetcher resolves `data/` relative to the
   current working directory (run it from the repo root), with an embedded
   schema fallback baked into the script.

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

See [`docs/cpp20-concepts.md`](docs/cpp20-concepts.md) for the deep-dive.
