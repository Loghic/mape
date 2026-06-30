/* mape_c_api.h — stable, flat C ABI over the C++ pricing core.
 *
 * This is the ONLY surface the Rust GUI (or any C consumer) sees. No C++ types
 * cross this boundary: the engine is an opaque handle, and everything else is
 * plain C scalars and enums. See plan.md §6.
 *
 * Ownership rule: whoever calls mape_create() must call mape_destroy(). The
 * Rust wrapper enforces this via Drop.
 */
#ifndef MAPE_C_API_H
#define MAPE_C_API_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Export macro -------------------------------------------------------
 * Keeps symbol visibility correct when built as a shared library on Windows.
 */
#if defined(_WIN32) && defined(MAPE_BUILD_SHARED)
#  define MAPE_API __declspec(dllexport)
#elif defined(_WIN32) && defined(MAPE_USE_SHARED)
#  define MAPE_API __declspec(dllimport)
#else
#  define MAPE_API
#endif

/* --- Opaque handle ------------------------------------------------------
 * The caller never sees the layout; it only holds the pointer.
 */
typedef struct MapeEngine MapeEngine;

/* --- Enums (kept as ints across the ABI) -------------------------------- */
typedef enum {
    MAPE_CALL = 0,
    MAPE_PUT  = 1
} MapeOptionType;

typedef enum {
    MAPE_EUROPEAN = 0,
    MAPE_AMERICAN = 1
} MapeExercise;

typedef enum {
    MAPE_MODEL_BLACK_SCHOLES = 0,
    MAPE_MODEL_BINOMIAL      = 1,
    MAPE_MODEL_MONTE_CARLO   = 2
} MapeModel;

/* Status codes returned by the *_ex variants. */
typedef enum {
    MAPE_OK              = 0,
    MAPE_ERR_NULL_HANDLE = 1,
    MAPE_ERR_BAD_INPUT   = 2,  /* e.g. negative vol, non-positive maturity */
    MAPE_ERR_UNKNOWN     = 99
} MapeStatus;

/* --- Lifecycle ---------------------------------------------------------- */

/* Create an engine. Returns NULL on allocation failure. */
MAPE_API MapeEngine* mape_create(void);

/* Destroy an engine created by mape_create(). Safe to pass NULL. */
MAPE_API void mape_destroy(MapeEngine* engine);

/* --- Pricing -----------------------------------------------------------
 * One flat entry point covering the vanilla-option matrix. The chosen model
 * decides how the option is valued; American exercise is honoured by the
 * binomial model (Black-Scholes/Monte Carlo treat it as European).
 *
 * On error these return NaN; use mape_price_ex() to get a status code.
 */
MAPE_API double mape_price(MapeEngine* engine,
                           MapeModel       model,
                           MapeOptionType  type,
                           MapeExercise    exercise,
                           double spot, double strike, double rate,
                           double vol, double maturity, double dividend);

MAPE_API MapeStatus mape_price_ex(MapeEngine* engine,
                                  MapeModel model, MapeOptionType type,
                                  MapeExercise exercise,
                                  double spot, double strike, double rate,
                                  double vol, double maturity, double dividend,
                                  double* out_price);

/* --- Greeks (closed-form, Black-Scholes) -------------------------------- */
MAPE_API double mape_delta(MapeEngine* engine, MapeOptionType type,
                           double spot, double strike, double rate,
                           double vol, double maturity, double dividend);

MAPE_API double mape_gamma(MapeEngine* engine, MapeOptionType type,
                           double spot, double strike, double rate,
                           double vol, double maturity, double dividend);

MAPE_API double mape_vega(MapeEngine* engine, MapeOptionType type,
                          double spot, double strike, double rate,
                          double vol, double maturity, double dividend);

/* --- Portfolio (exercises the threaded path) ----------------------------
 * Prices `count` options against one market snapshot using the thread pool,
 * writing results into out_prices[0..count). Strikes/maturities are arrays of
 * length `count`; all share the same model, type, exercise and market data.
 * Returns MAPE_OK on success. out_prices must have room for `count` doubles.
 */
MAPE_API MapeStatus mape_price_portfolio(MapeEngine* engine,
                                         MapeModel model, MapeOptionType type,
                                         MapeExercise exercise,
                                         double spot, double rate, double vol,
                                         double dividend,
                                         const double* strikes,
                                         const double* maturities,
                                         size_t count,
                                         double* out_prices);

