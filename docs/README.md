# Documentation

In-depth documentation for the multi-asset pricing engine. Diagrams use
[Mermaid](https://mermaid.js.org/) and render on GitHub.

- **[user-guide.md](user-guide.md)** — plain-language guide to the desktop app:
  what each tab does, every input explained, how the models work, and an
  education-only disclaimer. Start here if you're using the GUI.
- **[architecture.md](architecture.md)** — how the C++ core, C ABI, Rust GUI,
  and Python/SQLite data layer fit together; sequence and data-flow diagrams.
- **[cpp20-concepts.md](cpp20-concepts.md)** — deep dive on the C++20 features:
  what the `concept` keyword does and the four concepts in use, where the
  templates live, and how the threading (parallel Monte Carlo + thread pool)
  works, all with code references.

See also [`../AGENTS.md`](../AGENTS.md) for contributor/agent guidance and
[`../plan.md`](../plan.md) for the original design.
