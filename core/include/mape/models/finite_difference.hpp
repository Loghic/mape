#ifndef MAPE_MODELS_FINITE_DIFFERENCE_HPP
#define MAPE_MODELS_FINITE_DIFFERENCE_HPP

#include <algorithm>
#include <cmath>
#include <vector>

#include "mape/instruments.hpp"
#include "mape/market_data.hpp"

namespace mape {

// Crank-Nicolson finite-difference PDE solver for the Black-Scholes equation
// (plan §4/§13 stretch model). Discretises the BS PDE on a uniform spot grid
// and steps backward in time with the Crank-Nicolson scheme (theta = 1/2,
// second-order accurate and unconditionally stable), solving the resulting
// tridiagonal system each step with the Thomas algorithm.
//
// Like the binomial tree it handles American exercise — by projecting the
// continuation value onto the payoff after each time step (the explicit
// early-exercise step). For European options it converges to the closed-form
// Black-Scholes price as the grid is refined. Satisfies the PricingModel
// concept, so it drops into Pricer<FdPde>.
//
// The BS PDE (in spot S, value V, backward in time tau = T - t):
//   dV/dtau = 0.5 sigma^2 S^2 d2V/dS2 + (r - q) S dV/dS - r V
class FdPde {
public:
    // `spot_steps` grid points in S, `time_steps` steps in time. `s_max_mult`
    // sets the upper boundary S_max = s_max_mult * max(spot, strike).
    explicit FdPde(int spot_steps = 400, int time_steps = 400,
                   double s_max_mult = 4.0)
        : spot_steps_(spot_steps),
          time_steps_(time_steps),
          s_max_mult_(s_max_mult) {}

    double price(const Option& opt, const MarketData& mkt) const {
        const int M = spot_steps_;  // spot intervals (M+1 grid points)
        const int N = time_steps_;  // time steps
        const double K = opt.strike, T = opt.maturity;
        const double r = mkt.rate, q = mkt.dividend, sigma = mkt.vol;
        const bool is_call = opt.type == OptionType::Call;
        const bool american = opt.exercise == Exercise::American;

        const double s_max = s_max_mult_ * std::max(mkt.spot, K);
        const double ds = s_max / M;
        const double dt = T / N;

        auto payoff = [&](double S) {
            return is_call ? std::max(S - K, 0.0) : std::max(K - S, 0.0);
        };

        // Grid of option values at the current time layer, V[j] at S_j = j*ds.
        std::vector<double> v(M + 1);
        for (int j = 0; j <= M; ++j) v[j] = payoff(j * ds);

        // Tridiagonal coefficients for interior nodes j = 1..M-1.
        // Crank-Nicolson averages the explicit and implicit operators (theta =
        // 1/2). The spatial operator at node j:
        //   a_j V_{j-1} + b_j V_j + c_j V_{j+1}
        // with (using x = j as the index, S_j = j*ds):
        const double half = 0.5;
        std::vector<double> a(M + 1), b(M + 1), c(M + 1);
        for (int j = 1; j < M; ++j) {
            const double sig2j2 = sigma * sigma * j * j;
            const double rqj = (r - q) * j;
            // Coefficients of the spatial operator L (per unit dt).
            a[j] = 0.5 * (sig2j2 - rqj);  // multiplies V_{j-1}
            b[j] = -(sig2j2 + r);         // multiplies V_j
            c[j] = 0.5 * (sig2j2 + rqj);  // multiplies V_{j+1}
        }

        // Implicit (left) and explicit (right) tridiagonals: (I - theta*dt*L)
        // and (I + (1-theta)*dt*L).
        std::vector<double> lo(M + 1), di(M + 1), up(M + 1);  // implicit matrix
        for (int j = 1; j < M; ++j) {
            lo[j] = -half * dt * a[j];
            di[j] = 1.0 - half * dt * b[j];
            up[j] = -half * dt * c[j];
        }

        std::vector<double> rhs(M + 1), scratch(M + 1);

        // Step backward in time.
        for (int n = 0; n < N; ++n) {
            const double tau_next =
                (n + 1) * dt;  // time-to-maturity at new layer

            // Dirichlet boundary values at S=0 and S=S_max for the new layer.
            double v0, vM;
            if (is_call) {
                v0 = 0.0;
                vM = s_max * std::exp(-q * tau_next) -
                     K * std::exp(-r * tau_next);
                vM = std::max(vM, 0.0);
            } else {
                v0 = K * std::exp(-r * tau_next);
                vM = 0.0;
            }

            // Build RHS = (I + (1-theta)*dt*L) * v, for interior nodes.
            for (int j = 1; j < M; ++j) {
                rhs[j] = (1.0 + half * dt * b[j]) * v[j] +
                         half * dt * a[j] * v[j - 1] +
                         half * dt * c[j] * v[j + 1];
            }
            // Fold the (known) boundary values into the first/last interior
            // eqns.
            rhs[1] -= lo[1] * v0;
            rhs[M - 1] -= up[M - 1] * vM;

            // Solve the tridiagonal system di/lo/up * v_new = rhs (Thomas) for
            // interior nodes 1..M-1.
            thomas_solve(lo, di, up, rhs, scratch, 1, M - 1);

            for (int j = 1; j < M; ++j) v[j] = rhs[j];
            v[0] = v0;
            v[M] = vM;

            // American early exercise: value can't drop below immediate payoff.
            if (american) {
                for (int j = 0; j <= M; ++j)
                    v[j] = std::max(v[j], payoff(j * ds));
            }
        }

        // Interpolate the solution at the actual spot.
        const double pos = mkt.spot / ds;
        int j = static_cast<int>(pos);
        if (j < 0) j = 0;
        if (j >= M) j = M - 1;
        const double w = pos - j;
        return v[j] * (1.0 - w) + v[j + 1] * w;
    }

    int spot_steps() const noexcept { return spot_steps_; }
    int time_steps() const noexcept { return time_steps_; }

private:
    // Thomas algorithm: solve a tridiagonal system in O(n) for indices
    // [first, last]. `lo`/`di`/`up` are the sub/main/super diagonals; `d` is
    // the RHS (overwritten with the solution); `cp` is scratch of size >=
    // last+1.
    static void thomas_solve(const std::vector<double>& lo,
                             const std::vector<double>& di,
                             const std::vector<double>& up,
                             std::vector<double>& d, std::vector<double>& cp,
                             int first, int last) {
        cp[first] = up[first] / di[first];
        d[first] = d[first] / di[first];
        for (int i = first + 1; i <= last; ++i) {
            const double m = di[i] - lo[i] * cp[i - 1];
            cp[i] = up[i] / m;
            d[i] = (d[i] - lo[i] * d[i - 1]) / m;
        }
        for (int i = last - 1; i >= first; --i) {
            d[i] -= cp[i] * d[i + 1];
        }
    }

    int spot_steps_;
    int time_steps_;
    double s_max_mult_;
};

}  // namespace mape

#endif  // MAPE_MODELS_FINITE_DIFFERENCE_HPP
