#ifndef MAPE_RISK_HPP
#define MAPE_RISK_HPP

#include <future>
#include <string>
#include <vector>

#include "mape/concepts.hpp"
#include "mape/instruments.hpp"
#include "mape/market_data.hpp"
#include "mape/threading/thread_pool.hpp"

// Risk + scenario engine (plan §16.4). Perturb the market, reprice, tabulate
// P&L. The same machinery serves delta/gamma/vega/rho, stress tests, and (with
// the §16.2 curves/surfaces) bucketed risk. It's embarrassingly parallel, so it
// runs scenarios concurrently on the existing ThreadPool.

namespace mape {

// A named market perturbation: additive shifts to each scalar field. (Shifting
// the flat scalars covers the common case; curve/surface bucket shifts can be
// layered on later by mutating MarketData::curve/surface in a richer Scenario.)
struct Scenario {
    std::string name;
    double d_spot = 0.0;      // shift to spot
    double d_rate = 0.0;      // shift to rate
    double d_vol = 0.0;       // shift to volatility
    double d_dividend = 0.0;  // shift to dividend yield

    MarketData apply(const MarketData& base) const {
        MarketData m = base;
        m.spot += d_spot;
        m.rate += d_rate;
        m.vol += d_vol;
        m.dividend += d_dividend;
        return m;
    }
};

// One row of the scenario table.
struct ScenarioResult {
    std::string name;
    double price = 0.0;  // repriced value under the scenario
    double pnl = 0.0;    // price - base_price
};

// Run a set of scenarios for one option against a base market, concurrently on
// the thread pool. Returns one row per scenario (order preserved), each with
// the repriced value and the P&L versus the base price.
template <PricingModel Model>
std::vector<ScenarioResult> run_scenarios(const Model& model, const Option& opt,
                                          const MarketData& base,
                                          const std::vector<Scenario>& scenarios,
                                          ThreadPool& pool) {
    const double base_price = model.price(opt, base);

    std::vector<std::future<double>> futures;
    futures.reserve(scenarios.size());
    for (const Scenario& s : scenarios) {
        const MarketData shifted = s.apply(base);
        futures.push_back(pool.submit(
            [&model, opt, shifted] { return model.price(opt, shifted); }));
    }

    std::vector<ScenarioResult> table;
    table.reserve(scenarios.size());
    for (std::size_t i = 0; i < scenarios.size(); ++i) {
        const double px = futures[i].get();
        table.push_back({scenarios[i].name, px, px - base_price});
    }
    return table;
}

// --- standard scenario sets --------------------------------------------

// Central-difference bump scenarios for the four first-order Greeks, plus a
// gamma pair. The caller divides the relevant P&Ls by the bump to get the
// sensitivity (e.g. delta = (up.pnl - down.pnl) / (2*h_spot)). Returned in a
// fixed order so callers can index them.
inline std::vector<Scenario> greek_bump_scenarios(double h_spot = 1.0,
                                                  double h_vol = 0.01,
                                                  double h_rate = 0.0001) {
    return {
        Scenario{"spot_up", +h_spot, 0, 0, 0},
        Scenario{"spot_down", -h_spot, 0, 0, 0},
        Scenario{"vol_up", 0, 0, +h_vol, 0},
        Scenario{"vol_down", 0, 0, -h_vol, 0},
        Scenario{"rate_up", 0, +h_rate, 0, 0},
        Scenario{"rate_down", 0, -h_rate, 0, 0},
    };
}

// A small stress-test set: large moves a desk might monitor. Spot shocks are
// expressed relative to the base spot, so the helper takes it as an argument.
inline std::vector<Scenario> stress_scenarios(double base_spot,
                                              double vol_shock = 0.10) {
    return {
        Scenario{"spot -20%", -0.20 * base_spot, 0, 0, 0},
        Scenario{"spot -10%", -0.10 * base_spot, 0, 0, 0},
        Scenario{"spot +10%", +0.10 * base_spot, 0, 0, 0},
        Scenario{"spot +20%", +0.20 * base_spot, 0, 0, 0},
        Scenario{"vol +10pt", 0, 0, +vol_shock, 0},
        Scenario{"crash: spot -20%, vol +10pt", -0.20 * base_spot, 0,
                 +vol_shock, 0},
    };
}

// Aggregate first-order Greeks for one option from a bump run. Convenience
// wrapper that builds the bump scenarios, runs them, and finite-differences.
struct PortfolioGreeks {
    double delta = 0.0;
    double gamma = 0.0;
    double vega = 0.0;
    double rho = 0.0;
};

template <PricingModel Model>
PortfolioGreeks scenario_greeks(const Model& model, const Option& opt,
                                const MarketData& base, ThreadPool& pool,
                                double h_spot = 1.0, double h_vol = 0.01,
                                double h_rate = 0.0001) {
    const auto sc = greek_bump_scenarios(h_spot, h_vol, h_rate);
    const auto t = run_scenarios(model, opt, base, sc, pool);
    const double base_price = model.price(opt, base);
    // t[0]=spot_up, [1]=spot_down, [2]=vol_up, [3]=vol_down, [4]=rate_up, [5]=rate_down
    PortfolioGreeks g;
    g.delta = (t[0].price - t[1].price) / (2.0 * h_spot);
    g.gamma = (t[0].price - 2.0 * base_price + t[1].price) / (h_spot * h_spot);
    g.vega = (t[2].price - t[3].price) / (2.0 * h_vol);
    g.rho = (t[4].price - t[5].price) / (2.0 * h_rate);
    return g;
}

}  // namespace mape

#endif  // MAPE_RISK_HPP
