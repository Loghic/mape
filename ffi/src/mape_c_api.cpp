// mape_c_api.cpp — implementation of the flat C ABI over the C++ core.
//
// All exceptions are caught at the boundary (an exception must never unwind
// across `extern "C"` into C/Rust — that's undefined behaviour). The opaque
// MapeEngine owns the thread pool used for portfolio pricing.

#include "mape_c_api.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <new>
#include <vector>

#include "mape/mape.hpp"

namespace {

// Translate C enums into core inputs.
mape::OptionType to_type(MapeOptionType t) {
    return t == MAPE_PUT ? mape::OptionType::Put : mape::OptionType::Call;
}
mape::Exercise to_exercise(MapeExercise e) {
    return e == MAPE_AMERICAN ? mape::Exercise::American
                              : mape::Exercise::European;
}

bool valid_market(double spot, double vol, double maturity) {
    return spot > 0.0 && vol >= 0.0 && maturity > 0.0 && std::isfinite(spot) &&
           std::isfinite(vol) && std::isfinite(maturity);
}

double price_with_model(MapeModel model, const mape::Option& opt,
                        const mape::MarketData& mkt) {
    switch (model) {
        case MAPE_MODEL_BINOMIAL:
            return mape::BinomialTree{512}.price(opt, mkt);
        case MAPE_MODEL_MONTE_CARLO:
            return mape::MonteCarlo{200000}.price(opt, mkt);
        case MAPE_MODEL_FINITE_DIFF:
            return mape::FdPde{}.price(opt, mkt);
        case MAPE_MODEL_BLACK_SCHOLES:
        default:
            return mape::BlackScholes{}.price(opt, mkt);
    }
}

}  // namespace

// The opaque engine. Holds a thread pool so portfolio calls reuse workers.
// `pool{}` gives the member an explicit default initializer so that
// `new MapeEngine{}` value-initializes it via ThreadPool's explicit
// constructor (AppleClang rejects the omitted-initializer form otherwise).
struct MapeEngine {
    mape::ThreadPool pool{};  // default-sized to hardware concurrency
};

extern "C" {

MapeEngine* mape_create(void) {
    try {
        return new MapeEngine{};
    } catch (...) {
        return nullptr;
    }
}

void mape_destroy(MapeEngine* engine) {
    delete engine;  // safe on nullptr
}

MapeStatus mape_price_ex(MapeEngine* engine, MapeModel model,
                         MapeOptionType type, MapeExercise exercise,
                         double spot, double strike, double rate, double vol,
                         double maturity, double dividend, double* out_price) {
    if (!engine || !out_price) return MAPE_ERR_NULL_HANDLE;
    if (!valid_market(spot, vol, maturity) || strike <= 0.0)
        return MAPE_ERR_BAD_INPUT;
    try {
        mape::Option opt{to_type(type), to_exercise(exercise), strike,
                         maturity};
        mape::MarketData mkt{spot, rate, vol, dividend};
        *out_price = price_with_model(model, opt, mkt);
        return MAPE_OK;
    } catch (...) {
        return MAPE_ERR_UNKNOWN;
    }
}

double mape_price(MapeEngine* engine, MapeModel model, MapeOptionType type,
                  MapeExercise exercise, double spot, double strike,
                  double rate, double vol, double maturity, double dividend) {
    double out = std::nan("");
    mape_price_ex(engine, model, type, exercise, spot, strike, rate, vol,
                  maturity, dividend, &out);
    return out;
}

static double greek(MapeEngine* engine, MapeOptionType type, double spot,
                    double strike, double rate, double vol, double maturity,
                    double dividend, int which) {
    if (!engine || !valid_market(spot, vol, maturity) || strike <= 0.0)
        return std::nan("");
    try {
        mape::BlackScholes bs;
        mape::Option opt{to_type(type), mape::Exercise::European, strike,
                         maturity};
        mape::MarketData mkt{spot, rate, vol, dividend};
        switch (which) {
            case 0:
                return bs.delta(opt, mkt);
            case 1:
                return bs.gamma(opt, mkt);
            case 2:
                return bs.vega(opt, mkt);
            default:
                return std::nan("");
        }
    } catch (...) {
        return std::nan("");
    }
}

double mape_delta(MapeEngine* e, MapeOptionType t, double s, double k, double r,
                  double v, double tm, double q) {
    return greek(e, t, s, k, r, v, tm, q, 0);
}
double mape_gamma(MapeEngine* e, MapeOptionType t, double s, double k, double r,
                  double v, double tm, double q) {
    return greek(e, t, s, k, r, v, tm, q, 1);
}
double mape_vega(MapeEngine* e, MapeOptionType t, double s, double k, double r,
                 double v, double tm, double q) {
    return greek(e, t, s, k, r, v, tm, q, 2);
}

MapeStatus mape_price_portfolio(MapeEngine* engine, MapeModel model,
                                MapeOptionType type, MapeExercise exercise,
                                double spot, double rate, double vol,
                                double dividend, const double* strikes,
                                const double* maturities, size_t count,
                                double* out_prices) {
    if (!engine || !strikes || !maturities || !out_prices)
        return MAPE_ERR_NULL_HANDLE;
    if (count == 0) return MAPE_OK;
    try {
        mape::MarketData mkt{spot, rate, vol, dividend};
        std::vector<mape::Option> book;
        book.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            book.push_back(mape::Option{to_type(type), to_exercise(exercise),
                                        strikes[i], maturities[i]});
        }

        // Dispatch through the engine's pool. We branch on model so each
        // concrete model object is captured by the portfolio helper.
        std::vector<double> prices;
        switch (model) {
            case MAPE_MODEL_BINOMIAL:
                prices = mape::price_portfolio(mape::BinomialTree{512}, book,
                                               mkt, engine->pool);
                break;
            case MAPE_MODEL_MONTE_CARLO:
                prices = mape::price_portfolio(mape::MonteCarlo{200000}, book,
                                               mkt, engine->pool);
                break;
            case MAPE_MODEL_FINITE_DIFF:
                prices = mape::price_portfolio(mape::FdPde{}, book, mkt,
                                               engine->pool);
                break;
            case MAPE_MODEL_BLACK_SCHOLES:
            default:
                prices = mape::price_portfolio(mape::BlackScholes{}, book, mkt,
                                               engine->pool);
                break;
        }
        for (size_t i = 0; i < count; ++i) out_prices[i] = prices[i];
        return MAPE_OK;
    } catch (...) {
        return MAPE_ERR_UNKNOWN;
    }
}

