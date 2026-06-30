# Study Guide — the math behind the models, and how MAPE implements it

This is the bridge between the theory and the code. For each model it gives the
**intuition**, the **key formulas** (with notation, no stochastic-calculus
background assumed), and then maps them to the **exact MAPE implementation** so
you can read the math and the code side by side.

For plain-language usage see [user-guide.md](user-guide.md); for software design
see [cpp-design.md](cpp-design.md). This doc sits in between: the *quant*.

- [Foundations: discounting & risk-neutral pricing](#foundations)
- [Black–Scholes](#black–scholes)
- [Binomial tree](#binomial-tree)
- [Monte Carlo](#monte-carlo)
- [Parallel Monte Carlo](#parallel-monte-carlo)
- [Greeks](#greeks)
- [Implied volatility](#implied-volatility)
- [Exotics](#exotics)
- [Symbols](#symbols)
- [Appendix: full derivations](#appendix-full-derivations) *(math-heavy)*

---

## Foundations

Two ideas underpin everything.

**1. Discounting.** Money later is worth less than money now. With a
continuously-compounded risk-free rate *r*, a cashflow *C* at time *T* is worth
`C · e^(−rT)` today. You'll see `exp(-r·T)` everywhere as the discount factor.

**2. Risk-neutral pricing.** The fair price of a derivative is the **expected
payoff, discounted** — but computed in a special "risk-neutral" world where
every asset grows at the risk-free rate *r* (not its real expected return). This
is the no-arbitrage result that makes pricing tractable:

```
price = e^(−rT) · E*[ payoff ]
```

where E* is expectation under the risk-neutral measure. Black–Scholes solves
this expectation in closed form; the binomial tree and Monte Carlo approximate
it. All three are computing the same thing, three ways — which is exactly why
the [Convergence tab](user-guide.md) shows them agreeing.

The underlying is assumed to follow **Geometric Brownian Motion (GBM)**: its log
return over time *T* is normally distributed,

```
S_T = S₀ · exp( (r − q − ½σ²)·T + σ·√T·Z ),   Z ~ N(0,1)
```

The `(r − q − ½σ²)` drift (note the `−½σ²` "Itô correction") and the `σ√T`
diffusion are the two terms you'll recognise in the code below.

---

## Black–Scholes

### Intuition

If the terminal price is log-normally distributed (GBM), the expected
discounted payoff of a European option has a **closed-form** solution — no
simulation, no tree, just a formula. It's the analytical benchmark.

### Formulas

For a call with spot *S*, strike *K*, rate *r*, dividend yield *q*, volatility
*σ*, maturity *T*:

```
d₁ = [ ln(S/K) + (r − q + ½σ²)·T ] / (σ·√T)
d₂ = d₁ − σ·√T

Call = S·e^(−qT)·N(d₁) − K·e^(−rT)·N(d₂)
Put  = K·e^(−rT)·N(−d₂) − S·e^(−qT)·N(−d₁)
```

`N(·)` is the standard normal CDF. Reading it: `N(d₁)` is (roughly) the option's
delta, and `N(d₂)` is the risk-neutral probability the call finishes in the
money; the formula is "expected value received minus expected cost paid,"
discounted.

### In MAPE

The standard normal CDF is built from `erfc` (`models/black_scholes.hpp`):

```cpp
inline double norm_cdf(double x) {
    return 0.5 * std::erfc(-x * M_SQRT1_2);   // N(x) = ½ erfc(−x/√2)
}
```

And the formula itself, written **once** over a generic scalar type so the same
code serves runtime pricing, AD Greeks, and compile-time evaluation
(`models/black_scholes_ad.hpp` — see [cpp-design.md](cpp-design.md) for why):

```cpp
const T d1 = (log(S / K) + (r - q + T(0.5) * sigma * sigma) * T_exp) / (sigma * sqrtT);
const T d2 = d1 - sigma * sqrtT;
auto N = [&](T x) { return T(0.5) * erfc(-x * inv_sqrt2); };
// Call:
return S * df_q * N(d1) - K * df_r * N(d2);
```

where `df_q = e^(−qT)` and `df_r = e^(−rT)`. Compare line-for-line with the
formulas above — the code *is* the math.

---

## Binomial tree

### Intuition

Instead of one big jump to expiry, chop *T* into *n* small steps. At each step
the price either goes **up** by factor *u* or **down** by *d*. This builds a
recombining tree of possible prices. Knowing the payoff at every final node, we
work **backwards**: each node's value is the discounted average of its two
children — and because we visit every node, we can also check **early exercise**
(which is how American options are handled).

### Formulas (Cox–Ross–Rubinstein)

```
dt = T / n
u  = e^(σ·√dt)          (up factor)
d  = 1/u                (down factor — recombining)
a  = e^((r−q)·dt)
p  = (a − d) / (u − d)   (risk-neutral probability of an up move)
```

Terminal price after *j* up-moves out of *n*: `S · uʲ · d^(n−j)`. Backward
induction at each node:

```
V = e^(−r·dt) · [ p·V_up + (1−p)·V_down ]
American:  V = max(V, payoff at this node)   ← early-exercise check
```

As `n → ∞`, the tree price converges to Black–Scholes (the [Convergence
tab](user-guide.md) shows this).

### In MAPE

Straight from `models/binomial.hpp` — note how each symbol matches:

```cpp
const double dt = T / n;
const double u  = std::exp(mkt.vol * std::sqrt(dt));
const double d  = 1.0 / u;
const double a  = std::exp((mkt.rate - mkt.dividend) * dt);
const double p  = (a - d) / (u - d);            // risk-neutral up prob
const double disc = std::exp(-mkt.rate * dt);
// ... terminal layer: spot = S · u^j · d^(n-j) ...
// backward induction:
values[j] = disc * (p * values[j + 1] + (1.0 - p) * values[j]);
if (american)
    values[j] = std::max(values[j], payoff(spot));   // early exercise
```

The default is 512 steps; the tree uses a single `std::vector` rolled back in
place, so memory is O(n), not O(n²).

---

## Monte Carlo

### Intuition

Risk-neutral pricing says price = discounted *expected* payoff. If you can't
solve the expectation analytically, **estimate it**: simulate many random
terminal prices, compute each payoff, average them, discount. The Law of Large
Numbers guarantees the average converges to the true expectation as you add
paths. Slow and noisy, but it works for *any* payoff — which is why it powers the
exotics.

### Formulas

Each path draws `Z ~ N(0,1)` and evolves the spot one step to maturity:

```
S_T = S₀ · exp( (r − q − ½σ²)·T + σ·√T·Z )
price ≈ e^(−rT) · (1/N) · Σ payoff(S_T⁽ⁱ⁾)
```

The estimate has a **standard error of order σ_payoff/√N** — to halve the noise
you need *four times* the paths. That `1/√N` convergence is the fundamental
speed/accuracy trade-off (and exactly what the Convergence tab plots).

### In MAPE

The GBM step is its own small type (`models/monte_carlo.hpp`) — note the drift's
`−½σ²` Itô term:

```cpp
struct GbmProcess {
    double spot, drift, diffusion;
    static GbmProcess from_market(const MarketData& mkt, double T) {
        return {mkt.spot,
                (mkt.rate - mkt.dividend - 0.5 * mkt.vol * mkt.vol) * T,  // drift
                mkt.vol * std::sqrt(T)};                                   // diffusion
    }
    double terminal(double z) const { return spot * std::exp(drift + diffusion * z); }
};
```

The core loop is the formula transcribed:

```cpp
std::mt19937_64 rng(seed);
std::normal_distribution<double> norm(0.0, 1.0);
double sum = 0.0;
for (std::size_t i = 0; i < paths; ++i)
    sum += payoff(process.terminal(norm(rng)));   // S_T then payoff
return discount * (sum / static_cast<double>(paths));   // e^(−rT) · mean
```

It's templated on the *process* and the *payoff*, so the same engine prices a
vanilla option or an exotic — you just hand it a different payoff callable.

---

## Parallel Monte Carlo

### Intuition

Every path is independent, so the work is *embarrassingly parallel*: split the
*N* paths across CPU cores, sum each chunk, then add the chunk sums. The one
trap — and it's a real one — is randomness. If two threads share a generator they
(a) race on its internal state and (b) produce correlated draws that **bias** the
estimate. Each thread must get its own **independent** stream.

### In MAPE

`threading/parallel_mc.hpp` fans out with `std::async` and reduces with
`future::get()`. Each worker seeds its own generator from a SplitMix64 mix of a
base seed and the thread index, guaranteeing disjoint streams:

```cpp
inline std::uint64_t seed_for(std::uint64_t base, unsigned thread_index) {
    std::uint64_t z = base + 0x9E3779B97F4A7C15ULL * (thread_index + 1);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}
// each thread: std::mt19937_64 rng(seed_for(base_seed, t));
```

Because the partial sums are added at the end (not shared during simulation),
there's no shared mutable state in the hot loop — verified race-free under
ThreadSanitizer. The serial and parallel results agree with Black–Scholes within
Monte Carlo error.

---

## Greeks

### Intuition

The Greeks are the **derivatives of the price** with respect to its inputs —
they say how the price moves when the market moves, which is how options are
hedged.

| Greek | Definition | Meaning |
|-------|-----------|---------|
| Delta | ∂Price/∂S | sensitivity to spot |
| Gamma | ∂²Price/∂S² | how delta itself changes (curvature) |
| Vega  | ∂Price/∂σ | sensitivity to volatility |
| Rho   | ∂Price/∂r | sensitivity to the rate |

### Two ways MAPE computes them

**Closed-form** (`models/black_scholes.hpp`) — the analytic derivatives, e.g.
delta = `e^(−qT)·N(d₁)`, gamma = `e^(−qT)·n(d₁)/(S·σ·√T)`, vega =
`S·e^(−qT)·n(d₁)·√T` where `n(·)` is the normal PDF.

**Automatic differentiation** (`autodiff.hpp`) — the elegant approach. A *dual
number* carries a value **and** its derivative; arithmetic propagates the
derivative by the chain rule automatically. Evaluating `bs_price_generic` on a
`Dual` seeded with derivative 1 returns the price *and* the exact Greek, with no
finite-difference error:

```cpp
struct Dual { double v, d; };                 // value, derivative
inline Dual operator*(Dual a, Dual b) {
    return {a.v * b.v, a.d * b.v + a.v * b.d}; // product rule, automatic
}
// delta: seed spot with derivative 1, read .d
double delta(const Option& opt, const MarketData& mkt) const {
    Dual S{mkt.spot, 1.0};
    return bs_price_generic<Dual>(opt.type, S, /*...*/).d;
}
```

Gamma is a *second* derivative, so it uses `Dual2` (value + 1st + 2nd
derivative) with the second-order chain rule — seed spot, read `.dd`. The AD
Greeks match the closed-form values to ~1e-9 (a test asserts this), which is the
whole point: the derivative is *exact*, computed by the same formula as the
price.

---

## Implied volatility

### Intuition

Volatility is the one input you can't see. But the market quotes option
*prices*. So invert the model: given a market price, find the σ that
reproduces it. Plot σ across strikes and you get the **volatility smile** — real
markets bend upward at the wings because they price in crash risk that flat-σ
Black–Scholes ignores.

### Formula / method

There's no closed form for σ, so solve `BS(σ) − market_price = 0` numerically.
MAPE uses **Newton–Raphson** — which needs the derivative of price w.r.t. σ,
i.e. **vega**, which we already get exactly from AD:

```
σ_{k+1} = σ_k − [ BS(σ_k) − price ] / vega(σ_k)
```

with a **bisection fallback** when vega is tiny (deep in/out-of-the-money, where
the price barely responds to σ and Newton stalls).

### In MAPE

`implied_vol.hpp` checks no-arbitrage bounds first (a price below intrinsic or
above the trivial upper bound has *no* valid σ — it returns `std::optional`'s
`nullopt`), seeds with the Brenner–Subrahmanyam ATM approximation, then iterates
Newton with the AD vega and a bracketed bisection guard. When σ is genuinely
undefined (price flat in σ to machine precision) it honestly returns "no
solution" rather than inventing a number — the Vol smile tab then skips that
strike.

---

## Exotics

### Intuition

Exotic options depend on the *whole price path*, not just the final value, so
they can't be priced by a single formula — you simulate the path and apply a
path-dependent payoff. MAPE handles three:

- **Asian** — payoff on the **average** price over the path (averaging dampens
  volatility, so an Asian option is cheaper than its vanilla cousin).
- **Barrier** — knocks **in** or **out** if the price touches a level (e.g. an
  up-and-out call dies if the price rises to the barrier). Up-and-out plus
  up-and-in equals the vanilla — a parity our tests check exactly.
- **Lookback** — payoff against the path's **min or max**.

### In MAPE

The path engine (`models/path_monte_carlo.hpp`) simulates a GBM trajectory step
by step into a buffer, and the payoffs (`exotic.hpp`) are small callables over
that path span — satisfying the `PathPayoff` concept:

```cpp
struct AsianPayoff {
    OptionType type; double strike;
    double operator()(std::span<const double> path) const {
        double avg = /* mean of path */;
        return type == OptionType::Call ? std::max(avg - strike, 0.0)
                                        : std::max(strike - avg, 0.0);
    }
};
```

These run on the same parallel Monte Carlo machinery
(`monte_carlo_path_parallel`) as the vanilla simulation — a new exotic is just a
new payoff struct, no engine change.

---

## Symbols

| Symbol | Meaning |
|--------|---------|
| *S*, *S₀* | spot price (now / at time 0) |
| *S_T* | price at maturity |
| *K* | strike |
| *r* | risk-free rate (continuously compounded) |
| *q* | continuous dividend yield |
| *σ* | volatility (annualised) |
| *T* | time to maturity (years) |
| *N(·)* | standard normal CDF; *n(·)* its PDF |
| *Z* | standard normal random variable, N(0,1) |
| E*[·] | expectation under the risk-neutral measure |
| *p*, *u*, *d* | binomial up-probability, up-factor, down-factor |
| *N* (MC) | number of simulated paths |

---

> **Education only.** These are textbook models with simplifying assumptions
> (constant volatility, flat rates, frictionless markets). Real pricing systems
> use calibrated volatility surfaces, full term structures, and rigorous
> validation. See the disclaimer in [user-guide.md](user-guide.md).

---

# Appendix: full derivations

The main body gave intuition and formulas. This appendix derives them. It
assumes comfort with calculus, basic probability, and a little stochastic
calculus (Itô's lemma is stated where used). Nothing here is needed to *use* the
code — it's for readers who want to see where the formulas come from.

- [A1. GBM and the log-normal terminal distribution](#a1-gbm-and-the-log-normal-terminal-distribution)
- [A2. Black–Scholes by risk-neutral expectation](#a2-blackscholes-by-risk-neutral-expectation)
- [A3. Deriving the Greeks (and put–call parity)](#a3-deriving-the-greeks-and-putcall-parity)
- [A4. The binomial model and its limit](#a4-the-binomial-model-and-its-limit)
- [A5. Monte Carlo: unbiasedness and standard error](#a5-monte-carlo-unbiasedness-and-standard-error)
- [A6. Dual numbers and exact differentiation](#a6-dual-numbers-and-exact-differentiation)

## A1. GBM and the log-normal terminal distribution

Under the risk-neutral measure ℚ, the spot solves the stochastic differential
equation

```
dS_t = (r − q) S_t dt + σ S_t dW_t
```

where W is a ℚ-Brownian motion. To solve it, apply **Itô's lemma** to
`f(S) = ln S`. Itô says for `f(S_t)`:

```
df = f'(S) dS + ½ f''(S) (dS)²,   with (dS)² = σ²S² dt
```

Here `f' = 1/S`, `f'' = −1/S²`, so

```
d(ln S) = (1/S)·[(r−q)S dt + σS dW] + ½(−1/S²)·σ²S² dt
        = (r − q − ½σ²) dt + σ dW.
```

The `−½σ²` term — the **Itô correction** — is the entire reason GBM's drift in
log-space isn't just `r−q`. Integrating from 0 to T (W_T ~ N(0, T), so
`W_T = √T·Z` with Z ~ N(0,1)):

```
ln(S_T / S₀) = (r − q − ½σ²)T + σ√T·Z
⟹  S_T = S₀ · exp( (r − q − ½σ²)T + σ√T·Z ).
```

That is exactly the line in `GbmProcess::terminal` — `drift = (r−q−½σ²)T`,
`diffusion = σ√T`. So `ln S_T` is normal with mean `ln S₀ + (r−q−½σ²)T` and
variance `σ²T`: the terminal price is **log-normal**.

## A2. Black–Scholes by risk-neutral expectation

The call price is the discounted risk-neutral expectation of the payoff:

```
C = e^(−rT) · E*[ max(S_T − K, 0) ].
```

Split the expectation at the exercise boundary `S_T ≥ K`:

```
C = e^(−rT) · ( E*[S_T · 1_{S_T ≥ K}] − K · E*[1_{S_T ≥ K}] ).
```

**Second term (the probability).** Using the log-normal form, `S_T ≥ K` iff
`Z ≥ −d₂` where

```
d₂ = [ ln(S₀/K) + (r − q − ½σ²)T ] / (σ√T).
```

By symmetry of the normal, `ℙ*(Z ≥ −d₂) = N(d₂)`, so the second term is
`K · N(d₂)`.

**First term (the conditional expectation).** We need
`E*[S_T · 1_{S_T ≥ K}]`. Substitute `S_T = S₀ exp((r−q−½σ²)T + σ√T·Z)` and
integrate the standard normal density `φ(z)`:

```
E*[S_T 1_{S_T≥K}] = S₀ e^{(r−q−½σ²)T} ∫_{−d₂}^∞ e^{σ√T·z} φ(z) dz.
```

Complete the square in the exponent — `−½z² + σ√T·z = −½(z − σ√T)² + ½σ²T` — so
the `e^{−½σ²T}` cancels the `+½σ²T`, leaving

```
= S₀ e^{(r−q)T} ∫_{−d₂}^∞ φ(z − σ√T) dz
= S₀ e^{(r−q)T} · N(d₂ + σ√T)
= S₀ e^{(r−q)T} · N(d₁),     where d₁ = d₂ + σ√T.
```

**Assemble**, pulling the `e^{−rT}` discount through:

```
C = e^(−rT)[ S₀ e^{(r−q)T} N(d₁) − K N(d₂) ]
  = S₀ e^{−qT} N(d₁) − K e^{−rT} N(d₂).
```

Which is the formula in `bs_price_generic`. The put follows identically (or from
parity, A3). The shortcut to remember: `d₁` is the "in-the-money expectation"
argument, `d₂` the "in-the-money probability" argument, separated by one σ√T of
drift.

## A3. Deriving the Greeks (and put–call parity)

**Put–call parity.** From the payoff identity
`max(S−K,0) − max(K−S,0) = S − K`, take risk-neutral discounted expectations:

```
C − P = e^(−rT)E*[S_T − K] = S₀ e^{−qT} − K e^{−rT}.
```

(The forward price `E*[S_T] = S₀ e^{(r−q)T}` discounts to `S₀ e^{−qT}`.) This is
the parity our code asserts in tests.

**Delta** = ∂C/∂S₀. A useful fact kills most of the work: the two
`N(·)` terms' S-derivatives cancel, because `S₀ e^{−qT} φ(d₁) = K e^{−rT}
φ(d₂)` (verify by plugging in d₁, d₂). Hence

```
Δ_call = e^{−qT} N(d₁).
```

**Gamma** = ∂²C/∂S₀² = ∂Δ/∂S₀. Differentiating `e^{−qT}N(d₁)` and using
`∂d₁/∂S₀ = 1/(S₀σ√T)`:

```
Γ = e^{−qT} φ(d₁) / (S₀ σ √T).
```

Gamma is the same for call and put (parity differentiates to a constant). It's
what the second-order dual `Dual2` recovers exactly.

**Vega** = ∂C/∂σ. Again the φ-terms cancel except through the explicit σ
dependence, giving

```
ν = S₀ e^{−qT} φ(d₁) √T.
```

These are the closed-form expressions in `black_scholes.hpp` — and the same
numbers the AD path produces by differentiating `bs_price_generic` directly
(A6), which is why the test asserts they agree to ~1e-9.

## A4. The binomial model and its limit

**One-step risk-neutral probability.** Over `dt`, the spot goes to `S·u` or
`S·d`. No-arbitrage requires the discounted expected price to equal the forward,
`E*[S_{dt}] = S·e^{(r−q)dt}`:

```
p·u + (1−p)·d = e^{(r−q)dt}  ⟹  p = (e^{(r−q)dt} − d)/(u − d).
```

That's the `p` in `binomial.hpp`. A one-step option value is then the discounted
expectation `e^{−r dt}[p V_u + (1−p)V_d]`, applied recursively backward through
the tree — the inner loop.

**CRR factor choice.** Cox–Ross–Rubinstein pick `u = e^{σ√dt}`, `d = 1/u`. Why
this matches GBM: over one step the log-return has variance `≈ p(1−p)(ln u − ln
d)² = p(1−p)(2σ√dt)²`. As `dt→0`, `p→½`, so the variance → `σ²dt` — matching the
GBM increment's variance. The mean is matched by the choice of `p`. With both
the first two moments of the log-return matched per step, the **Central Limit
Theorem** makes the n-step terminal log-price converge to the normal of A1,
hence the tree price converges to Black–Scholes. The convergence is `O(1/n)`
(with the characteristic even/odd oscillation), which is what the Convergence
tab shows.

## A5. Monte Carlo: unbiasedness and standard error

Let `X_i = payoff(S_T⁽ⁱ⁾)` for i.i.d. draws, and the estimator be
`Ĉ = e^{−rT} · (1/N)ΣX_i`.

**Unbiased.** By linearity, `E[Ĉ] = e^{−rT} E[X] = e^{−rT}E*[payoff] = C`
exactly. The estimator targets the true price with no bias (for the Euler-exact
GBM step we use, there's no discretisation bias either — we sample S_T directly,
not a stepped path, except in the path-dependent exotic engine).

**Standard error.** With `Var(X) = s²`, independence gives
`Var(Ĉ) = e^{−2rT} s²/N`, so the standard error is

```
SE(Ĉ) = e^{−rT} · s / √N.
```

Two consequences the code and the Convergence tab make visible: error shrinks
like `1/√N` (quadrupling paths halves the error — the fundamental cost of Monte
Carlo), and the result is a *random* estimate whose noise band you can quote.
The `seed_for` independent streams (A-independence across threads) are what keep
the `Var(Ĉ) = s²/N` formula valid in the parallel version — correlated streams
would inflate the variance.

## A6. Dual numbers and exact differentiation

A **dual number** adjoins an element ε with `ε² = 0` (nilpotent), writing
`a + bε`. Evaluate any analytic f at `x + ε`:

```
f(x + ε) = f(x) + f'(x)ε + ½f''(x)ε² + … = f(x) + f'(x)ε,
```

because `ε² = 0` truncates everything past the linear term. So the ε-coefficient
of `f(x + ε)` is **exactly** `f'(x)` — no limit, no step size, no truncation
error. The arithmetic rules fall out of multiplying out and dropping `ε²`:

```
(a+bε)(c+dε) = ac + (ad+bc)ε        ⟹ product rule
(a+bε)/(c+dε) = a/c + (bc−ad)/c² ε  ⟹ quotient rule
```

which is exactly `operator*` and `operator/` on `Dual`. Seeding the input as
`x + 1·ε` (derivative 1) and reading the ε-part after evaluating
`bs_price_generic` yields the exact partial derivative — that's delta/vega/rho.

**Second order** uses a truncation at `ε³ = 0` instead, carrying
`f(x) + f'(x)ε + f''(x)ε²/… ` — `Dual2` stores `(v, d, dd)` and its operators
implement the second-order product/quotient/chain rules (e.g.
`(fg)'' = f''g + 2f'g' + fg''`). Seeding spot with first-derivative 1 and reading
`dd` gives gamma exactly. This is why the AD and closed-form Greeks agree to
machine precision: they are the *same* derivative, one computed symbolically by
hand (A3), the other by the algebra of ε.
