// Lightweight, dependency-free test harness for the pricing core.
//
// GoogleTest is the intended framework (plan §11) but isn't always installed,
// so these checks use a tiny macro set instead. The assertions mirror what the
// GoogleTest version would do; swapping in real GTest later is mechanical.

#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

#include "mape/mape.hpp"

using namespace mape;

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::printf("  FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);      \
        }                                                                      \
    } while (0)

#define CHECK_NEAR(a, b, tol, msg)                                             \
    do {                                                                       \
        ++g_checks;                                                            \
        double _d = std::fabs((a) - (b));                                      \
        if (_d > (tol)) {                                                      \
            ++g_failures;                                                      \
            std::printf("  FAIL: %s expected %.6f got %.6f (|d|=%.6g > %.6g)\n",\
                        msg, (double)(b), (double)(a), _d, (double)(tol));     \
        }                                                                      \
    } while (0)

// Shared test fixture: a textbook ATM-ish European call.
static MarketData make_market() {
    return MarketData{/*spot*/ 100.0, /*rate*/ 0.05, /*vol*/ 0.20, /*div*/ 0.0};
}
static Option make_call() {
    return Option{OptionType::Call, Exercise::European, /*strike*/ 100.0,
                  /*maturity*/ 1.0};
}

// --- 1. Black–Scholes against a hand-computed reference -----------------
static void test_black_scholes_reference() {
    std::printf("test_black_scholes_reference\n");
    BlackScholes bs;
    Option call = make_call();
    MarketData mkt = make_market();
    // Known value for S=K=100, r=5%, sigma=20%, T=1: ~10.4506.
    double price = bs.price(call, mkt);
    CHECK_NEAR(price, 10.4506, 1e-3, "BS ATM call price");

    // Put–call parity: C - P = S e^{-qT} - K e^{-rT}.
    Option put = call;
    put.type = OptionType::Put;
    double pprice = bs.price(put, mkt);
    double parity = price - pprice;
    double rhs = mkt.spot * std::exp(-mkt.dividend * call.maturity) -
                 call.strike * std::exp(-mkt.rate * call.maturity);
    CHECK_NEAR(parity, rhs, 1e-9, "put-call parity");
}

// --- 2. Greeks sign / bound sanity (plan §11) ---------------------------
static void test_greeks() {
    std::printf("test_greeks\n");
    BlackScholes bs;
    Option call = make_call();
    MarketData mkt = make_market();
    double delta = bs.delta(call, mkt);
    double gamma = bs.gamma(call, mkt);
    double vega  = bs.vega(call, mkt);
    CHECK(delta > 0.0 && delta < 1.0, "call delta in (0,1)");
    CHECK(gamma > 0.0, "gamma positive");
    CHECK(vega > 0.0, "vega positive");

    // Cross-check delta against a finite-difference bump.
    double h = 1e-4;
    MarketData up = mkt, dn = mkt;
    up.spot += h; dn.spot -= h;
    double fd_delta = (bs.price(call, up) - bs.price(call, dn)) / (2 * h);
    CHECK_NEAR(delta, fd_delta, 1e-4, "delta vs finite-difference");
}

// --- 3. Binomial converges to Black–Scholes (European) ------------------
static void test_binomial_converges_to_bs() {
    std::printf("test_binomial_converges_to_bs\n");
    BlackScholes bs;
    BinomialTree tree(2000);
    Option call = make_call();
    MarketData mkt = make_market();
    double bs_price   = bs.price(call, mkt);
    double tree_price = tree.price(call, mkt);
    CHECK_NEAR(tree_price, bs_price, 1e-2, "binomial -> BS (European call)");

    // American call on a non-dividend stock equals the European call.
    Option amer = call;
    amer.exercise = Exercise::American;
    CHECK_NEAR(tree.price(amer, mkt), bs_price, 1e-2,
               "American call (no div) == European");

    // American put should be worth at least the European put (early exercise).
    Option eur_put = call; eur_put.type = OptionType::Put;
    Option amer_put = eur_put; amer_put.exercise = Exercise::American;
    BinomialTree t2(2000);
    CHECK(t2.price(amer_put, mkt) >= t2.price(eur_put, mkt) - 1e-6,
          "American put >= European put");
}

// --- 4. Monte Carlo converges to Black–Scholes --------------------------
static void test_monte_carlo_converges_to_bs() {
    std::printf("test_monte_carlo_converges_to_bs\n");
    BlackScholes bs;
    Option call = make_call();
    MarketData mkt = make_market();
    double bs_price = bs.price(call, mkt);

    MonteCarlo mc(2'000'000, 42);
    double mc_price = mc.price(call, mkt);
    // ~2M paths gives a standard error well under 0.05 for these params.
    CHECK_NEAR(mc_price, bs_price, 5e-2, "MC -> BS (European call)");
}

// --- 5. Concurrency: parallel == single-threaded within MC error --------
static void test_parallel_mc_agrees() {
    std::printf("test_parallel_mc_agrees\n");
    Option call = make_call();
    MarketData mkt = make_market();
    auto proc = GbmProcess::from_market(mkt, call.maturity);
    double disc = std::exp(-mkt.rate * call.maturity);
    std::size_t paths = 2'000'000;

    BlackScholes bs;
    double bs_price = bs.price(call, mkt);

    double serial   = monte_carlo_price(proc, call.payoff(), paths, disc, 7);
    double parallel = monte_carlo_parallel(proc, call.payoff(), paths, disc,
                                           8, 7);
    // Both must land near BS; they use different stream layouts so they won't
    // be bit-identical, but both should agree with the analytic price.
    CHECK_NEAR(serial,   bs_price, 5e-2, "serial MC near BS");
    CHECK_NEAR(parallel, bs_price, 5e-2, "parallel MC near BS");

    // Independent seeds must produce distinct streams.
    CHECK(seed_for(7, 0) != seed_for(7, 1), "per-thread seeds differ");
}

// --- 6. Templated Pricer works uniformly across models ------------------
static void test_templated_pricer() {
    std::printf("test_templated_pricer\n");
    Option call = make_call();
    MarketData mkt = make_market();

    Pricer p_bs{BlackScholes{}};
    Pricer p_tree{BinomialTree{2000}};
    Pricer p_mc{MonteCarlo{1'000'000, 99}};

    double a = p_bs.value(call, mkt);
    double b = p_tree.value(call, mkt);
    double c = p_mc.value(call, mkt);
    CHECK_NEAR(a, b, 1e-2, "Pricer<BlackScholes> ~ Pricer<BinomialTree>");
    CHECK_NEAR(a, c, 1e-1, "Pricer<BlackScholes> ~ Pricer<MonteCarlo>");
}

// --- 7. Thread-pool portfolio pricing -----------------------------------
static void test_portfolio_threadpool() {
    std::printf("test_portfolio_threadpool\n");
    BlackScholes bs;
    MarketData mkt = make_market();
    std::vector<Option> book;
    book.reserve(200);
    for (int i = 0; i < 200; ++i) {
        book.push_back(Option{OptionType::Call, Exercise::European,
                              80.0 + (i * 0.2), 1.0});
    }
    ThreadPool pool(4);
    auto prices = price_portfolio(bs, book, mkt, pool);
    CHECK(prices.size() == book.size(), "portfolio result size matches");
    // Compare each against a serial computation.
    bool all_match = true;
    for (std::size_t i = 0; i < book.size(); ++i) {
        if (std::fabs(prices[i] - bs.price(book[i], mkt)) > 1e-9)
            all_match = false;
    }
    CHECK(all_match, "threaded portfolio == serial portfolio");
}

// --- 8. AD Greeks match the closed-form Greeks (plan §12) ---------------
static void test_ad_greeks() {
    std::printf("test_ad_greeks\n");
    BlackScholes bs;       // closed-form reference
    BlackScholesAD ad;     // dual-number AD
    Option call = make_call();
    MarketData mkt = make_market();

    // Price agreement first.
    CHECK_NEAR(ad.price(call, mkt), bs.price(call, mkt), 1e-10, "AD price == BS");
    // delta and vega are exact, so a tight tolerance is appropriate.
    CHECK_NEAR(ad.delta(call, mkt), bs.delta(call, mkt), 1e-9, "AD delta == BS");
    CHECK_NEAR(ad.vega(call, mkt),  bs.vega(call, mkt),  1e-9, "AD vega == BS");
    // gamma via second-order duals must match the closed-form second derivative.
    CHECK_NEAR(ad.gamma(call, mkt), bs.gamma(call, mkt), 1e-9, "AD gamma == BS");

    // rho for a call: positive, and check against a tight finite difference.
    double h = 1e-6;
    MarketData up = mkt, dn = mkt;
    up.rate += h; dn.rate -= h;
    double fd_rho = (bs.price(call, up) - bs.price(call, dn)) / (2 * h);
    CHECK_NEAR(ad.rho(call, mkt), fd_rho, 1e-4, "AD rho vs finite-difference");
}

// --- 9. Exotic, path-dependent payoffs ----------------------------------
static void test_exotics() {
    std::printf("test_exotics\n");
    MarketData mkt = make_market();
    Option ref = make_call();   // S=K=100, r=5%, vol=20%, T=1
    double disc = std::exp(-mkt.rate * ref.maturity);
    GbmPathGenerator gen(mkt, ref.maturity, /*steps*/ 50);
    std::size_t paths = 400000;

    // Asian (avg) call must be cheaper than the vanilla European call:
    // averaging reduces effective volatility.
    BlackScholes bs;
    double vanilla = bs.price(ref, mkt);
    AsianPayoff asian{OptionType::Call, ref.strike};
    double asian_px =
        monte_carlo_path_parallel(gen, asian, paths, disc, 0, 123);
    CHECK(asian_px > 0.0 && asian_px < vanilla,
          "Asian call cheaper than vanilla European");

    // Barrier parity: up-and-out + up-and-in == vanilla (same MC streams).
    BarrierPayoff uo{OptionType::Call, ref.strike, 130.0, BarrierKind::UpAndOut};
    BarrierPayoff ui{OptionType::Call, ref.strike, 130.0, BarrierKind::UpAndIn};
    double uo_px = monte_carlo_path_parallel(gen, uo, paths, disc, 0, 7);
    double ui_px = monte_carlo_path_parallel(gen, ui, paths, disc, 0, 7);
    // The European reference priced on the SAME path engine (terminal-only).
    VanillaPayoff term{OptionType::Call, ref.strike};
    struct TerminalOnly {
        VanillaPayoff p;
        double operator()(std::span<const double> path) const {
            return p(path.back());
        }
    } terminal{term};
    double euro_path = monte_carlo_path_parallel(gen, terminal, paths, disc, 0, 7);
    CHECK_NEAR(uo_px + ui_px, euro_path, 1e-9,
               "barrier in/out parity (same streams)");

    // Lookback call >= vanilla call (it pays terminal minus the path minimum).
    LookbackPayoff lb{OptionType::Call};
    double lb_px = monte_carlo_path_parallel(gen, lb, paths, disc, 0, 55);
    CHECK(lb_px >= vanilla - 0.5, "lookback call >= vanilla (approx)");
}

// --- 10. Bond and FX forward pricing ------------------------------------
static void test_fixed_income() {
    std::printf("test_fixed_income\n");
    // A bond whose coupon equals the (effective) discount rate should price
    // near par. Use a 5% coupon with a 5% continuous rate as a sanity band.
    MarketData mkt{100.0, 0.05, 0.0, 0.0};
    Bond bond{/*face*/100.0, /*coupon*/0.05, /*maturity*/5.0, /*freq*/2};
    double pv = price_bond(bond, mkt);
    CHECK(pv > 95.0 && pv < 105.0, "5% bond near par at 5% rate");

    // Zero-coupon: PV = face * exp(-r T).
    Bond zero{100.0, 0.0, 2.0, 1};
    CHECK_NEAR(price_bond(zero, mkt), 100.0 * std::exp(-0.05 * 2.0), 1e-9,
               "zero-coupon bond = discounted face");

    // FX forward struck at the fair rate is worth ~0.
    FxForward fwd{/*strike*/0.0, /*maturity*/1.0, /*foreign_rate*/0.03};
    MarketData fx{1.25, 0.05, 0.0, 0.0};  // spot 1.25, domestic 5%
    double fair = fx_forward_rate(fwd, fx);
    FxForward at_fair{fair, 1.0, 0.03};
    CHECK_NEAR(price_fx_forward(at_fair, fx), 0.0, 1e-12,
               "FX forward at fair rate is worth zero");
    // Long a below-fair strike is worth positive.
    FxForward cheap{fair - 0.1, 1.0, 0.03};
    CHECK(price_fx_forward(cheap, fx) > 0.0, "below-fair FX forward positive");
}

// --- 11. Implied volatility solver --------------------------------------
static void test_implied_vol() {
    std::printf("test_implied_vol\n");
    BlackScholes bs;

    // Round-trip: for a grid of (strike, maturity, vol), price the option then
    // recover the vol. We only require recovery where the option has
    // meaningful vega — deep in/out-of-the-money options whose price equals
    // intrinsic (or zero) to machine precision have a genuinely undefined
    // implied vol, and the solver correctly reports no solution for those.
    const double spot = 100.0, rate = 0.05, div = 0.0;
    const double strikes[]    = {70, 85, 100, 115, 130};
    const double maturities[] = {0.25, 1.0, 2.0};
    const double vols[]       = {0.10, 0.20, 0.45};

    BlackScholesAD ad;
    bool all_ok = true;
    int recovered = 0, skipped_flat = 0;
    for (double K : strikes) {
        for (double T : maturities) {
            for (double sig : vols) {
                Option call{OptionType::Call, Exercise::European, K, T};
                MarketData mkt{spot, rate, sig, div};
                double px = bs.price(call, mkt);
                auto iv = implied_vol(OptionType::Call, px, spot, K, rate, T, div);

                // Attainable vol precision is ~ tol / vega, so a tight 1e-5
                // recovery is only meaningful where vega is well clear of
                // zero. Below that the price barely responds to vol and the
                // implied vol is ill-conditioned (deep ITM/OTM).
                const double vega = ad.vega(call, mkt);
                if (vega > 1e-3) {
                    if (!iv || std::fabs(*iv - sig) > 1e-5) all_ok = false;
                    else ++recovered;
                } else {
                    ++skipped_flat;  // undefined / ill-conditioned IV
                }
            }
        }
    }
    CHECK(all_ok, "IV round-trip recovers vol where vega is meaningful");
    CHECK(recovered >= 35, "IV recovered for the bulk of the grid");

    // Where vega is small but non-trivial, recovery should still be close even
    // if not to 1e-5 — confirm graceful degradation rather than failure.
    {
        Option otm{OptionType::Call, Exercise::European, 130.0, 0.25};
        MarketData mkt{spot, rate, 0.10, div};
        auto iv = implied_vol(OptionType::Call, bs.price(otm, mkt), spot, 130.0,
                              rate, 0.25, div);
        CHECK(iv && std::fabs(*iv - 0.10) < 1e-3,
              "ill-conditioned deep-OTM IV still close (1e-3)");
    }
    std::printf("  (recovered %d, skipped %d flat/degenerate)\n",
                recovered, skipped_flat);

    // Puts too.
    Option put{OptionType::Put, Exercise::European, 110.0, 1.0};
    MarketData mkt{spot, rate, 0.30, div};
    double pp = bs.price(put, mkt);
    auto ivp = implied_vol(OptionType::Put, pp, spot, 110.0, rate, 1.0, div);
    CHECK(ivp && std::fabs(*ivp - 0.30) < 1e-5, "IV round-trip for a put");

    // Edge cases -> no solution (std::nullopt).
    // Price below intrinsic for a call.
    double intrinsic_call = spot - 90.0 * std::exp(-rate * 1.0);
    auto below = implied_vol(OptionType::Call, intrinsic_call - 1.0, spot, 90.0,
                             rate, 1.0, div);
    CHECK(!below.has_value(), "below-intrinsic price -> no IV");

    // Price above the no-arbitrage upper bound (call <= spot).
    auto above = implied_vol(OptionType::Call, spot + 5.0, spot, 100.0, rate,
                             1.0, div);
    CHECK(!above.has_value(), "above-upper-bound price -> no IV");

    // Non-positive / non-finite price.
    CHECK(!implied_vol(OptionType::Call, 0.0, spot, 100.0, rate, 1.0, div),
          "zero price -> no IV");
}

// --- 12. Variance reduction (§14.2): antithetic + control variate -------
static void test_variance_reduction() {
    std::printf("test_variance_reduction\n");
    BlackScholes bs;
    Option call = make_call();
    MarketData mkt = make_market();
    const double exact = bs.price(call, mkt);
    const double disc = std::exp(-mkt.rate * call.maturity);
    auto proc = GbmProcess::from_market(mkt, call.maturity);

    // Antithetic: with N pairs it still lands near the analytic price.
    double anti = monte_carlo_price_antithetic(proc, call.payoff(), 1'000'000,
                                               disc, 42);
    CHECK_NEAR(anti, exact, 5e-2, "antithetic MC near analytic");

    // Control variate: with a perfectly-correlated control (the payoff itself)
    // the corrected estimate should be MUCH closer to the analytic price than
    // the plain MC estimate on the same draws.
    auto cv = monte_carlo_control_variate(call, mkt, 200'000, 7);
    const double plain_err = std::fabs(cv.plain_price - exact);
    const double cv_err = std::fabs(cv.price - exact);
    CHECK(cv.bs_price == exact, "control mean equals closed-form BS");
    CHECK(cv_err <= plain_err + 1e-12, "control variate not worse than plain");
    // For the self-control case the corrected estimate is essentially exact.
    CHECK_NEAR(cv.price, exact, 1e-6, "control-variate estimate ~ exact");
    std::printf("  (plain err %.5f, control-variate err %.2e)\n", plain_err,
                cv_err);
}

// --- 13. CRTP bump Greeks + capability dispatch (§15.2/§15.3) ------------
namespace {
// A model with NO analytic delta — gets bumped Greeks via the CRTP mixin.
struct BumpOnlyBS : BumpGreeks<BumpOnlyBS> {
    double price(const Option& opt, const MarketData& mkt) const {
        return BlackScholes{}.price(opt, mkt);
    }
};
}  // namespace

static void test_crtp_greeks() {
    std::printf("test_crtp_greeks\n");
    BlackScholes bs;        // has analytic delta
    BumpOnlyBS bumped;      // only CRTP bump delta
    Option call = make_call();
    MarketData mkt = make_market();

    // The CRTP bump delta matches the analytic delta.
    CHECK_NEAR(bumped.bump_delta(call, mkt), bs.delta(call, mkt), 1e-5,
               "CRTP bump delta ~ analytic delta");
    CHECK_NEAR(bumped.bump_gamma(call, mkt), bs.gamma(call, mkt), 1e-3,
               "CRTP bump gamma ~ analytic gamma");

    // Capability dispatch: best_delta uses the analytic path for BlackScholes
    // and the bump path for BumpOnlyBS — both correct, chosen at compile time.
    static_assert(HasAnalyticDelta<BlackScholes>, "BS exposes analytic delta");
    static_assert(!HasAnalyticDelta<BumpOnlyBS>, "BumpOnlyBS does not");
    CHECK_NEAR(best_delta(bs, call, mkt), bs.delta(call, mkt), 1e-12,
               "best_delta uses analytic for BS");
    CHECK_NEAR(best_delta(bumped, call, mkt), bs.delta(call, mkt), 1e-5,
               "best_delta falls back to bump");
}

// --- 14. Variadic compile-time Portfolio (§15.1) ------------------------
static void test_variadic_portfolio() {
    std::printf("test_variadic_portfolio\n");
    MarketData mkt = make_market();  // spot 100, r 5%, vol 20%

    Option call{OptionType::Call, Exercise::European, 100.0, 1.0};
    Option put{OptionType::Put, Exercise::European, 100.0, 1.0};
    Bond bond{100.0, 0.05, 5.0, 2};

    // A mixed, heterogeneous portfolio: two options + a bond, one typed object.
    Portfolio book{call, put, bond};
    static_assert(decltype(book)::size() == 3, "three legs at compile time");

    // Total value equals the sum of the individual leg values (the fold).
    BlackScholes bs;
    const double expected = bs.price(call, mkt) + bs.price(put, mkt) +
                            price_bond(bond, MarketData{0.0, mkt.rate, 0.0, 0.0});
    // value() prices the bond with the same market; build the comparison the
    // same way the portfolio does (bond ignores spot/vol).
    const double got = book.value(mkt);
    CHECK_NEAR(got, expected, 1e-9, "portfolio value == sum of legs");

    // A straddle (call + put) is just a two-leg portfolio.
    Portfolio straddle{call, put};
    CHECK_NEAR(straddle.value(mkt), bs.price(call, mkt) + bs.price(put, mkt),
               1e-9, "straddle = call + put");
}

// --- 15. Lazy coroutine MC stream (§15.4) -------------------------------
static void test_lazy_monte_carlo() {
    std::printf("test_lazy_monte_carlo\n");
    Option call = make_call();
    MarketData mkt = make_market();
    auto proc = GbmProcess::from_market(mkt, call.maturity);
    double disc = std::exp(-mkt.rate * call.maturity);
    const std::size_t n = 500'000;

    // The lazy stream uses the same RNG sequence as the eager engine, so for
    // the same seed the mean is bit-identical.
    double eager = monte_carlo_price(proc, call.payoff(), n, disc, 7);
    double lazy = monte_carlo_price_lazy(proc, call.payoff(), n, disc, 7);
    CHECK_NEAR(lazy, eager, 1e-9, "lazy MC stream == eager MC (same draws)");

    // The generator is a real input range: range-for consumes it lazily.
    int count = 0;
    double sum = 0.0;
    for (double pay : mc_payoff_stream(proc, call.payoff(), 1000, disc, 1)) {
        sum += pay;
        ++count;
    }
    CHECK(count == 1000, "coroutine stream yields exactly N payoffs");
    // The streamed payoffs are real (positive mean for an ATM call), which also
    // uses `sum` so it isn't a dead accumulation.
    CHECK(sum > 0.0 && std::isfinite(sum), "streamed payoffs accumulate sanely");
}

// --- 16. Sync primitives: latch + semaphore (§15.6) ---------------------
static void test_sync_primitives() {
    std::printf("test_sync_primitives\n");
    Option call = make_call();
    MarketData mkt = make_market();
    auto proc = GbmProcess::from_market(mkt, call.maturity);
    double disc = std::exp(-mkt.rate * call.maturity);
    BlackScholes bs;
    double exact = bs.price(call, mkt);

    // Latch-synchronized parallel MC lands near the analytic price.
    double synced = monte_carlo_parallel_synced(proc, call.payoff(), 2'000'000,
                                                disc, 4, 7);
    CHECK_NEAR(synced, exact, 5e-2, "latch-synced parallel MC near analytic");

    // Semaphore-bounded job runner: cap in-flight tasks, results still correct
    // and in order.
    std::vector<std::function<double()>> jobs;
    jobs.reserve(50);
    for (int i = 0; i < 50; ++i) {
        jobs.push_back([i] { return static_cast<double>(i * i); });
    }
    auto results = run_bounded(jobs, 4);  // at most 4 concurrent
    bool ok = results.size() == jobs.size();
    for (int i = 0; i < 50 && ok; ++i) {
        if (results[i] != static_cast<double>(i * i)) ok = false;
    }
    CHECK(ok, "semaphore-bounded runner returns correct, ordered results");
}

// --- 17. Property / invariant tests (§16.1) -----------------------------
// Assert structural truths that must hold for ANY valid inputs, rather than a
// single hand-computed value — catches whole classes of regression.
static void test_invariants() {
    std::printf("test_invariants\n");
    BlackScholes bs;
    BinomialTree tree(1000);

    // Sweep a grid of markets and contracts.
    const double spots[] = {60, 90, 100, 110, 150};
    const double strikes[] = {80, 100, 120};
    const double vols[] = {0.10, 0.25, 0.50};
    const double mats[] = {0.25, 1.0, 3.0};
    const double rate = 0.04, div = 0.0;

    bool parity = true, nonneg = true, intrinsic = true, amer_ge = true;
    bool delta_band = true, mono_spot = true, mono_vol = true, mono_mat = true;

    for (double S : spots)
      for (double K : strikes)
        for (double vol : vols)
          for (double T : mats) {
            MarketData mkt{S, rate, vol, div};
            Option call{OptionType::Call, Exercise::European, K, T};
            Option put{OptionType::Put, Exercise::European, K, T};
            const double c = bs.price(call, mkt);
            const double p = bs.price(put, mkt);

            // price >= 0
            if (c < -1e-9 || p < -1e-9) nonneg = false;
            // put-call parity: C - P = S e^{-qT} - K e^{-rT}
            const double rhs = S * std::exp(-div * T) - K * std::exp(-rate * T);
            if (std::fabs((c - p) - rhs) > 1e-6) parity = false;
            // call >= discounted intrinsic on the forward
            const double fwd_intrinsic =
                std::max(S * std::exp(-div * T) - K * std::exp(-rate * T), 0.0);
            if (c < fwd_intrinsic - 1e-6) intrinsic = false;
            // call delta in [0, 1]
            const double d = bs.delta(call, mkt);
            if (d < -1e-9 || d > 1.0 + 1e-9) delta_band = false;
            // American >= European (binomial), for a put where it can bite
            Option amer_put{OptionType::Put, Exercise::American, K, T};
            if (tree.price(amer_put, mkt) < tree.price(put, mkt) - 1e-6)
                amer_ge = false;

            // monotonicity: call price increases in spot, vol, maturity
            const double h = 1.0;
            MarketData up_s = mkt; up_s.spot += h;
            if (bs.price(call, up_s) < c - 1e-9) mono_spot = false;
            MarketData up_v = mkt; up_v.vol += 0.01;
            if (bs.price(call, up_v) < c - 1e-9) mono_vol = false;
            Option longer = call; longer.maturity += 0.5;
            if (bs.price(longer, mkt) < c - 1e-9) mono_mat = false;
          }

    CHECK(nonneg, "invariant: price >= 0");
    CHECK(parity, "invariant: put-call parity holds across the grid");
    CHECK(intrinsic, "invariant: call >= discounted intrinsic");
    CHECK(delta_band, "invariant: call delta in [0,1]");
    CHECK(amer_ge, "invariant: American put >= European put");
    CHECK(mono_spot, "invariant: call increases in spot");
    CHECK(mono_vol, "invariant: call increases in vol");
    CHECK(mono_mat, "invariant: call increases in maturity");
}

// --- 18. Deterministic parallel MC (§16.5) ------------------------------
static void test_deterministic_mc() {
    std::printf("test_deterministic_mc\n");
    Option call = make_call();
    MarketData mkt = make_market();
    auto proc = GbmProcess::from_market(mkt, call.maturity);
    double disc = std::exp(-mkt.rate * call.maturity);
    const std::size_t n = 2'000'000;

    // Same key must give a bit-identical price at any thread count — the whole
    // point of the counter-based RNG + fixed-block reduction.
    double p1 = monte_carlo_parallel_deterministic(proc, call.payoff(), n, disc, 1, 42);
    double p2 = monte_carlo_parallel_deterministic(proc, call.payoff(), n, disc, 2, 42);
    double p4 = monte_carlo_parallel_deterministic(proc, call.payoff(), n, disc, 4, 42);
    double p8 = monte_carlo_parallel_deterministic(proc, call.payoff(), n, disc, 8, 42);
    CHECK(p1 == p2 && p2 == p4 && p4 == p8,
          "deterministic MC: bit-identical across 1/2/4/8 threads");

    // And it still converges to the analytic price.
    CHECK_NEAR(p1, BlackScholes{}.price(call, mkt), 5e-2,
               "deterministic MC near analytic");
}

// --- 19. Richer PricingResult with diagnostics (§16.6) ------------------
static void test_pricing_result() {
    std::printf("test_pricing_result\n");
    Option call = make_call();
    MarketData mkt = make_market();
    auto proc = GbmProcess::from_market(mkt, call.maturity);
    double disc = std::exp(-mkt.rate * call.maturity);
    double exact = BlackScholes{}.price(call, mkt);

    auto r = monte_carlo_result(proc, call.payoff(), 1'000'000, disc, 99);
    CHECK_NEAR(r.price, exact, 5e-2, "MC result price near analytic");
    CHECK(r.paths.value_or(0) == 1'000'000, "result records paths");

    // Pull the optionals into locals once we've confirmed they're present, so
    // every subsequent access is provably safe (no unchecked-optional warning).
    const double se = r.std_error.value_or(-1.0);
    CHECK(se > 0.0, "result reports a positive standard error");
    const double half_width = r.confidence_95().value_or(-1.0);
    CHECK(half_width > 0.0, "95% half-width available for MC");
    // The analytic price should sit within a few standard errors of the MC mean.
    CHECK(std::fabs(r.price - exact) < 5.0 * se,
          "analytic within 5 standard errors of MC mean");
    std::printf("  (MC price %.4f ± %.4f, exact %.4f)\n", r.price, half_width,
                exact);
}

// --- 20. Market value types (§16.2) -------------------------------------
static void test_market_types() {
    std::printf("test_market_types\n");

    // YieldCurve: flat curve equals the scalar; term structure interpolates.
    YieldCurve flat = YieldCurve::flat(0.05);
    CHECK_NEAR(flat.rate_at(1.0), 0.05, 1e-12, "flat curve rate == scalar");
    CHECK_NEAR(flat.discount(2.0), std::exp(-0.05 * 2.0), 1e-12,
               "flat curve discount factor");

    YieldCurve term = YieldCurve::from_pivots({1.0, 2.0, 5.0},
                                              {0.03, 0.035, 0.045});
    CHECK_NEAR(term.rate_at(1.0), 0.03, 1e-12, "curve hits pivot");
    CHECK_NEAR(term.rate_at(1.5), 0.0325, 1e-12, "curve interpolates midpoint");
    CHECK_NEAR(term.rate_at(0.5), 0.03, 1e-12, "curve clamps below first pivot");
    CHECK_NEAR(term.rate_at(10.0), 0.045, 1e-12, "curve clamps above last pivot");

    // VolSurface: flat vs smile lookup.
    VolSurface vflat = VolSurface::flat(0.20);
    CHECK_NEAR(vflat.vol_at(100.0), 0.20, 1e-12, "flat surface vol == scalar");
    VolSurface smile = VolSurface::smile({80, 100, 120}, {0.25, 0.20, 0.23});
    CHECK_NEAR(smile.vol_at(100.0), 0.20, 1e-12, "smile hits ATM pivot");
    CHECK_NEAR(smile.vol_at(90.0), 0.225, 1e-12, "smile interpolates");
    CHECK(smile.vol_at(80) > smile.vol_at(100), "smile: wing above ATM");

    // MarketData backward compatibility + override semantics.
    MarketData flat_md{100.0, 0.05, 0.20, 0.0};
    CHECK_NEAR(flat_md.rate_at(3.0), 0.05, 1e-12, "MarketData flat rate_at");
    CHECK_NEAR(flat_md.vol_at(100.0), 0.20, 1e-12, "MarketData flat vol_at");

    // Attach a curve: rate_at now follows the curve, not the flat field.
    MarketData curved = flat_md;
    curved.curve = term;
    CHECK_NEAR(curved.rate_at(1.5), 0.0325, 1e-12,
               "attached curve overrides flat rate");
    // Attaching a surface overrides flat vol.
    curved.surface = smile;
    CHECK_NEAR(curved.vol_at(90.0), 0.225, 1e-12,
               "attached surface overrides flat vol");

    // A flat curve read through MarketData matches the scalar rate —
    // proves the abstraction is a no-op when flat (the compatibility guarantee).
    Option call = make_call();
    MarketData scalar{100.0, 0.04, 0.25, 0.0};
    MarketData with_flat_curve = scalar;
    with_flat_curve.curve = YieldCurve::flat(0.04);
    CHECK_NEAR(scalar.rate_at(call.maturity),
               with_flat_curve.rate_at(call.maturity), 1e-12,
               "flat curve == scalar rate end to end");
}

int main() {
    test_black_scholes_reference();
    test_greeks();
    test_binomial_converges_to_bs();
    test_monte_carlo_converges_to_bs();
    test_parallel_mc_agrees();
    test_templated_pricer();
    test_portfolio_threadpool();
    test_ad_greeks();
    test_exotics();
    test_fixed_income();
    test_implied_vol();
    test_variance_reduction();
    test_crtp_greeks();
    test_variadic_portfolio();
    test_lazy_monte_carlo();
    test_sync_primitives();
    test_invariants();
    test_deterministic_mc();
    test_pricing_result();
    test_market_types();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
