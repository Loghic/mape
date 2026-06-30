# ADR 0002 — A flat C ABI as the only external surface

**Status:** accepted

## Context

The C++ core uses templates and C++ types (`std::variant`, `std::optional`,
concepts) that can't cross a language boundary. The Rust GUI — and any future
non-C++ consumer — needs a stable way to call the engine.

## Decision

Expose the engine through a **flat C ABI** (`ffi/include/mape_c_api.h`):
`extern "C"` functions over an **opaque handle** (`MapeEngine*`), taking only
plain scalars, enums, and pointers. No C++ type crosses the boundary.

## Rationale

- A C ABI is the lowest common denominator every language can call. It decouples
  the volatile C++ internals from consumers — the templated `Pricer<Model>` can
  change freely behind the stable handle.
- The opaque handle lets the engine hold state (a reusable thread pool for
  portfolio pricing) without exposing its layout.
- Exception safety is explicit: every entry point wraps its body in
  `try/catch(...)` and converts failure to `NaN` or a `MapeStatus` code, because
  an exception unwinding across `extern "C"` is undefined behaviour.

## Consequences

- A small runtime-dispatch shim (the Bridge, plan §15.5) is needed to map the
  compile-time `Pricer<Model>` templates to the flat runtime API the GUI's model
  dropdown selects.
- The C header and the Rust `extern "C"` block must stay in lockstep; CI runs
  `scripts/check_abi_consistency.py` to enforce it (21 functions, matched by
  name/arity/type).
- Ownership is caller-managed (`mape_create`/`mape_destroy`); the Rust wrapper
  encodes this with `Drop` so it's leak- and double-free-safe from safe Rust.
