# Documentation

In-depth documentation for the multi-asset pricing engine. Diagrams use
[Mermaid](https://mermaid.js.org/) and render on GitHub.

- **[user-guide.md](user-guide.md)** — plain-language guide to the desktop app:
  what each tab does, every input explained, how the models work, and an
  education-only disclaimer. Start here if you're using the GUI.
- **[study-guide.md](study-guide.md)** — the quant bridge: the math behind
  Black-Scholes, the binomial tree, Monte Carlo, the Greeks, implied vol, and
  exotics — each with formulas *and* the exact MAPE code that implements it.
- **[building.md](building.md)** — build & test in depth: every CMake flag,
  building without CMake, `compile_commands.json` for your editor, sanitizers,
  and benchmarks.
- **[architecture.md](architecture.md)** — how the C++ core, C ABI, Rust GUI,
  and Python/SQLite data layer fit together; sequence and data-flow diagrams.
- **[cpp20-concepts.md](cpp20-concepts.md)** — deep dive on the C++20 features:
  what the `concept` keyword does and the four concepts in use, where the
  templates live, and how the threading (parallel Monte Carlo + thread pool)
  works, all with code references.
- **[cpp-codemap.md](cpp-codemap.md)** — navigation guide to the C++: every
  header, what's in it, and where to find each feature. Start here to find
  something in the code.
- **[cpp-design.md](cpp-design.md)** — in-depth design rationale: why
  header-only, why concepts over inheritance, the scalar-generic "one formula,
  three modes" trick, threading choices, how the modules connect, and how the
  FFI works.

See also [`../AGENTS.md`](../AGENTS.md) for contributor/agent guidance and
[`../plan.md`](../plan.md) for the original design.