double mape_ad_greek(MapeEngine* engine, MapeGreek greek, MapeOptionType type,
                     double spot, double strike, double rate, double vol,
                     double maturity, double dividend) {
    if (!engine || !valid_market(spot, vol, maturity) || strike <= 0.0)
        return std::nan("");
    try {
        mape::BlackScholesAD ad;
        mape::Option opt{to_type(type), mape::Exercise::European, strike,
                         maturity};
        mape::MarketData mkt{spot, rate, vol, dividend};
        switch (greek) {
            case MAPE_GREEK_DELTA:
                return ad.delta(opt, mkt);
            case MAPE_GREEK_VEGA:
                return ad.vega(opt, mkt);
            case MAPE_GREEK_RHO:
                return ad.rho(opt, mkt);
            case MAPE_GREEK_GAMMA:
                return ad.gamma(opt, mkt);
        }
        return std::nan("");
    } catch (...) {
        return std::nan("");
    }
}

double mape_price_exotic(MapeEngine* engine, MapeExotic exotic,
                         MapeOptionType type, double spot, double strike,
                         double rate, double vol, double maturity,
                         double dividend, double barrier,
                         MapeBarrierKind barrier_kind, size_t steps,
                         size_t paths) {
    if (!engine || !valid_market(spot, vol, maturity) || steps == 0 ||
        paths == 0)
        return std::nan("");
    try {
        mape::MarketData mkt{spot, rate, vol, dividend};
        mape::GbmPathGenerator gen(mkt, maturity, steps);
        const double discount = std::exp(-rate * maturity);
        const mape::OptionType ot = to_type(type);

        switch (exotic) {
            case MAPE_EXOTIC_ASIAN: {
                mape::AsianPayoff p{ot, strike};
                return mape::monte_carlo_path_parallel(gen, p, paths, discount);
            }
            case MAPE_EXOTIC_LOOKBACK: {
                mape::LookbackPayoff p{ot};
                return mape::monte_carlo_path_parallel(gen, p, paths, discount);
            }
            case MAPE_EXOTIC_BARRIER: {
                mape::BarrierKind bk;
                switch (barrier_kind) {
                    case MAPE_BARRIER_DOWN_OUT:
                        bk = mape::BarrierKind::DownAndOut;
                        break;
                    case MAPE_BARRIER_UP_IN:
                        bk = mape::BarrierKind::UpAndIn;
                        break;
                    case MAPE_BARRIER_DOWN_IN:
                        bk = mape::BarrierKind::DownAndIn;
                        break;
                    case MAPE_BARRIER_UP_OUT:
                    default:
                        bk = mape::BarrierKind::UpAndOut;
                        break;
                }
                mape::BarrierPayoff p{ot, strike, barrier, bk};
                return mape::monte_carlo_path_parallel(gen, p, paths, discount);
            }
        }
        return std::nan("");
    } catch (...) {
        return std::nan("");
    }
}

