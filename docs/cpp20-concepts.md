# C++20 features in MAPE — a deep dive

This is an in-depth walkthrough of the C++20 language features the project leans
on, **where** they live in the code, and **what** they actually do. Every claim
points at a real file so you can read along.

- [Part 1 — Concepts](#part-1--concepts-the-concept-keyword)
- [Part 2 — Templates](#part-2--templates)
- [Part 3 — Threading](#part-3--threading)
- [Part 4 — Coroutines](#part-4--coroutines)
- [Part 5 — Other C++20 features in use](#part-5--other-c20-features-in-use)

---

## Part 1 — Concepts (the `concept` keyword)

### What a concept *is*

A **concept** is a named, compile-time predicate on types. Before C++20, a
template accepted *any* type and failed deep inside instantiation with a wall of
errors if the type didn't have the right members. A concept lets you state the
requirements up front, so a non-conforming type is rejected at the call site
with a clear message — and the requirement doubles as documentation.

All four concepts in this project live in one file:
[`core/include/mape/concepts.hpp`](../core/include/mape/concepts.hpp).

```mermaid
flowchart TD
    subgraph concepts["concepts.hpp"]
        PM["PricingModel&lt;T&gt;<br/>m.price(opt, mkt) → double"]
        PO["Payoff&lt;T&gt;<br/>p(spot) → double"]
        SP["StochasticProcess&lt;T&gt;<br/>proc.terminal(z) → double"]
        PP["PathPayoff&lt;T&gt;<br/>p(span&lt;double&gt;) → double"]
    end
    PM -->|constrains| PRICER["Pricer&lt;Model&gt;<br/>pricer.hpp"]
    PM -->|constrains| PORT["price_portfolio<br/>portfolio.hpp"]
    SP -->|constrains| MC["monte_carlo_price<br/>monte_carlo.hpp"]
    PO -->|constrains| MC
    SP -->|constrains| PMC["monte_carlo_parallel<br/>parallel_mc.hpp"]
    PO -->|constrains| PMC
    PP -->|constrains| PATHMC["monte_carlo_path_*<br/>path_monte_carlo.hpp / parallel_mc.hpp"]
```

### The `requires` expression

Each concept is written with a `requires` clause that lists expressions the type
must support, and what they must return. Verbatim from `concepts.hpp`:

```cpp
// A PricingModel can value an Option against a MarketData snapshot and return
// something convertible to a price.
template <typename T>
concept PricingModel = requires(const T m, const Option& opt, const MarketData& mkt) {
    { m.price(opt, mkt) } -> std::convertible_to<double>;
};
```

Read it as: "`T` is a `PricingModel` if, given a `const T`, an `Option`, and a
`MarketData`, the expression `m.price(opt, mkt)` is valid **and** its result is
convertible to `double`." The `{ expr } -> Constraint` syntax is a *compound
requirement*: it checks both that the expression compiles and that its type
satisfies the trailing constraint (here the standard-library concept
`std::convertible_to<double>`).

### The four concepts and what each requires

| Concept | Requirement | Who satisfies it | Who consumes it |
|---------|-------------|------------------|-----------------|
| `PricingModel` | `m.price(opt, mkt)` → `double` | `BlackScholes`, `BinomialTree`, `MonteCarlo` | `Pricer<Model>`, `price_portfolio` |
| `Payoff` | `p(spot)` → `double` | `CallPayoff`, `PutPayoff`, `VanillaPayoff` | the Monte Carlo cores |
| `StochasticProcess` | `proc.terminal(z)` → `double` | `GbmProcess` | Monte Carlo cores |
| `PathPayoff` | `p(span<const double>)` → `double` | `AsianPayoff`, `BarrierPayoff`, `LookbackPayoff` | path Monte Carlo |

`Payoff` and `StochasticProcess` verbatim:

```cpp
template <typename T>
concept Payoff = requires(const T p, double spot) {
    { p(spot) } -> std::convertible_to<double>;
};

template <typename T>
concept StochasticProcess = requires(const T proc, double normal_draw) {
    { proc.terminal(normal_draw) } -> std::convertible_to<double>;
};
```

`PathPayoff` is the one that makes exotic, path-dependent options drop in
cheaply — it takes a whole price path as a `std::span<const double>` (also a
C++20 feature):

```cpp
template <typename T>
concept PathPayoff = requires(const T p, std::span<const double> path) {
    { p(path) } -> std::convertible_to<double>;
};
```

### What this buys us

- **Errors at the boundary, not the bowels.** Try to build `Pricer<int>` and you
  get "`int` does not satisfy `PricingModel`" at the declaration, not a cascade
  of template-internal errors.
- **Self-documenting contracts.** A new model author reads `PricingModel` and
  knows exactly the one method to implement — no base class to inherit.
- **No runtime cost.** Concepts are purely compile-time; the generated code is
  identical to an unconstrained template.

---

## Part 2 — Templates

Templates are how the engine prices *any* (model, payoff) pair without
inheritance or virtual dispatch. There are four distinct uses.

### 2.1 The generic engine — `Pricer<Model>`

[`core/include/mape/pricer.hpp`](../core/include/mape/pricer.hpp):

```cpp
template <PricingModel Model>          // constrained template parameter
class Pricer {
public:
    explicit Pricer(Model model) : model_(std::move(model)) {}
    double value(const Option& inst, const MarketData& mkt) const {
        return model_.price(inst, mkt);
    }
private:
    Model model_;
};
template <typename Model> Pricer(Model) -> Pricer<Model>;   // deduction guide
```

`Pricer<BlackScholes>`, `Pricer<BinomialTree>`, and `Pricer<MonteCarlo>` are
three *separate, fully-inlined* types. There is no shared base class and no
`virtual` — the `model_.price(...)` call is resolved and inlined at compile
time. `template <PricingModel Model>` is the constrained form: `Model` must
satisfy the `PricingModel` concept or the program won't compile.

```mermaid
flowchart LR
    BS["BlackScholes"] --> P1["Pricer&lt;BlackScholes&gt;"]
    BT["BinomialTree"] --> P2["Pricer&lt;BinomialTree&gt;"]
    MC["MonteCarlo"] --> P3["Pricer&lt;MonteCarlo&gt;"]
    P1 & P2 & P3 -->|".value(opt, mkt)"| PRICE["double"]
```

### 2.2 The templated Monte Carlo core

[`core/include/mape/models/monte_carlo.hpp`](../core/include/mape/models/monte_carlo.hpp)
is parameterised on *both* the stochastic process and the payoff:

```cpp
template <StochasticProcess Process, Payoff Pay>
double monte_carlo_price(const Process& process, const Pay& payoff,
                         std::size_t paths, double discount,
                         std::uint64_t seed = 12345ULL);
```

Adding a new payoff is just writing a small callable that satisfies `Payoff` (or
`PathPayoff`) — no change to the engine. That is the design the plan calls "a
new payoff is just a small callable."

### 2.3 Scalar-generic Black–Scholes for automatic differentiation

[`core/include/mape/models/black_scholes_ad.hpp`](../core/include/mape/models/black_scholes_ad.hpp)
writes the Black–Scholes formula **once**, templated on the number type:

```cpp
template <typename T>
T bs_price_generic(OptionType type, T S, T K, T r, T q, T sigma, T T_exp);
```

Instantiate with `T = double` and you get the price. Instantiate with `T = Dual`
(a dual number carrying a value *and* a derivative) and the same code computes
the price **and** an exact Greek by the chain rule — no bumping, no finite
differences. This is forward-mode automatic differentiation expressed purely
through templates.

### 2.4 The thread pool's generic `submit`

[`core/include/mape/threading/thread_pool.hpp`](../core/include/mape/threading/thread_pool.hpp)
accepts any callable and deduces its return type:

```cpp
template <typename F>
auto submit(F&& f) -> std::future<std::invoke_result_t<F>>;
```

`std::invoke_result_t<F>` (a C++ type trait) figures out what `f()` returns so
the pool can hand back the right `std::future<R>`.

---

## Part 3 — Threading

Monte Carlo is *embarrassingly parallel* — each simulated path is independent —
which makes it the natural showcase. Two files carry the concurrency.

### 3.1 Parallel Monte Carlo — fan-out / fan-in

[`core/include/mape/threading/parallel_mc.hpp`](../core/include/mape/threading/parallel_mc.hpp).
The structure is: split the paths into chunks, run each chunk on its own thread
via `std::async`, then reduce the partial sums via `std::future::get()`.

```cpp
template <StochasticProcess Process, Payoff Pay>
double monte_carlo_parallel(const Process& process, const Pay& payoff,
                            std::size_t total_paths, double discount,
                            unsigned n_threads = 0,
                            std::uint64_t base_seed = 12345ULL) {
    if (n_threads == 0)
        n_threads = std::max(1u, std::thread::hardware_concurrency());
    // ... split total_paths into chunks (remainder spread over first threads) ...
    std::vector<std::future<double>> futures;
    for (unsigned t = 0; t < n_threads; ++t) {
        const std::uint64_t seed = seed_for(base_seed, t);   // disjoint stream
        futures.push_back(std::async(std::launch::async,
            [&process, &payoff, this_chunk, seed] {
                std::mt19937_64 rng(seed);                   // per-thread RNG
                return simulate_chunk(process, payoff, this_chunk, rng);
            }));
    }
    double sum = 0.0;
    for (auto& f : futures) sum += f.get();                  // reduce
    return discount * (sum / static_cast<double>(total_paths));
}
```

Standard-library threading primitives used here:

- **`std::thread::hardware_concurrency()`** — default the worker count to the
  number of cores.
- **`std::async(std::launch::async, ...)`** — fan-out: each call launches work
  on a new thread and returns a `std::future`.
- **`std::future::get()`** — fan-in: blocks until a worker finishes and returns
  its partial sum.

#### The independent-RNG subtlety

The easy bug here is sharing one random generator across threads — it both data-
races and statistically corrupts the estimate. Instead each thread derives a
**disjoint** stream from a base seed and its index, using a SplitMix64 mix:

```cpp
inline std::uint64_t seed_for(std::uint64_t base, unsigned thread_index) {
    std::uint64_t z = base + 0x9E3779B97F4A7C15ULL * (thread_index + 1);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
```

The single-threaded and multi-threaded results agree with the Black–Scholes
analytic price within Monte Carlo error, and the whole thing is verified clean
under ThreadSanitizer (`-fsanitize=thread`).

```mermaid
sequenceDiagram
    participant Main
    participant T0 as worker 0
    participant T1 as worker 1
    participant T2 as worker 2
    Main->>T0: async(chunk, seed_for(base,0))
    Main->>T1: async(chunk, seed_for(base,1))
    Main->>T2: async(chunk, seed_for(base,2))
    T0-->>Main: future.get() partial sum
    T1-->>Main: future.get() partial sum
    T2-->>Main: future.get() partial sum
    Note over Main: reduce → discount × mean
```

### 3.2 The thread pool — portfolio pricing

[`core/include/mape/threading/thread_pool.hpp`](../core/include/mape/threading/thread_pool.hpp)
is a fixed set of worker threads draining a shared, mutex-guarded task queue.
It prices a whole book of instruments concurrently (one task per instrument).
Primitives used:

- **`std::thread`** — the persistent worker threads (`std::vector<std::thread> workers_`).
- **`std::mutex` + `std::lock_guard` / `std::unique_lock`** — guard the task
  queue against concurrent push/pop.
- **`std::condition_variable`** — workers sleep until there is work or a stop
  signal, instead of busy-waiting.
- **`std::packaged_task` + `std::future`** — wrap each submitted callable so the
  caller gets a future for its result.

```cpp
template <typename F>
auto submit(F&& f) -> std::future<std::invoke_result_t<F>> {
    using R = std::invoke_result_t<F>;
    auto task = std::make_shared<std::packaged_task<R()>>(std::forward<F>(f));
    std::future<R> fut = task->get_future();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.emplace([task] { (*task)(); });
    }
    cv_.notify_one();
    return fut;
}
```

The pool is RAII: its destructor sets a stop flag, notifies all workers, and
joins them, so no thread is left running and no task is half-finished.

```mermaid
flowchart TD
    SUBMIT["submit(task)"] -->|lock mutex, push| QUEUE[("task queue")]
    SUBMIT -->|notify_one| CV{{"condition_variable"}}
    CV --> W0["worker 0"]
    CV --> W1["worker 1"]
    CV --> W2["worker 2"]
    QUEUE --> W0 & W1 & W2
    W0 & W1 & W2 -->|set future value| RES["std::future&lt;R&gt;"]
```

### Where threading is invoked

- **Exotics** (`mape_price_exotic`) and the GUI smile/portfolio paths call
  `monte_carlo_parallel` / `monte_carlo_path_parallel`.
- **Portfolio pricing** (`price_portfolio` in
  [`core/include/mape/portfolio.hpp`](../core/include/mape/portfolio.hpp)) uses
  the `ThreadPool`.
- The opaque FFI engine owns one `ThreadPool` so portfolio calls reuse workers
  across invocations.

---

## Part 4 — Coroutines

C++20 added **coroutines**: functions that can suspend and resume, keeping their
local state alive across suspensions. MAPE uses one to stream Monte Carlo
payoffs lazily.

### What a coroutine *is*

A function becomes a coroutine the moment its body uses `co_yield`, `co_await`,
or `co_return`. The compiler rewrites it into a state machine backed by a
**coroutine frame** (its locals live there, not on the stack), driven by a
**promise type** that you supply. `co_yield v` hands `v` to the caller and
suspends; resuming continues right after the `co_yield`. The upshot: you can
produce a sequence one element at a time, on demand, without ever materialising
the whole thing.

### The generator type

[`core/include/mape/generator.hpp`](../core/include/mape/generator.hpp) is a
minimal `generator<T>` — the hand-rolled C++20 stand-in for C++23's
`std::generator`. Its `promise_type` stores the most recent `co_yield`ed value;
the generator exposes a **standard input iterator** (`begin()`/`end()` with a
`std::default_sentinel_t`), so it plugs into range-based `for` and the
ranges/views machinery:

```cpp
struct promise_type {
    T current_{};
    std::suspend_always yield_value(T value) noexcept {   // co_yield lands here
        current_ = std::move(value);
        return {};                                        // suspend after each
    }
    // initial/final_suspend, get_return_object, unhandled_exception ...
};
```

### Lazy Monte Carlo

[`core/include/mape/models/lazy_monte_carlo.hpp`](../core/include/mape/models/lazy_monte_carlo.hpp)
turns the MC loop into a coroutine that `co_yield`s one discounted payoff per
path:

```cpp
template <StochasticProcess Process, Payoff Pay>
generator<double> mc_payoff_stream(Process process, Pay payoff,
                                   std::size_t paths, double discount,
                                   std::uint64_t seed = 12345ULL) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double> norm(0.0, 1.0);
    for (std::size_t i = 0; i < paths; ++i)
        co_yield discount * payoff(process.terminal(norm(rng)));   // one at a time
}
```

Why it earns its place: the consumer holds **one payoff at a time** — O(1)
memory regardless of path count — and because the generator is a real input
range, you can compose it declaratively:

```cpp
for (double pay : mc_payoff_stream(proc, payoff, N, disc, seed)) sum += pay;
// or with views: ... | std::views::filter(in_the_money) | std::views::transform(...)
```

For the same seed it produces the **identical** estimate to the eager
`monte_carlo_price` — verified in `test_lazy_monte_carlo`. One subtlety the code
is careful about: the RNG and distribution are captured **by value** into the
coroutine frame, because a reference would dangle once the calling scope exits
before the stream is fully consumed.

---

## Part 5 — Other C++20 features in use

Beyond the four above, the engine uses several more C++20 facilities where they
pull weight:

- **`std::latch`** ([`threading/sync_primitives.hpp`](../core/include/mape/threading/sync_primitives.hpp))
  — a one-shot countdown so all parallel-MC workers begin the timed region at
  the same instant (`monte_carlo_parallel_synced`), keeping benchmark timing
  honest.
- **`std::counting_semaphore`** (same file) — caps in-flight tasks in
  `run_bounded`, bounding peak memory when a huge book is repriced at once.
- **`std::span`** ([`exotic.hpp`](../core/include/mape/exotic.hpp),
  [`concepts.hpp`](../core/include/mape/concepts.hpp)) — the `PathPayoff` concept
  takes a `std::span<const double>` over a path, a non-owning view that lets
  exotics read the path without copying.
- **`constexpr`/`consteval`** — compile-time pricing; covered in depth in the
  [study guide](study-guide.md) and [cpp-design](cpp-design.md). The `concept`
  keyword itself (Part 1) is of course also C++20.

---

## Summary: feature → location

| Feature | File(s) |
|---------|---------|
| Concept definitions | `core/include/mape/concepts.hpp` |
| Generic engine | `core/include/mape/pricer.hpp` |
| Templated MC | `core/include/mape/models/monte_carlo.hpp`, `models/path_monte_carlo.hpp` |
| Scalar-generic BS (AD) | `core/include/mape/models/black_scholes_ad.hpp`, `autodiff.hpp` |
| Parallel MC (`std::async`/`future`) | `core/include/mape/threading/parallel_mc.hpp` |
| Thread pool (`thread`/`mutex`/`cv`) | `core/include/mape/threading/thread_pool.hpp` |
| Portfolio pricing via pool | `core/include/mape/portfolio.hpp` |
| Coroutine generator | `core/include/mape/generator.hpp`, `models/lazy_monte_carlo.hpp` |
| `latch` / `counting_semaphore` | `core/include/mape/threading/sync_primitives.hpp` |
| Variadic portfolio (fold) | `core/include/mape/portfolio_compile_time.hpp` |
| CRTP Greeks + capability detection | `core/include/mape/greeks_mixin.hpp` |
| `constexpr`/`consteval` pricing | `core/include/mape/ct_math.hpp`, `compile_time.hpp` |
