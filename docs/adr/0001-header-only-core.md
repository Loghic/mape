# ADR 0001 — A header-only C++ core

**Status:** accepted

## Context

The pricing core (`core/include/mape/`) needs to be consumed by a test harness,
a C ABI / FFI layer, a benchmark harness, and potentially future bindings. We
had to decide whether to ship it as a compiled library (`.a`/`.so`) or as
headers only.

## Decision

Make the entire core **header-only** — no `.cpp` files in `core/`. The only
compiled C++ translation unit in the project is the FFI wrapper
(`ffi/src/mape_c_api.cpp`).

## Rationale

- The core is overwhelmingly **templates** (`Pricer<Model>`,
  `bs_price_generic<T>`, the Monte Carlo cores, the thread pool's `submit`).
  Templates must be visible at the point of instantiation, so a separate `.cpp`
  would buy little.
- Consumption is trivial: `-Icore/include` and `#include "mape/mape.hpp"`. No
  library to build or link for the pure-C++ path; the test harness compiles with
  one `g++` command, which keeps CI cheap and the sandbox workflow simple.
- The cost of template instantiation is paid once, at the FFI boundary — the one
  place a compiled artifact (`libmape`) is actually needed.

## Consequences

- Every header must be **self-contained** (own include guard, includes exactly
  the standard headers it uses). A header that compiles only because another
  pulled in `<algorithm>` is a latent bug — libstdc++ and libc++ differ on
  transitive includes, which is why CI builds on both (see ADR 0002 and the
  clang/libc++ CI leg).
- Compile times are bounded because only the FFI TU instantiates the templates
  for the exported entry points.
- If the core ever grows non-template, non-trivial implementation, revisit:
  some of it could move to a compiled `mape_core` target without changing the
  public API.