double mape_implied_vol(MapeEngine* engine, MapeOptionType type,
                        double market_price, double spot, double strike,
                        double rate, double maturity, double dividend) {
    if (!engine) return std::nan("");
    try {
        auto iv = mape::implied_vol(to_type(type), market_price, spot, strike,
                                    rate, maturity, dividend);
        return iv ? *iv : std::nan("");
    } catch (...) {
        return std::nan("");
    }
}

double mape_price_bond(MapeEngine* engine, double face, double coupon,
                       double maturity, int frequency, double rate) {
    if (!engine || maturity <= 0.0 || frequency <= 0) return std::nan("");
    try {
        mape::Bond bond{face, coupon, maturity, frequency};
        mape::MarketData mkt{0.0, rate, 0.0, 0.0};
        return mape::price_bond(bond, mkt);
    } catch (...) {
        return std::nan("");
    }
}

double mape_price_fx_forward(MapeEngine* engine, double spot, double strike,
                             double maturity, double domestic_rate,
                             double foreign_rate) {
    if (!engine || maturity <= 0.0 || spot <= 0.0) return std::nan("");
    try {
        mape::FxForward fwd{strike, maturity, foreign_rate};
        mape::MarketData mkt{spot, domestic_rate, 0.0, 0.0};
        return mape::price_fx_forward(fwd, mkt);
    } catch (...) {
        return std::nan("");
    }
}

double mape_fx_forward_rate(MapeEngine* engine, double spot, double maturity,
                            double domestic_rate, double foreign_rate) {
    if (!engine || maturity <= 0.0 || spot <= 0.0) return std::nan("");
    try {
        mape::FxForward fwd{0.0, maturity, foreign_rate};
        mape::MarketData mkt{spot, domestic_rate, 0.0, 0.0};
        return mape::fx_forward_rate(fwd, mkt);
    } catch (...) {
        return std::nan("");
    }
}

MapeStatus mape_convergence(MapeEngine* engine, MapeModel model,
                            MapeOptionType type, double spot, double strike,
                            double rate, double vol, double maturity,
                            double dividend, const double* sample_sizes,
                            size_t n, double* out_prices) {
    if (!engine || !sample_sizes || !out_prices) return MAPE_ERR_NULL_HANDLE;
    if (!valid_market(spot, vol, maturity) || strike <= 0.0)
        return MAPE_ERR_BAD_INPUT;
    try {
        mape::Option opt{to_type(type), mape::Exercise::European, strike,
                         maturity};
        mape::MarketData mkt{spot, rate, vol, dividend};
        for (size_t i = 0; i < n; ++i) {
            const auto sz = static_cast<std::size_t>(sample_sizes[i]);
            switch (model) {
                case MAPE_MODEL_BINOMIAL:
                    out_prices[i] =
                        mape::BinomialTree{static_cast<int>(sz)}.price(opt,
                                                                       mkt);
                    break;
                case MAPE_MODEL_MONTE_CARLO:
                    out_prices[i] =
                        mape::MonteCarlo{sz, 12345ULL}.price(opt, mkt);
                    break;
                case MAPE_MODEL_FINITE_DIFF: {
                    // For the PDE, the "sample size" is the grid resolution
                    // (same number of spot and time steps).
                    const int g = static_cast<int>(sz);
                    out_prices[i] = mape::FdPde{g, g}.price(opt, mkt);
                    break;
                }
                case MAPE_MODEL_BLACK_SCHOLES:
                default:
                    out_prices[i] = mape::BlackScholes{}.price(opt, mkt);
                    break;
            }
        }
        return MAPE_OK;
    } catch (...) {
        return MAPE_ERR_UNKNOWN;
    }
}

double mape_price_mc_deterministic(MapeEngine* engine, MapeOptionType type,
                                   double spot, double strike, double rate,
                                   double vol, double maturity, double dividend,
                                   size_t paths, size_t threads, size_t key) {
    if (!engine) return std::nan("");
    if (!valid_market(spot, vol, maturity) || strike <= 0.0)
        return std::nan("");
    try {
        const mape::Option opt{to_type(type), mape::Exercise::European, strike,
                               maturity};
        const mape::MarketData mkt{spot, rate, vol, dividend};
        const auto process = mape::GbmProcess::from_market(mkt, maturity);
        const double discount = std::exp(-rate * maturity);
        return mape::monte_carlo_parallel_deterministic(
            process, opt.payoff(), paths, discount,
            static_cast<unsigned>(threads), static_cast<std::uint64_t>(key));
    } catch (...) {
        return std::nan("");
    }
}

