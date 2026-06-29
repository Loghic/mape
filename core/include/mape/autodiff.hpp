#ifndef MAPE_AUTODIFF_HPP
#define MAPE_AUTODIFF_HPP

#include <cmath>

namespace mape {

// Forward-mode automatic differentiation via dual numbers (plan §12).
//
// A Dual carries a value and its derivative w.r.t. one chosen input. Arithmetic
// propagates the derivative by the chain rule, so evaluating a function on a
// Dual seeded with d(input)=1 yields the function value AND its exact partial
// derivative — no bumping, no truncation error.
struct Dual {
    double v = 0.0;  // value
    double d = 0.0;  // derivative w.r.t. the seeded variable

    Dual() = default;
    Dual(double value) : v(value), d(0.0) {}        // constant
    Dual(double value, double deriv) : v(value), d(deriv) {}
};

inline Dual operator+(Dual a, Dual b) { return {a.v + b.v, a.d + b.d}; }
inline Dual operator-(Dual a, Dual b) { return {a.v - b.v, a.d - b.d}; }
inline Dual operator-(Dual a)         { return {-a.v, -a.d}; }
inline Dual operator*(Dual a, Dual b) { return {a.v * b.v, a.d * b.v + a.v * b.d}; }
inline Dual operator/(Dual a, Dual b) {
    return {a.v / b.v, (a.d * b.v - a.v * b.d) / (b.v * b.v)};
}

// Elementary functions with their derivatives.
inline Dual exp(Dual a)  { double e = std::exp(a.v);  return {e, a.d * e}; }
inline Dual log(Dual a)  { return {std::log(a.v), a.d / a.v}; }
inline Dual sqrt(Dual a) { double s = std::sqrt(a.v); return {s, a.d / (2.0 * s)}; }
inline Dual erfc(Dual a) {
    // d/dx erfc(x) = -2/sqrt(pi) * e^{-x^2}
    static const double two_over_sqrt_pi = 1.1283791670955126;
    return {std::erfc(a.v), a.d * (-two_over_sqrt_pi * std::exp(-a.v * a.v))};
}

}  // namespace mape

#endif  // MAPE_AUTODIFF_HPP
