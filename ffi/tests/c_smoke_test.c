/* c_smoke_test.c — exercises the C ABI from pure C (no C++), which is what
 * the Rust GUI will effectively do. Verifies the boundary links and behaves.
 */
#include <math.h>
#include <stdio.h>

#include "mape_c_api.h"

static int failures = 0;

static void check_near(const char* msg, double got, double want, double tol) {
    double d = fabs(got - want);
    if (d > tol) {
        ++failures;
        printf("  FAIL: %s got %.6f want %.6f (|d|=%.6g > %.6g)\n", msg, got,
               want, d, tol);
    } else {
        printf("  ok:   %s = %.6f\n", msg, got);
    }
}

int main(void) {
    printf("mape C ABI smoke test (v%s)\n", mape_version());

    MapeEngine* e = mape_create();
    if (!e) {
        printf("FAIL: mape_create returned NULL\n");
        return 1;
    }

    /* ATM call: S=K=100, r=5%, vol=20%, T=1, no div -> ~10.4506 (Black-Scholes)
     */
    double bs = mape_price(e, MAPE_MODEL_BLACK_SCHOLES, MAPE_CALL,
                           MAPE_EUROPEAN, 100, 100, 0.05, 0.20, 1.0, 0.0);
    check_near("black-scholes call", bs, 10.4506, 1e-3);

    /* Binomial should converge to the same value. */
    double bin = mape_price(e, MAPE_MODEL_BINOMIAL, MAPE_CALL, MAPE_EUROPEAN,
                            100, 100, 0.05, 0.20, 1.0, 0.0);
    check_near("binomial call", bin, 10.4506, 5e-2);

    /* Monte Carlo within statistical error. */
    double mc = mape_price(e, MAPE_MODEL_MONTE_CARLO, MAPE_CALL, MAPE_EUROPEAN,
                           100, 100, 0.05, 0.20, 1.0, 0.0);
    check_near("monte-carlo call", mc, 10.4506, 1e-1);

    /* Greeks sign sanity. */
    double delta = mape_delta(e, MAPE_CALL, 100, 100, 0.05, 0.20, 1.0, 0.0);
    if (!(delta > 0.0 && delta < 1.0)) {
        ++failures;
        printf("  FAIL: call delta out of (0,1): %.6f\n", delta);
    } else {
        printf("  ok:   delta = %.6f\n", delta);
    }

    /* Bad input -> NaN via plain entry, error code via _ex. */
    double bad =
        mape_price(e, MAPE_MODEL_BLACK_SCHOLES, MAPE_CALL, MAPE_EUROPEAN, 100,
                   100, 0.05, 0.20, /*T*/ -1.0, 0.0);
    if (!isnan(bad)) {
        ++failures;
        printf("  FAIL: bad maturity not NaN\n");
    } else
        printf("  ok:   invalid input returns NaN\n");

    double px = 0;
    MapeStatus st =
        mape_price_ex(NULL, MAPE_MODEL_BLACK_SCHOLES, MAPE_CALL, MAPE_EUROPEAN,
                      100, 100, 0.05, 0.2, 1, 0, &px);
    if (st != MAPE_ERR_NULL_HANDLE) {
        ++failures;
        printf("  FAIL: NULL engine not reported (%d)\n", st);
    } else {
        printf("  ok:   NULL handle reported\n");
    }

    /* Threaded portfolio path. */
    double strikes[5] = {80, 90, 100, 110, 120};
    double maturities[5] = {1, 1, 1, 1, 1};
    double out[5] = {0};
    st = mape_price_portfolio(e, MAPE_MODEL_BLACK_SCHOLES, MAPE_CALL,
                              MAPE_EUROPEAN, 100, 0.05, 0.20, 0.0, strikes,
                              maturities, 5, out);
    if (st != MAPE_OK) {
        ++failures;
        printf("  FAIL: portfolio status %d\n", st);
    }
    /* Lower strike must be worth more for a call. */
    if (!(out[0] > out[4])) {
        ++failures;
        printf("  FAIL: portfolio monotonicity\n");
    } else {
        printf("  ok:   portfolio[80]=%.4f > portfolio[120]=%.4f\n", out[0],
               out[4]);
    }

    /* AD Greeks: delta should match the closed-form delta closely. */
    double ad_delta = mape_ad_greek(e, MAPE_GREEK_DELTA, MAPE_CALL, 100, 100,
                                    0.05, 0.20, 1.0, 0.0);
    check_near("AD delta == closed-form", ad_delta, delta, 1e-9);

    /* Exotic: Asian call must be cheaper than the vanilla European call. */
    double asian =
        mape_price_exotic(e, MAPE_EXOTIC_ASIAN, MAPE_CALL, 100, 100, 0.05, 0.20,
                          1.0, 0.0, 0.0, MAPE_BARRIER_UP_OUT, 50, 200000);
    if (!(asian > 0.0 && asian < bs)) {
        ++failures;
        printf("  FAIL: Asian not in (0, vanilla): %.4f\n", asian);
    } else {
        printf("  ok:   Asian call %.4f < vanilla %.4f\n", asian, bs);
    }

    /* Implied vol round-trip: price at 20% vol, recover ~20%. */
    double iv = mape_implied_vol(e, MAPE_CALL, bs, 100, 100, 0.05, 1.0, 0.0);
    check_near("implied vol round-trip", iv, 0.20, 1e-6);
    /* Below-intrinsic price -> NaN (no solution). */
    double iv_bad =
        mape_implied_vol(e, MAPE_CALL, 0.0001, 100, 70, 0.05, 1.0, 0.0);
    if (!isnan(iv_bad)) {
        ++failures;
        printf("  FAIL: bad IV not NaN\n");
    } else
        printf("  ok:   below-intrinsic IV returns NaN\n");

    /* Bond: zero-coupon = discounted face. */
    double zc = mape_price_bond(e, 100.0, 0.0, 2.0, 1, 0.05);
    check_near("zero-coupon bond", zc, 100.0 * exp(-0.05 * 2.0), 1e-9);

    /* FX forward struck at the fair rate is worth ~0. */
    double fair = mape_fx_forward_rate(e, 1.25, 1.0, 0.05, 0.03);
    double fxpv = mape_price_fx_forward(e, 1.25, fair, 1.0, 0.05, 0.03);
    check_near("FX forward at fair rate", fxpv, 0.0, 1e-12);

    /* Convergence: binomial price at high step count approaches BS. */
    double sizes[3] = {16, 128, 1024};
    double conv[3] = {0};
    st = mape_convergence(e, MAPE_MODEL_BINOMIAL, MAPE_CALL, 100, 100, 0.05,
                          0.20, 1.0, 0.0, sizes, 3, conv);
    if (st != MAPE_OK) {
        ++failures;
        printf("  FAIL: convergence status %d\n", st);
    }
    check_near("binomial converges to BS", conv[2], bs, 1e-2);

    mape_destroy(e);

    printf("\n%s (failures: %d)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
