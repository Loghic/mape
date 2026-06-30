#ifndef MAPE_MAPE_HPP
#define MAPE_MAPE_HPP

// Umbrella header for the pricing core. Including this pulls in the full
// public API: domain types, the templated engine, all models, and the
// threading utilities.

#include "mape/autodiff.hpp"
#include "mape/compile_time.hpp"
#include "mape/concepts.hpp"
#include "mape/ct_math.hpp"
#include "mape/exotic.hpp"
#include "mape/generator.hpp"
#include "mape/greeks_mixin.hpp"
#include "mape/implied_vol.hpp"
#include "mape/instruments.hpp"
#include "mape/market_data.hpp"
#include "mape/portfolio.hpp"
#include "mape/portfolio_compile_time.hpp"
#include "mape/pricer.hpp"
#include "mape/variance_reduction.hpp"
#include "mape/models/black_scholes.hpp"
#include "mape/models/black_scholes_ad.hpp"
#include "mape/models/binomial.hpp"
#include "mape/models/fixed_income.hpp"
#include "mape/models/lazy_monte_carlo.hpp"
#include "mape/models/monte_carlo.hpp"
#include "mape/models/path_monte_carlo.hpp"
#include "mape/threading/parallel_mc.hpp"
#include "mape/threading/sync_primitives.hpp"
#include "mape/threading/thread_pool.hpp"

#endif  // MAPE_MAPE_HPP
