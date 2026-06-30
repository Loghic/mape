// libFuzzer harness for the flat C ABI (plan §16.7).
//
// Interprets the fuzzer's random bytes as option/market parameters and feeds
// them through every C entry point. The contract being fuzzed is the project's
// boundary rule: NO input — however malformed (NaNs, infinities, negatives,
// absurd path counts) — may crash the process or let an exception escape
// `extern "C"`. Invalid inputs must come back as NaN / a MapeStatus, never UB.
//
// Build (Clang):
//   clang++ -std=c++20 -g -fsanitize=fuzzer,address,undefined \
//     -Icore/include -Iffi/include \
//     ffi/tests/fuzz_c_api.cpp ffi/src/mape_c_api.cpp -o fuzz_c_api
//   ./fuzz_c_api -runs=100000

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "mape_c_api.h"

namespace {

// Pull a double from the fuzz buffer (or 0 if exhausted).
double take_double(const uint8_t*& p, const uint8_t* end) {
    double v = 0.0;
    if (p + sizeof(double) <= end) {
        std::memcpy(&v, p, sizeof(double));
        p += sizeof(double);
    }
    return v;
}

int take_int(const uint8_t*& p, const uint8_t* end) {
    int v = 0;
    if (p + sizeof(int) <= end) {
        std::memcpy(&v, p, sizeof(int));
        p += sizeof(int);
    }
    return v;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const uint8_t* p = data;
    const uint8_t* end = data + size;

    MapeEngine* e = mape_create();
    if (!e) return 0;

    // Derive parameters from the fuzz bytes — deliberately unvalidated, so the
    // harness exercises the engine's own input checking and exception guards.
    const double spot = take_double(p, end);
    const double strike = take_double(p, end);
    const double rate = take_double(p, end);
    const double vol = take_double(p, end);
    const double maturity = take_double(p, end);
    const double dividend = take_double(p, end);
    const double market_price = take_double(p, end);
    const double barrier = take_double(p, end);
    const int sel = take_int(p, end);

    // Map the selector onto enum ranges (modulo keeps them in-range; the values
    // themselves are still arbitrary combinations).
    const MapeModel model = static_cast<MapeModel>((sel & 3) % 3);
    const MapeOptionType type = static_cast<MapeOptionType>(sel & 1);
    const MapeExercise ex = static_cast<MapeExercise>((sel >> 1) & 1);

    // Exercise every entry point. We don't check the *values* — only that none
    // of these crash or throw across the boundary on arbitrary input.
    volatile double sink = 0.0;
    sink += mape_price(e, model, type, ex, spot, strike, rate, vol, maturity,
                       dividend);
    sink += mape_delta(e, type, spot, strike, rate, vol, maturity, dividend);
    sink += mape_gamma(e, type, spot, strike, rate, vol, maturity, dividend);
    sink += mape_vega(e, type, spot, strike, rate, vol, maturity, dividend);
    sink += mape_implied_vol(e, type, market_price, spot, strike, rate,
                             maturity, dividend);
    sink += mape_price_bond(e, spot, rate, maturity, (sel & 7) + 1, vol);
    sink += mape_price_fx_forward(e, spot, strike, maturity, rate, dividend);
    sink += mape_fx_forward_rate(e, spot, maturity, rate, dividend);

    // Exotic with a bounded step/path count (keep the fuzzer fast).
    const std::size_t steps = static_cast<std::size_t>(sel & 31) + 1;
    const std::size_t paths = static_cast<std::size_t>((sel >> 5) & 1023) + 1;
    sink += mape_price_exotic(e, MAPE_EXOTIC_BARRIER, type, spot, strike, rate,
                              vol, maturity, dividend, barrier,
                              MAPE_BARRIER_UP_OUT, steps, paths);

    // FD-PDE model and deterministic MC: same boundary contract on garbage.
    sink += mape_price(e, MAPE_MODEL_FINITE_DIFF, type, ex, spot, strike, rate,
                       vol, maturity, dividend);
    sink += mape_price_mc_deterministic(
        e, type, spot, strike, rate, vol, maturity, dividend, paths,
        static_cast<std::size_t>(sel & 7), static_cast<std::size_t>(sel));

    // Calibration over a few quotes drawn from the fuzz scalars.
    const double strikes[3] = {strike, strike + 5.0, strike - 5.0};
    const double maturities[3] = {maturity, maturity, maturity};
    const double ivs[3] = {vol, market_price, barrier};
    double params[5] = {0, 0, 0, 0, 0};
    double rmse = 0.0;
    int iters = 0;
    mape_calibrate_svi(e, strikes, maturities, ivs, 3, spot, params, &rmse,
                       &iters);
    sink += mape_svi_vol(params, strike, spot, maturity);

    // Stress run under an arbitrary model + vol shock.
    double sp[MAPE_STRESS_COUNT];
    double spnl[MAPE_STRESS_COUNT];
    mape_run_stress(e, model, type, ex, spot, strike, rate, vol, maturity,
                    dividend, barrier, sp, spnl);
    for (int i = 0; i < MAPE_STRESS_COUNT; ++i) sink += sp[i] + spnl[i];

    (void)sink;
    mape_destroy(e);
    return 0;
}
