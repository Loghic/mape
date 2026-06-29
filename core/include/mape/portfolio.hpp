#ifndef MAPE_PORTFOLIO_HPP
#define MAPE_PORTFOLIO_HPP

#include <future>
#include <vector>

#include "mape/concepts.hpp"
#include "mape/instruments.hpp"
#include "mape/market_data.hpp"
#include "mape/threading/thread_pool.hpp"

namespace mape {

// Price a book of options concurrently against one market snapshot using the
// thread pool (plan §5.2 "thread pool for portfolio pricing"). Order of the
// returned prices matches the input order.
template <PricingModel Model>
std::vector<double> price_portfolio(const Model& model,
                                    const std::vector<Option>& book,
                                    const MarketData& mkt,
                                    ThreadPool& pool) {
    std::vector<std::future<double>> futures;
    futures.reserve(book.size());
    for (const Option& opt : book) {
        futures.push_back(pool.submit(
            [&model, opt, &mkt] { return model.price(opt, mkt); }));
    }
    std::vector<double> out;
    out.reserve(book.size());
    for (auto& f : futures) out.push_back(f.get());
    return out;
}

}  // namespace mape

#endif  // MAPE_PORTFOLIO_HPP
