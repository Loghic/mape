# User Guide

A plain-language guide to the desktop app: what each tab does, what every input
means, and how the pricing models actually work. No finance background assumed.

> **Disclaimer — education only.** This project is a learning exercise in C++,
> Rust, and numerical methods. It is **not** financial advice, not a trading
> tool, and not validated for real money. The models are textbook
> approximations (flat rates, no transaction costs, idealised markets); the
> market data is best-effort and may be wrong or stale. Do not make financial
> decisions with it. See [Real-world uses](#real-world-uses) for the honest
> framing.

## Contents

- [The 30-second mental model](#the-30-second-mental-model)
- [Core concepts](#core-concepts)
- [The tabs](#the-tabs)
- [Every parameter, explained](#every-parameter-explained)
- [How the models work](#how-the-models-work)
- [Real-world uses](#real-world-uses)

---

## The 30-second mental model

An **option** is a contract: the right (not obligation) to buy or sell something
(the "underlying" — a stock, an index, a currency) at a fixed price on a future
date. The central question this app answers is: **what is that right worth
today?** That number is the *price* (or *premium*). Everything in the app is
either computing that price a different way, measuring how it changes, or
working it backwards from market data.

---

## Core concepts

### Call vs put

- **Call** — the right to **buy** the underlying at the strike. You want a call
  when you think the price will **rise**: you lock in a low purchase price. A
  call pays off `max(spot − strike, 0)` at expiry.
- **Put** — the right to **sell** the underlying at the strike. You want a put
  when you think the price will **fall**: you lock in a high selling price. A
  put pays off `max(strike − spot, 0)` at expiry.

In short: a call profits from up moves, a put from down moves. The `max(…, 0)`
is the key — an option is never worth less than zero, because you can always
just walk away.

### European vs American exercise

- **European** — can only be exercised **at** the expiry date.
- **American** — can be exercised **any time** up to expiry.

American options are worth at least as much as European (more flexibility), and
need a model that can check early exercise at every step — that's the binomial
tree below.

### Moneyness

- **In-the-money (ITM)** — exercising now would pay off (call with spot above
  strike; put with spot below strike).
- **At-the-money (ATM)** — spot ≈ strike.
- **Out-of-the-money (OTM)** — exercising now pays nothing.

---

## The tabs

| Tab | What it does |
|-----|--------------|
| **Single instrument** | Price one option four ways (Black-Scholes / binomial / Monte Carlo / finite-difference PDE) and see its Greeks — both closed-form and exact (AD). The everyday calculator. With Monte Carlo, a **Deterministic** checkbox switches to a reproducible run whose price is identical regardless of thread count. |
| **Compare** | Price the *same* option with **every** method side by side — Black-Scholes, binomial, Monte Carlo, and finite-difference — with each model's price, its difference from the baseline, and how long the call took. The **Model** you pick is the baseline everything is measured against (choose Black-Scholes for the exact reference, or any model to compare the rest to it). The quickest way to *see* the models agree. |
| **Portfolio** | A "book" is a set of option positions valued together. This tab prices many strikes of one underlying at once. Build the book with a **strike ladder** (min / max / step) or add and edit rows by hand, then **Reprice all** to value the whole book in parallel on the thread pool — the timing shows how that scales. |
| **Convergence** | A chart proving the numerical models (binomial, Monte Carlo, finite-difference) approach the exact Black-Scholes value as you give them more steps / paths / grid resolution. Great for *seeing* how the methods trade speed for accuracy. |
| **Vol smile** | Work the engine **backwards**: take real market option prices, solve for the volatility each implies, and plot it against strike. The resulting "smile" is a real-world phenomenon textbook Black-Scholes can't produce. Needs market data (see the data section in the README). |
| **Calibration** | Fit a smooth **SVI** volatility smile to the implied-vol points from the Vol smile tab, and overlay the fitted curve on the raw market points. Shows the five SVI parameters and the fit error (RMSE). |
| **Risk** | Reprice the Single-tab option under a set of market **stress scenarios** (spot ±10/±20%, a vol shock, a crash combo) on the thread pool, and show each scenario's price and P&L versus the unstressed value. |
| **Fixed income** | Price a fixed-coupon **bond** (present value of its coupons + principal) and an **FX forward** (a contract to exchange currency at a future date). |
| **Exotics** | Price path-dependent options — **Asian**, **barrier**, **lookback** — by simulating many price paths in parallel. |

---

## Every parameter, explained

### Market & contract inputs (Single / Portfolio / Exotics)

| Field | Meaning | Typical value |
|-------|---------|---------------|
| **Spot** | Current price of the underlying, *S*. | e.g. 100 |
| **Strike** | The agreed exercise price in the contract, *K*. | e.g. 100 |
| **Rate** | Risk-free interest rate, *r*, continuously compounded, as a decimal (0.05 = 5%). Used to discount future cashflows to today. | 0.01–0.06 |
| **Volatility** | How much the underlying's price fluctuates, *σ*, annualised, as a decimal (0.20 = 20%). The single biggest driver of an option's value. | 0.10–0.50 |
| **Maturity (yrs)** | Time to expiry, *T*, in years (0.5 = six months). | 0.25–2 |
| **Dividend yield** | Continuous dividend yield, *q*, as a decimal. Dividends paid to holders of the underlying (which option holders don't receive), so they lower a call's value. 0 for a non-dividend stock. | 0–0.03 |
| **Model** | Which method computes the price — see [How the models work](#how-the-models-work). | Black-Scholes |
| **Type** | Call or put (see above). | — |
| **Exercise** | European or American (see above). Only the binomial model honours American. | European |

### Results (Single tab)

- **Price** — the option's fair value today, in the same currency as spot/strike.
- **The Greeks** — sensitivities of the price to each input. They tell you *how
  the price moves* when the market moves, which is how options are risk-managed:

  | Greek | Answers | Sign for a call |
  |-------|---------|-----------------|
  | **Delta** | If spot moves $1, how much does the price move? | 0 to +1 |
  | **Gamma** | How fast does delta itself change? (curvature) | positive |
  | **Vega** | If volatility rises 1 point, how much does the price move? | positive |
  | **Rho** | If the interest rate rises, how much does the price move? | positive for a call |

  The app shows two columns: **closed-form** (the textbook formulas) and **AD
  (exact)** — the same values computed by automatic differentiation, which
  carries derivatives through the calculation with no approximation. They should
  agree; showing both demonstrates the technique and acts as a cross-check.

### Fixed income tab

**Bond:**

| Field | Meaning |
|-------|---------|
| **Face** | The principal repaid at maturity (e.g. 100). |
| **Coupon (annual)** | Annual interest rate the bond pays, as a decimal (0.05 = 5% of face per year). |
| **Maturity (yrs)** | Years until the principal is repaid. |
| **Coupons / year** | Payment frequency (2 = semi-annual, the common convention). |
| **Rate (cont.)** | The discount rate used to value the cashflows. If it equals the coupon, the bond prices near *par* (≈ face). |

**FX forward** (an agreement to swap one currency for another at a future date,
at a rate fixed today):

| Field | Meaning |
|-------|---------|
| **Spot (dom/for)** | Current exchange rate: units of domestic currency per unit of foreign. |
| **Strike** | The exchange rate the contract locks in. |
| **Maturity (yrs)** | When the exchange happens. |
| **Domestic rate** | Risk-free rate in the domestic currency. |
| **Foreign rate** | Risk-free rate in the foreign currency. |

The **fair forward rate** shown is the strike that makes the contract worth
exactly zero today (by covered interest parity). A contract struck cheaper than
fair has positive value to the buyer.

### Exotics tab

Exotic options depend on the *whole path* the price takes, not just the final
value — so they're priced by simulating thousands of paths.

| Field | Meaning |
|-------|---------|
| **Exotic** | Which payoff: **Asian** (uses the average price over the path), **Barrier** (knocks in or out if a level is touched), **Lookback** (uses the path's min/max). |
| **Barrier / Barrier kind** | (Barrier only) the trigger level and rule: *up-and-out* (dies if price rises to the barrier), *down-and-out*, *up-and-in* (only activates if touched), *down-and-in*. |
| **Steps / path** | How many time points each simulated path has. More steps = finer monitoring, slower. |
| **Paths** | How many random paths to simulate. More paths = less Monte Carlo noise, slower. The work is split across CPU threads. |

---

## How the models work

All three price the *same* European option; they differ in method, speed, and
what else they can handle.

### Black–Scholes

A **closed-form formula** — plug in the six inputs, get the price directly. It
assumes the underlying follows a smooth random walk (geometric Brownian motion)
with constant volatility. Instant and exact *for the model's assumptions*, but
it can only handle simple European options. It's the **benchmark** the other two
are checked against.

### Binomial tree (Cox–Ross–Rubinstein)

Models the future as a branching tree: at each small time step the price can go
**up** or **down** by a fixed factor. Starting from the payoffs at expiry, it
works **backwards** to today, averaging and discounting at each node. Because it
visits every node, it can check **early exercise** — which is why it's the model
that handles **American** options. More steps → closer to Black-Scholes (see the
Convergence tab).

### Monte Carlo

**Simulates** many possible random price paths, computes the payoff on each, and
**averages** them, then discounts to today. Slowest and noisiest, but the most
flexible: any payoff you can write as a function of the path can be priced this
way — which is exactly how the **exotics** are handled. The simulation is split
across threads (each with its own independent random stream), and the noise
shrinks as you add paths (Convergence tab).

By default each Monte Carlo run draws fresh randomness, so prices wobble slightly
run to run. Tick **Deterministic** (Single tab, Monte Carlo model) to switch to a
counter-based generator whose draws are a fixed function of the path index: the
price is then exactly reproducible *and* identical at any thread count — useful
when you need a number you can reproduce or compare across machines.

### Finite-difference PDE (Crank–Nicolson)

**Solves the Black-Scholes partial differential equation directly** on a grid of
prices and time steps, marching backward from expiry to today. It's the workhorse
for American options and a precise cross-check on the others: as you refine the
grid (Convergence tab) it converges to the closed-form Black-Scholes value. This
is a *one-dimensional* solver (one underlying); see `plan.md` §16.8 for why the
finite-element / finite-volume cousins only earn their keep on two-dimensional
problems like stochastic-volatility or basket options.

### Implied volatility & the smile (Vol smile tab)

Volatility is the one input you can't observe directly. But the *market* quotes
option **prices**. So you can run Black-Scholes backwards: given a market price,
find the volatility that reproduces it — the **implied volatility**. Plot it for
many strikes and you typically get a curved "smile" or "skew", because real
markets price in fat tails and crash risk that the flat-volatility model
doesn't. Strikes where no valid volatility exists (a price below intrinsic
value, or deep out-of-the-money noise) are skipped.

---

## Real-world uses

In industry, engines like this (vastly more sophisticated) are the core of:

- **Market making & trading** — quoting bid/ask on options and hedging the risk
  using the Greeks (delta-hedging, vega management).
- **Risk management** — a desk revalues its whole book under many scenarios
  (the portfolio + threading pattern here, scaled to millions of positions).
- **Structuring** — pricing bespoke exotic payoffs for clients.
- **Mark-to-market & reporting** — valuing positions daily for P&L and
  regulatory capital.

What this project demonstrates of that world: the templated engine (one pricer,
many models), parallel Monte Carlo, automatic differentiation for exact Greeks,
implied-vol inversion, and a clean C++ core exposed to a GUI over a stable ABI.

**Again, the honest disclaimer:** real pricing systems use calibrated volatility
surfaces, full interest-rate and dividend term structures, careful day-count
conventions, transaction costs, and rigorous validation against live markets —
none of which is production-grade here. This app is for **learning how the
machinery works**, not for trading. Treat every number it produces as an
illustration, not a quote.
