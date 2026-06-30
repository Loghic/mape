# C++ Code Map

A navigation guide to the C++ codebase: every header, what lives in it, and
where to find each feature. The core is **header-only** (all logic in
`core/include/mape/`); the FFI is the only compiled C++ translation unit.

For *why* it's built this way, see [cpp-design.md](cpp-design.md). For the
language-feature deep dive (concepts/templates/threading), see
[cpp20-concepts.md](cpp20-concepts.md).

## Directory layout

```
core/include/mape/
├── mape.hpp                    umbrella header — includes everything
├── market_data.hpp             MarketData (spot, rate, vol, dividend)
├── instruments.hpp             Option, Bond, FxForward, payoff callables
├── concepts.hpp                PricingModel, Payoff, StochasticProcess, PathPayoff
├── pricer.hpp                  Pricer<Model> — the generic engine
├── portfolio.hpp               price_portfolio() over a thread pool
├── autodiff.hpp                Dual, Dual2 — forward-mode AD scalars
├── ct_math.hpp                 constexpr sqrt/exp/log/erfc + CtDouble
├── compile_time.hpp            constexpr Black-Scholes, consteval validator
├── implied_vol.hpp             implied_vol() — invert BS for vol
├── exotic.hpp                  Asian/barrier/lookback PathPayoffs
├── models/
│   ├── black_scholes.hpp       BlackScholes (closed form + Greeks)
│   ├── black_scholes_ad.hpp    bs_price_generic<T> + BlackScholesAD
│   ├── binomial.hpp            BinomialTree (CRR, American)
│   ├── monte_carlo.hpp         MonteCarlo + GbmProcess + monte_carlo_price
│   ├── path_monte_carlo.hpp    GbmPathGenerator + monte_carlo_path_price
│   └── fixed_income.hpp        price_bond, price_fx_forward, fx_forward_rate
└── threading/
    ├── thread_pool.hpp         ThreadPool (workers + queue)
    └── parallel_mc.hpp         monte_carlo_parallel, monte_carlo_path_parallel

ffi/
├── include/mape_c_api.h        the flat C ABI (opaque handle + functions)
├── src/mape_c_api.cpp          implementation over the core
└── tests/c_smoke_test.c        pure-C consumer test

core/tests/
├── test_main.cpp               runtime test harness (36 checks)
└── test_compile_time.cpp       compile-time (static_assert) tests
```

## Where to find each feature

| Feature | File(s) | Key symbol(s) |
|---------|---------|---------------|
| Market snapshot | `market_data.hpp` | `MarketData` |
| Instruments | `instruments.hpp` | `Option`, `Bond`, `FxForward`, `Instrument` (variant) |
| Payoffs (vanilla) | `instruments.hpp` | `CallPayoff`, `PutPayoff`, `VanillaPayoff` |
| Payoffs (exotic) | `exotic.hpp` | `AsianPayoff`, `BarrierPayoff`, `LookbackPayoff` |
| Concepts | `concepts.hpp` | `PricingModel`, `Payoff`, `StochasticProcess`, `PathPayoff` |
| Generic engine | `pricer.hpp` | `Pricer<Model>` |
| Black-Scholes (runtime) | `models/black_scholes.hpp` | `BlackScholes`, `norm_cdf`, `norm_pdf` |
| Scalar-generic BS | `models/black_scholes_ad.hpp` | `bs_price_generic<T>` |
| AD Greeks | `models/black_scholes_ad.hpp` + `autodiff.hpp` | `BlackScholesAD`, `Dual`, `Dual2` |
| Binomial tree | `models/binomial.hpp` | `BinomialTree` |
| Monte Carlo (terminal) | `models/monte_carlo.hpp` | `MonteCarlo`, `GbmProcess`, `monte_carlo_price` |
| Monte Carlo (path) | `models/path_monte_carlo.hpp` | `GbmPathGenerator`, `monte_carlo_path_price` |
| Parallel MC | `threading/parallel_mc.hpp` | `monte_carlo_parallel`, `monte_carlo_path_parallel`, `seed_for` |
| Thread pool | `threading/thread_pool.hpp` | `ThreadPool` |
| Portfolio pricing | `portfolio.hpp` | `price_portfolio` |
| Implied volatility | `implied_vol.hpp` | `implied_vol` |
| Bond / FX | `models/fixed_income.hpp` | `price_bond`, `price_fx_forward`, `fx_forward_rate` |
| Compile-time math | `ct_math.hpp` | `sqrt_ct`, `exp_ct`, `log_ct`, `erfc_ct`, `CtDouble` |
| Compile-time pricing | `compile_time.hpp` | `ct::bs_call`, `ct::make_option` (consteval) |
| C ABI | `ffi/include/mape_c_api.h` | `mape_*` functions, `MapeEngine` (opaque) |

## How to include it

Pull in the whole public API with the umbrella header:

```cpp
#include "mape/mape.hpp"   // -Icore/include
```

…or include just what you need (each header is self-contained with its own
include guard and includes exactly the standard headers it uses):

```cpp
#include "mape/models/black_scholes.hpp"
#include "mape/pricer.hpp"
```

## Reading order (if you're new to the code)

1. `market_data.hpp` + `instruments.hpp` — the domain vocabulary.
2. `concepts.hpp` — the contracts models and payoffs satisfy.
3. `pricer.hpp` — how the generic engine ties a model to those contracts.
4. `models/black_scholes.hpp` — the simplest concrete model.
5. `models/black_scholes_ad.hpp` — the scalar-generic version that also powers
   AD and compile-time pricing (one formula, three modes).
6. `threading/parallel_mc.hpp` — the threading showcase.
7. `ffi/src/mape_c_api.cpp` — how all of it is exposed to the outside world.
