#ifndef MAPE_GREEKS_MIXIN_HPP
#define MAPE_GREEKS_MIXIN_HPP

#include "mape/concepts.hpp"
#include "mape/instruments.hpp"
#include "mape/market_data.hpp"

namespace mape {

// CRTP bump-and-revalue Greeks mixin (plan §15.2). A model inherits
// `BumpGreeks<Model>` and gains delta/gamma/vega via central finite
// differences computed through the derived class's own `price` — resolved
// statically (CRTP), so there's no virtual-call overhead and one definition of
// bumped Greeks is reused across every model.
//
//   struct MyModel : BumpGreeks<MyModel> {
//       double price(const Option&, const MarketData&) const;
//   };
//
// It deliberately mirrors the (Option, MarketData) signature the rest of the
// engine uses, so it drops into the parallel-Greeks path (plan §5.2) unchanged.
template <typename Model>
class BumpGreeks {
    // Protected so only the derived Model can construct the base — guards
    // against an accidental mismatched CRTP parameter (clang-tidy
    // bugprone-crtp-constructor-accessibility).
    BumpGreeks() = default;
    friend Model;

public:
    double bump_delta(const Option& opt, const MarketData& mkt,
                      double h = 1e-4) const {
        const Model& m = static_cast<const Model&>(*this);
        MarketData up = mkt, dn = mkt;
        up.spot += h;
        dn.spot -= h;
        return (m.price(opt, up) - m.price(opt, dn)) / (2.0 * h);
    }

    double bump_gamma(const Option& opt, const MarketData& mkt,
                      double h = 1e-3) const {
        const Model& m = static_cast<const Model&>(*this);
        MarketData up = mkt, dn = mkt;
        up.spot += h;
        dn.spot -= h;
        return (m.price(opt, up) - 2.0 * m.price(opt, mkt) + m.price(opt, dn)) /
               (h * h);
    }

    double bump_vega(const Option& opt, const MarketData& mkt,
                     double h = 1e-4) const {
        const Model& m = static_cast<const Model&>(*this);
        MarketData up = mkt, dn = mkt;
        up.vol += h;
        dn.vol -= h;
        return (m.price(opt, up) - m.price(opt, dn)) / (2.0 * h);
    }
};

// --- Capability detection (plan §15.3) ----------------------------------
//
// Some models expose an exact analytic delta (Black-Scholes); others don't.
// Detect it with a concept and dispatch: use the analytic form when present,
// otherwise fall back to the CRTP bump above. The engine always uses the most
// accurate delta available, transparently to the caller. (In pre-concepts C++
// this is the std::void_t SFINAE detection idiom; C++20 lets us write it
// directly as a concept.)
template <typename M>
concept HasAnalyticDelta =
    requires(const M m, const Option& opt, const MarketData& mkt) {
        { m.delta(opt, mkt) } -> std::convertible_to<double>;
    };

// best_delta: analytic if the model provides it, bumped otherwise.
template <typename Model>
double best_delta(const Model& model, const Option& opt,
                  const MarketData& mkt) {
    if constexpr (HasAnalyticDelta<Model>) {
        return model.delta(opt, mkt);          // exact, no finite-difference
    } else {
        // The model must derive BumpGreeks<Model> for this path to exist.
        return model.bump_delta(opt, mkt);
    }
}

}  // namespace mape

#endif  // MAPE_GREEKS_MIXIN_HPP
