#ifndef MAPE_PRICER_HPP
#define MAPE_PRICER_HPP

#include <utility>

#include "mape/concepts.hpp"
#include "mape/instruments.hpp"
#include "mape/market_data.hpp"

namespace mape {

// One generic engine, any conforming model — no shared base class, no virtual
// dispatch (plan §5.1). `Pricer<BlackScholes>`, `Pricer<BinomialTree>`,
// `Pricer<MonteCarlo>` are all distinct, fully-inlined types.
template <PricingModel Model>
class Pricer {
public:
    explicit Pricer(Model model) : model_(std::move(model)) {}

    double value(const Option& inst, const MarketData& mkt) const {
        return model_.price(inst, mkt);
    }

    const Model& model() const noexcept { return model_; }

private:
    Model model_;
};

// Deduction guide so `Pricer p{BlackScholes{}};` works without spelling the
// type.
template <typename Model>
Pricer(Model) -> Pricer<Model>;

}  // namespace mape

#endif  // MAPE_PRICER_HPP