/* --- AD Greeks (exact, via dual numbers) --------------------------------
 * Forward-mode automatic-differentiation Greeks for a European option. These
 * are exact (no bumping). delta/vega/rho use first-order duals; gamma uses a
 * second-order dual. Returns NaN on invalid input.
 */
typedef enum {
    MAPE_GREEK_DELTA = 0,
    MAPE_GREEK_VEGA  = 1,
    MAPE_GREEK_RHO   = 2,
    MAPE_GREEK_GAMMA = 3
} MapeGreek;

MAPE_API double mape_ad_greek(MapeEngine* engine, MapeGreek greek,
                              MapeOptionType type,
                              double spot, double strike, double rate,
                              double vol, double maturity, double dividend);

/* --- Exotic, path-dependent options (Monte Carlo) ----------------------- */
typedef enum {
    MAPE_EXOTIC_ASIAN    = 0,  /* arithmetic-average-price option */
    MAPE_EXOTIC_BARRIER  = 1,  /* single-barrier knock in/out     */
    MAPE_EXOTIC_LOOKBACK = 2   /* floating-strike lookback        */
} MapeExotic;

typedef enum {
    MAPE_BARRIER_UP_OUT   = 0,
    MAPE_BARRIER_DOWN_OUT = 1,
    MAPE_BARRIER_UP_IN    = 2,
    MAPE_BARRIER_DOWN_IN  = 3
} MapeBarrierKind;

/* Price an exotic via parallel path Monte Carlo. `barrier` and `barrier_kind`
 * are ignored unless exotic == MAPE_EXOTIC_BARRIER; `strike` is ignored for
 * lookback. `steps` is the number of monitoring points per path, `paths` the
 * number of simulations. Returns NaN on invalid input. */
MAPE_API double mape_price_exotic(MapeEngine* engine,
                                  MapeExotic exotic, MapeOptionType type,
                                  double spot, double strike, double rate,
                                  double vol, double maturity, double dividend,
                                  double barrier, MapeBarrierKind barrier_kind,
                                  size_t steps, size_t paths);

/* --- Implied volatility -------------------------------------------------
 * Invert Black-Scholes for the vol that reproduces a market option price
 * (the engine used backwards). Returns NaN when no valid implied vol exists:
 * a price below intrinsic, above the no-arbitrage bound, or numerically
 * undefined (deep in/out-of-the-money, where price is flat in vol). Callers
 * should skip NaN results rather than treat them as a vol of 0.
 */
MAPE_API double mape_implied_vol(MapeEngine* engine, MapeOptionType type,
                                 double market_price, double spot,
                                 double strike, double rate, double maturity,
                                 double dividend);

/* --- Fixed income ------------------------------------------------------- */

/* Present value of a fixed-coupon bond. `coupon` is the annual rate (0.05 =
 * 5%), paid `frequency` times/year; discounted continuously at `rate`. */
MAPE_API double mape_price_bond(MapeEngine* engine,
                                double face, double coupon, double maturity,
                                int frequency, double rate);

/* Present value of an FX forward (long, per unit foreign notional):
 * spot * exp(-foreign_rate*T) - strike * exp(-domestic_rate*T). */
MAPE_API double mape_price_fx_forward(MapeEngine* engine,
                                      double spot, double strike,
                                      double maturity,
                                      double domestic_rate,
                                      double foreign_rate);

/* Fair forward FX rate by covered interest parity. */
MAPE_API double mape_fx_forward_rate(MapeEngine* engine,
                                     double spot, double maturity,
                                     double domestic_rate,
                                     double foreign_rate);

/* --- Convergence data (for the GUI chart) -------------------------------
 * Fill out_x[i]/out_y[i] with (sample_size, price) pairs showing the chosen
 * model converging toward Black-Scholes. For the binomial model sample_size
 * is the step count; for Monte Carlo it is the path count. `n` is the number
 * of points; both arrays must hold `n` doubles. Returns MAPE_OK on success. */
MAPE_API MapeStatus mape_convergence(MapeEngine* engine,
                                     MapeModel model, MapeOptionType type,
                                     double spot, double strike, double rate,
                                     double vol, double maturity,
                                     double dividend,
                                     const double* sample_sizes, size_t n,
                                     double* out_prices);

/* Library version string, e.g. "0.1.0". */
MAPE_API const char* mape_version(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* MAPE_C_API_H */
