# Architecture

This document describes how the pieces of the multi-asset pricing engine fit
together. For the deep-dive on the C++20 language features (concepts, templates,
threading) see [cpp20-concepts.md](cpp20-concepts.md).

## The big picture

Three languages, three responsibilities, one strict rule: **the C++ core knows
nothing about its consumers.** It never includes a C header, never links Rust,
never opens a socket or a database. Everything flows *toward* the core.

```mermaid
flowchart TD
    subgraph py["Python (data layer)"]
        YF["yfinance fetch<br/>src/mape_data/fetch_data.py"]
    end
    subgraph db["SQLite cache"]
        DB[("data/market.db<br/>snapshots · option_quotes")]
    end
    subgraph rust["Rust GUI (gui/)"]
        DATA["data.rs<br/>rusqlite reader"]
        BRIDGE["bridge.rs<br/>safe FFI wrapper"]
        UI["main.rs<br/>egui tabs"]
    end
    subgraph ffi["C ABI (ffi/)"]
        CAPI["mape_c_api.h / .cpp<br/>opaque handle, flat functions"]
    end
    subgraph core["C++ core (core/) — the heart"]
        ENGINE["Pricer · models · threading<br/>implied_vol · exotics · AD"]
    end

    YF -->|writes| DB
    DB -->|reads| DATA
    DATA --> UI
    UI --> BRIDGE
    BRIDGE -->|extern C| CAPI
    CAPI -->|C++ calls| ENGINE

    classDef heart fill:#1f6feb,stroke:#0b3d91,color:#fff;
    class ENGINE heart;
```

The dependency arrows only point downward/inward. The Python side is a
completely separate process that communicates with the rest exclusively through
the SQLite file — there is no Python ↔ C++ binding.

## Component responsibilities

```mermaid
flowchart LR
    A["pricing-core (C++20)<br/>instruments, models,<br/>templated engine, threads"]
    B["pricing-ffi (C++)<br/>extern C over<br/>opaque handles"]
    C["pricing-gui (Rust)<br/>eframe/egui, links libmape"]
    A --> B --> C
```

- **`core/`** — pure C++20, header-only. Domain types, pricing models, the
  templated `Pricer`, threading utilities, the implied-vol solver, exotics, and
  automatic-differentiation Greeks.
- **`ffi/`** — a thin `extern "C"` layer producing `libmape`. Translates between
  C scalars/enums and C++ types, and catches every exception so none unwinds
  across the language boundary.
- **`gui/`** — the Rust desktop app. `build.rs` compiles the C++ library via
  CMake and links it; `bridge.rs` wraps the raw C calls in a safe API; `data.rs`
  reads the SQLite cache; `main.rs` is the UI.

## Single-instrument pricing call

What happens when a user changes an input in the GUI's *Single* tab:

```mermaid
sequenceDiagram
    participant U as User
    participant UI as main.rs (egui)
    participant W as bridge.rs (safe)
    participant C as mape_c_api.cpp
    participant E as C++ core (Pricer/model)

    U->>UI: edit spot / strike / vol ...
    UI->>W: engine.price(model, type, quote)
    W->>C: mape_price(handle, ..., scalars)
    C->>E: BlackScholes{}.price(opt, mkt)
    E-->>C: double
    C-->>W: double (NaN on bad input)
    W-->>UI: Option<f64>
    UI-->>U: render price + Greeks
```

The same shape applies to Greeks, implied vol, exotics, and portfolio pricing —
only the C function and the core call differ.

## Parallel Monte Carlo (the threading showcase)

The *Reprice all* and exotic-pricing paths fan work across threads, then reduce.
See `core/include/mape/threading/parallel_mc.hpp`.

```mermaid
flowchart TD
    START["monte_carlo_parallel(total_paths, n_threads)"]
    SPLIT["split paths into chunks<br/>(remainder spread over first threads)"]
    SEED["seed_for(base, t)<br/>independent RNG stream per thread"]
    subgraph workers["std::async workers (fan-out)"]
        W0["thread 0<br/>simulate_chunk"]
        W1["thread 1<br/>simulate_chunk"]
        WN["thread N<br/>simulate_chunk"]
    end
    REDUCE["future.get() sum (fan-in)"]
    OUT["discount × mean over ALL paths"]

    START --> SPLIT --> SEED
    SEED --> W0 & W1 & WN
    W0 & W1 & WN --> REDUCE --> OUT
```

The critical correctness detail: each thread gets a **disjoint** random stream
via `seed_for` (a SplitMix64 mix of the base seed and thread index). Sharing a
generator across threads would both race and statistically bias the estimate.
This is verified clean under ThreadSanitizer.

## The data layer

```mermaid
erDiagram
    snapshots ||--o{ option_quotes : has
    snapshots {
        int    id PK
        string ticker
        string fetched_at "ISO-8601 UTC"
        real   spot
        real   rate
        real   dividend
    }
    option_quotes {
        int    id PK
        int    snapshot_id FK
        string expiry
        real   maturity_yrs
        real   strike
        string option_type "call|put"
        real   market_price
    }
```

`fetch_data.py` appends a time-stamped `snapshot` per ticker plus its option
chain. The Rust `data.rs` reads the latest snapshot (via the `latest_snapshots`
view) and feeds each quote's market price to `implied_vol` to build the
volatility smile. Strikes with no valid implied vol are skipped.

## Build flow

```mermaid
flowchart LR
    subgraph cpp["C++ / CMake"]
        CM["CMakeLists.txt"] --> LIB["libmape (static)"]
        LIB --> CT["ctest: core + C smoke"]
    end
    subgraph rs["Rust / Cargo"]
        BR["build.rs"] -->|invokes CMake| LIB
        BR --> LINK["link libmape + C++ stdlib"]
        LINK --> BIN["pricing-gui binary"]
    end
    subgraph pyb["Python / uv"]
        PP["pyproject.toml"] --> VENV[".venv + fetch-data"]
    end
```

The three build systems are independent: CMake builds the core/FFI, Cargo's
`build.rs` reuses CMake to produce `libmape` and links it into the GUI, and uv
manages the Python fetcher. None of them reaches into the others' territory.
