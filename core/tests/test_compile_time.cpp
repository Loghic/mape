// Compile-time pricing tests (plan §5.4).
//
// Almost everything here is a static_assert: it is checked by the compiler, so
// if this translation unit compiles, the compile-time pricing path is correct.
// The few runtime checks at the end confirm the compile-time results agree with
// the runtime model. This file is also a regression guard — change the math and
// these static_asserts either still hold or the build fails.

#include <cmath>
#include <cstdio>

#include "mape/compile_time.hpp"
#include "mape/ct_math.hpp"
#include "mape/models/black_scholes.hpp"

using namespace mape;

// --- 1. constexpr primitives match std:: closely (compile-time) ---------
namespace {
constexpr bool approx(double a, double b, double tol) {
    const double d = a - b;
    return (d < 0 ? -d : d) <= tol;
}
}  // namespace

static_assert(approx(ct::sqrt_ct(2.0), 1.4142135623730951, 1e-12), "sqrt_ct");
static_assert(approx(ct::exp_ct(1.0), 2.718281828459045, 1e-12), "exp_ct");
static_assert(approx(ct::exp_ct(-0.1), 0.9048374180359595, 1e-12),
              "exp_ct neg");
static_assert(approx(ct::log_ct(2.0), 0.6931471805599453, 1e-12), "log_ct");
// erf via Abramowitz-Stegun: accurate to ~1.5e-7.
static_assert(approx(ct::erf_ct(1.0), 0.8427007929497149, 2e-7), "erf_ct");

// --- 2. constexpr Black-Scholes folds at compile time -------------------
// ATM call S=K=100, r=5%, vol=20%, T=1 -> ~10.4506.
constexpr double kCtCall = ct::bs_call(100, 100, 0.05, 0.20, 1.0);
static_assert(approx(kCtCall, 10.4506, 1e-3),
              "compile-time BS call matches the analytic value");

// Put-call parity holds at compile time: C - P == S - K e^{-rT}.
constexpr double kCtPut = ct::bs_put(100, 100, 0.05, 0.20, 1.0);
static_assert(approx(kCtCall - kCtPut,
                     100.0 - 100.0 * ct::discount_factor(0.05, 1.0), 1e-4),
              "compile-time put-call parity");

// --- 3. constexpr day-count / discounting -------------------------------
static_assert(ct::year_fraction(0, 365) == 1.0, "ACT/365 full year");
static_assert(ct::year_fraction(0, 182) > 0.49 &&
                  ct::year_fraction(0, 182) < 0.50,
              "ACT/365 half year");
static_assert(approx(ct::discount_factor(0.05, 2.0), 0.9048374180359595, 1e-10),
              "discount factor exp(-rT)");

// --- 4. consteval contract validation -----------------------------------
// A valid spec is constructed at compile time and priced at compile time.
constexpr ct::OptionSpec kSpec =
    ct::make_option(OptionType::Call, 100.0, 0.20, 1.0);
constexpr double kSpecPrice = ct::price_spec(kSpec, 100.0, 0.05);
static_assert(approx(kSpecPrice, 10.4506, 1e-3),
              "priced from a validated spec");
// (A negative strike / vol / maturity would make make_option ill-formed — that
// is verified by the build, see core/tests/README or the static-analysis docs.)

int main() {
    // Runtime cross-check: the compile-time price agrees with the runtime model
    // within the Abramowitz-Stegun CDF tolerance (~1e-5 here).
    BlackScholes bs;
    Option call{OptionType::Call, Exercise::European, 100, 1.0};
    MarketData mkt{100, 0.05, 0.20, 0.0};
    const double runtime = bs.price(call, mkt);

    int failures = 0;
    if (std::fabs(kCtCall - runtime) > 1e-4) {
        ++failures;
        std::printf("  FAIL: compile-time %.6f vs runtime %.6f\n", kCtCall,
                    runtime);
    }
    std::printf("compile-time BS call = %.6f (runtime %.6f)\n", kCtCall,
                runtime);
    std::printf("all compile-time assertions passed at build time\n");
    std::printf("\n%s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
