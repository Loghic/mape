#ifndef MAPE_EXOTIC_HPP
#define MAPE_EXOTIC_HPP

#include <algorithm>
#include <span>

#include "mape/instruments.hpp"

namespace mape {

// Path-dependent payoff callables (plan §12). Each takes a price path — a span
// of simulated spots, path[0..n) with path.back() the terminal value — and
// returns a cashflow. They satisfy the PathPayoff concept and plug straight
// into the path-aware Monte Carlo engine.

// Arithmetic-average Asian option: strike against the mean of the path.
struct AsianPayoff {
    OptionType type;
    double strike;

    double operator()(std::span<const double> path) const {
        if (path.empty()) return 0.0;
        double sum = 0.0;
        for (double s : path) sum += s;
        const double avg = sum / static_cast<double>(path.size());
        return type == OptionType::Call ? std::max(avg - strike, 0.0)
                                        : std::max(strike - avg, 0.0);
    }
};

enum class BarrierKind { UpAndOut, DownAndOut, UpAndIn, DownAndIn };

// Single-barrier option on the terminal payoff. Knock-out: worthless if the
// barrier is breached; knock-in: only alive if breached. Monitored discretely
// at each path step (continuous monitoring is a refinement, not done here).
struct BarrierPayoff {
    OptionType type;
    double strike;
    double barrier;
    BarrierKind kind;

    double operator()(std::span<const double> path) const {
        if (path.empty()) return 0.0;

        bool breached = false;
        for (double s : path) {
            switch (kind) {
                case BarrierKind::UpAndOut:
                case BarrierKind::UpAndIn:
                    if (s >= barrier) breached = true;
                    break;
                case BarrierKind::DownAndOut:
                case BarrierKind::DownAndIn:
                    if (s <= barrier) breached = true;
                    break;
            }
            if (breached) break;
        }

        const bool knock_in =
            kind == BarrierKind::UpAndIn || kind == BarrierKind::DownAndIn;
        const bool alive = knock_in ? breached : !breached;
        if (!alive) return 0.0;

        const double terminal = path.back();
        return type == OptionType::Call ? std::max(terminal - strike, 0.0)
                                        : std::max(strike - terminal, 0.0);
    }
};

// Lookback (floating-strike) call: pays terminal minus the path minimum; put
// pays path maximum minus terminal. A bonus exotic to show the design scales.
struct LookbackPayoff {
    OptionType type;

    double operator()(std::span<const double> path) const {
        if (path.empty()) return 0.0;
        const double terminal = path.back();
        if (type == OptionType::Call) {
            const double lo = *std::min_element(path.begin(), path.end());
            return std::max(terminal - lo, 0.0);
        }
        const double hi = *std::max_element(path.begin(), path.end());
        return std::max(hi - terminal, 0.0);
    }
};

}  // namespace mape

#endif  // MAPE_EXOTIC_HPP