MapeStatus mape_calibrate_svi(MapeEngine* engine, const double* strikes,
                              const double* maturities,
                              const double* implied_vols, size_t count,
                              double forward, double* out_params,
                              double* out_rmse, int* out_iterations) {
    if (!engine || !strikes || !maturities || !implied_vols || !out_params)
        return MAPE_ERR_NULL_HANDLE;
    if (count == 0 || !(forward > 0.0)) return MAPE_ERR_BAD_INPUT;
    try {
        std::vector<mape::MarketQuote> quotes;
        quotes.reserve(count);
        for (size_t i = 0; i < count; ++i)
            quotes.push_back({strikes[i], maturities[i], implied_vols[i]});

        const mape::CalibrationResult res =
            mape::calibrate_svi(quotes, forward);
        out_params[0] = res.params.a;
        out_params[1] = res.params.b;
        out_params[2] = res.params.rho;
        out_params[3] = res.params.m;
        out_params[4] = res.params.sigma;
        if (out_rmse) *out_rmse = res.rmse;
        if (out_iterations) *out_iterations = res.iterations;
        return MAPE_OK;
    } catch (...) {
        return MAPE_ERR_UNKNOWN;
    }
}

double mape_svi_vol(const double* params, double strike, double forward,
                    double maturity) {
    if (!params || !(strike > 0.0) || !(forward > 0.0) || !(maturity > 0.0))
        return std::nan("");
    const mape::SviParams p{params[0], params[1], params[2], params[3],
                            params[4]};
    const double v = p.vol(strike, forward, maturity);
    return std::isfinite(v) ? v : std::nan("");
}

namespace {

// Fixed stress-scenario ordering, shared by mape_run_stress and
// mape_stress_scenario_name so the GUI's labels always match the rows. Mirrors
// mape::stress_scenarios() but with stable, vol-shock-independent names.
const char* const kStressNames[MAPE_STRESS_COUNT] = {
    "spot -20%", "spot -10%",  "spot +10%",
    "spot +20%", "vol +shock", "crash: spot -20%, vol +shock"};

}  // namespace

MapeStatus mape_run_stress(MapeEngine* engine, MapeModel model,
                           MapeOptionType type, MapeExercise exercise,
                           double spot, double strike, double rate, double vol,
                           double maturity, double dividend, double vol_shock,
                           double* out_prices, double* out_pnls) {
    if (!engine || !out_prices || !out_pnls) return MAPE_ERR_NULL_HANDLE;
    if (!valid_market(spot, vol, maturity) || strike <= 0.0)
        return MAPE_ERR_BAD_INPUT;
    try {
        const mape::Option opt{to_type(type), to_exercise(exercise), strike,
                               maturity};
        const mape::MarketData mkt{spot, rate, vol, dividend};
        const std::vector<mape::Scenario> sc =
            mape::stress_scenarios(spot, vol_shock);

        // Dispatch the model once; run_scenarios is templated on the concrete
        // model, so we branch and call it per model type.
        std::vector<mape::ScenarioResult> table;
        switch (model) {
            case MAPE_MODEL_BINOMIAL:
                table = mape::run_scenarios(mape::BinomialTree{512}, opt, mkt,
                                            sc, engine->pool);
                break;
            case MAPE_MODEL_MONTE_CARLO:
                table = mape::run_scenarios(mape::MonteCarlo{200000}, opt, mkt,
                                            sc, engine->pool);
                break;
            case MAPE_MODEL_FINITE_DIFF:
                table = mape::run_scenarios(mape::FdPde{}, opt, mkt, sc,
                                            engine->pool);
                break;
            case MAPE_MODEL_BLACK_SCHOLES:
            default:
                table = mape::run_scenarios(mape::BlackScholes{}, opt, mkt, sc,
                                            engine->pool);
                break;
        }
        for (size_t i = 0; i < MAPE_STRESS_COUNT && i < table.size(); ++i) {
            out_prices[i] = table[i].price;
            out_pnls[i] = table[i].pnl;
        }
        return MAPE_OK;
    } catch (...) {
        return MAPE_ERR_UNKNOWN;
    }
}

const char* mape_stress_scenario_name(size_t i) {
    return i < MAPE_STRESS_COUNT ? kStressNames[i] : nullptr;
}

const char* mape_version(void) { return "0.1.0"; }

}  // extern "C"
