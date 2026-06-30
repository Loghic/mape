# ADR 0003 — A Rust + egui desktop GUI

**Status:** accepted

## Context

The engine needs a front-end so a user can enter parameters and see prices,
Greeks, a convergence chart, and a volatility smile. Options considered: a
Python/Qt or Python/Tk app, a web UI, a Dear ImGui (C++) app, or a Rust GUI.

## Decision

Build the GUI in **Rust with `eframe`/`egui`**, linking the C++ `libmape` over
the C ABI (ADR 0002) via a `build.rs` that drives CMake.

## Rationale

- **Single compiled binary, no runtime to ship** — unlike Python, which needs an
  interpreter and environment on the target machine.
- **Clean, safe FFI to C** — Rust's `extern "C"` + a thin safe wrapper
  (`bridge.rs`) gives memory-safe access to the engine, with `Drop` enforcing
  the C ABI's ownership rule.
- **immediate-mode UI** — egui is simple for the kind of form-and-chart UI here,
  with `egui_plot` covering the convergence and smile charts.
- It keeps the engine's separation intact: the GUI does no math, only formats
  inputs, calls the FFI, and renders results.

## Consequences

- Building the GUI requires three toolchains on PATH (CMake + a C++20 compiler +
  Rust); `build.rs` isolates the platform-specific linking (libstdc++ / libc++ /
  MSVC runtime).
- A third language enters the project, but it stays confined to `gui/`; the core
  and FFI remain pure C++.
- Python is still used, but only for the optional, decoupled market-data fetcher
  (writes SQLite) — never linked into the engine.

## Alternatives rejected

- **PyBind11 + Python UI** — faster to prototype, but ships a runtime and loses
  the single-binary packaging.
- **Dear ImGui in C++** — would avoid the FFI entirely, but the FFI boundary is
  itself a goal of the project (a real, stable C ABI), and Rust's safety at that
  seam is a feature, not a cost.
