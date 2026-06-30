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
    Dual(double value) : v(value), d(0.0) {}  // constant
    Dual(double value, double deriv) : v(value), d(deriv) {}
};

inline Dual operator+(Dual a, Dual b) { return {a.v + b.v, a.d + b.d}; }
inline Dual operator-(Dual a, Dual b) { return {a.v - b.v, a.d - b.d}; }
inline Dual operator-(Dual a) { return {-a.v, -a.d}; }
inline Dual operator*(Dual a, Dual b) {
    return {a.v * b.v, a.d * b.v + a.v * b.d};
}
inline Dual operator/(Dual a, Dual b) {
    return {a.v / b.v, (a.d * b.v - a.v * b.d) / (b.v * b.v)};
}

// Elementary functions with their derivatives.
inline Dual exp(Dual a) {
    double e = std::exp(a.v);
    return {e, a.d * e};
}
inline Dual log(Dual a) { return {std::log(a.v), a.d / a.v}; }
inline Dual sqrt(Dual a) {
    double s = std::sqrt(a.v);
    return {s, a.d / (2.0 * s)};
}
inline Dual erfc(Dual a) {
    // d/dx erfc(x) = -2/sqrt(pi) * e^{-x^2}
    static const double two_over_sqrt_pi = 1.1283791670955126;
    return {std::erfc(a.v), a.d * (-two_over_sqrt_pi * std::exp(-a.v * a.v))};
}

// --- Second-order forward-mode AD --------------------------------------
//
// Dual2 carries a value, its first derivative, and its second derivative w.r.t.
// one seeded input. Evaluating a function on Dual2{x, 1, 0} yields f(x), f'(x),
// and f''(x) at once — which is exactly what gamma (d^2 price / d spot^2)
// needs, with no bumping. The operator rules are the standard second-order
// chain rule:
//   (fg)''  = f''g + 2 f'g' + f g''
//   (f/g)'' = (f'' - 2 (f/g)' g' - (f/g) g'') / g
//   h(f)''  = h''(f) (f')^2 + h'(f) f''
struct Dual2 {
    double v = 0.0;   // value
    double d = 0.0;   // first derivative
    double dd = 0.0;  // second derivative

    Dual2() = default;
    Dual2(double value) : v(value), d(0.0), dd(0.0) {}  // constant
    Dual2(double value, double d1, double d2) : v(value), d(d1), dd(d2) {}
};

inline Dual2 operator+(Dual2 a, Dual2 b) {
    return {a.v + b.v, a.d + b.d, a.dd + b.dd};
}
inline Dual2 operator-(Dual2 a, Dual2 b) {
    return {a.v - b.v, a.d - b.d, a.dd - b.dd};
}
inline Dual2 operator-(Dual2 a) { return {-a.v, -a.d, -a.dd}; }
inline Dual2 operator*(Dual2 a, Dual2 b) {
    return {a.v * b.v, a.d * b.v + a.v * b.d,
            a.dd * b.v + 2.0 * a.d * b.d + a.v * b.dd};
}
inline Dual2 operator/(Dual2 a, Dual2 b) {
    const double q = a.v / b.v;
    const double q1 = (a.d - q * b.d) / b.v;  // first deriv of a/b
    const double q2 = (a.dd - 2.0 * q1 * b.d - q * b.dd) / b.v;
    return {q, q1, q2};
}

// Chain rule helper for a scalar function h with known h', h''.
inline Dual2 chain2(Dual2 a, double h, double dh, double ddh) {
    return {h, dh * a.d, ddh * a.d * a.d + dh * a.dd};
}

inline Dual2 exp(Dual2 a) {
    const double e = std::exp(a.v);
    return chain2(a, e, e, e);
}
inline Dual2 log(Dual2 a) {
    return chain2(a, std::log(a.v), 1.0 / a.v, -1.0 / (a.v * a.v));
}
inline Dual2 sqrt(Dual2 a) {
    const double s = std::sqrt(a.v);
    return chain2(a, s, 0.5 / s, -0.25 / (s * a.v));
}
inline Dual2 erfc(Dual2 a) {
    static const double two_over_sqrt_pi = 1.1283791670955126;
    const double e = std::exp(-a.v * a.v);
    const double dh = -two_over_sqrt_pi * e;              // d/dx erfc
    const double ddh = two_over_sqrt_pi * 2.0 * a.v * e;  // d^2/dx^2 erfc
    return chain2(a, std::erfc(a.v), dh, ddh);
}

}  // namespace mape

#endif  // MAPE_AUTODIFF_HPP
