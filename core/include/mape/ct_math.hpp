#ifndef MAPE_CT_MATH_HPP
#define MAPE_CT_MATH_HPP

#include <cstdint>

// Compile-time math (plan §5.4). The C++20 <cmath> transcendentals are NOT
// constexpr (that arrives in C++23), so to price closed-form models inside a
// constant expression we need our own constexpr exp/log/sqrt and normal CDF.
//
// These live on a thin wrapper type, CtDouble, rather than plain `double`, so
// that the SAME generic Black-Scholes template (bs_price_generic) resolves its
// unqualified exp/log/sqrt/erfc calls — via ADL — to THESE constexpr overloads
// when instantiated with CtDouble, exactly as it resolves to the Dual overloads
// for automatic differentiation. One formula, three modes (runtime double,
// AD Dual, compile-time CtDouble).

namespace mape::ct {

// --- bare constexpr primitives on double -------------------------------

constexpr double abs_ct(double x) { return x < 0.0 ? -x : x; }

// Newton-Raphson square root. Converges quadratically; 60 iterations is far
// more than enough for double precision across our input range.
constexpr double sqrt_ct(double x) {
    if (x <= 0.0) return 0.0;
    double g = x > 1.0 ? x : 1.0;
    for (int i = 0; i < 60; ++i) {
        const double next = 0.5 * (g + x / g);
        if (abs_ct(next - g) <= 1e-17 * next) break;
        g = next;
    }
    return g;
}

// exp via range reduction (x = k*ln2 + r) + a Taylor series on the small
// remainder r. Range reduction keeps the series well-conditioned.
constexpr double exp_ct(double x) {
    constexpr double LN2 = 0.6931471805599453;
    // k = round(x / ln2)
    const double kf = x / LN2;
    long long k = static_cast<long long>(kf >= 0 ? kf + 0.5 : kf - 0.5);
    const double r = x - static_cast<double>(k) * LN2;  // |r| <= ln2/2

    // Taylor series for e^r.
    double term = 1.0, sum = 1.0;
    for (int n = 1; n < 30; ++n) {
        term *= r / n;
        sum += term;
        if (abs_ct(term) < 1e-18) break;
    }

    // Multiply by 2^k via repeated doubling/halving (constexpr-friendly).
    double pow2 = 1.0;
    if (k >= 0)
        for (long long i = 0; i < k; ++i) pow2 *= 2.0;
    else
        for (long long i = 0; i < -k; ++i) pow2 *= 0.5;
    return sum * pow2;
}

// log via log(m * 2^e) = e*ln2 + log(m), with the mantissa log from the
// atanh series log((1+t)/(1-t)) which converges fast for m in [1, 2).
constexpr double log_ct(double x) {
    if (x <= 0.0) return 0.0;  // domain guard; callers keep x > 0
    constexpr double LN2 = 0.6931471805599453;
    int e = 0;
    double m = x;
    while (m >= 2.0) {
        m *= 0.5;
        ++e;
    }
    while (m < 1.0) {
        m *= 2.0;
        --e;
    }
    // m in [1, 2); t = (m-1)/(m+1) in [0, 1/3).
    const double t = (m - 1.0) / (m + 1.0);
    const double t2 = t * t;
    double term = t, sum = 0.0;
    for (int n = 1; n < 60; n += 2) {
        sum += term / n;
        term *= t2;
        if (abs_ct(term) < 1e-19) break;
    }
    return 2.0 * sum + e * LN2;
}

// Abramowitz & Stegun 7.1.26 approximation of erf (|error| <= 1.5e-7), made
// constexpr. erfc(x) = 1 - erf(x). The coefficients are materialised once at
// compile time via the consteval table below.
struct ErfCoeffs {
    double p, a1, a2, a3, a4, a5;
};

// consteval guarantees this table is built at compile time, never at runtime.
consteval ErfCoeffs as_erf_coeffs() {
    return ErfCoeffs{0.3275911,   0.254829592,  -0.284496736,
                     1.421413741, -1.453152027, 1.061405429};
}

constexpr double erf_ct(double x) {
    constexpr ErfCoeffs c = as_erf_coeffs();
    const double sign = x < 0.0 ? -1.0 : 1.0;
    const double ax = abs_ct(x);
    const double t = 1.0 / (1.0 + c.p * ax);
    const double poly =
        ((((c.a5 * t + c.a4) * t + c.a3) * t + c.a2) * t + c.a1) * t;
    const double y = 1.0 - poly * exp_ct(-ax * ax);
    return sign * y;
}

constexpr double erfc_ct(double x) { return 1.0 - erf_ct(x); }

// --- CtDouble: a scalar type whose ADL math IS the constexpr math -------
//
// bs_price_generic calls unqualified exp/log/sqrt/erfc; for CtDouble those
// resolve here. Arithmetic operators are constexpr so the whole formula folds.
struct CtDouble {
    double v = 0.0;
    constexpr CtDouble() = default;
    constexpr CtDouble(double x) : v(x) {}  // implicit, matches `T(0.5)` etc.
    constexpr explicit operator double() const { return v; }
};

constexpr CtDouble operator+(CtDouble a, CtDouble b) { return {a.v + b.v}; }
constexpr CtDouble operator-(CtDouble a, CtDouble b) { return {a.v - b.v}; }
constexpr CtDouble operator-(CtDouble a) { return {-a.v}; }
constexpr CtDouble operator*(CtDouble a, CtDouble b) { return {a.v * b.v}; }
constexpr CtDouble operator/(CtDouble a, CtDouble b) { return {a.v / b.v}; }

constexpr CtDouble exp(CtDouble a) { return {exp_ct(a.v)}; }
constexpr CtDouble log(CtDouble a) { return {log_ct(a.v)}; }
constexpr CtDouble sqrt(CtDouble a) { return {sqrt_ct(a.v)}; }
constexpr CtDouble erfc(CtDouble a) { return {erfc_ct(a.v)}; }

}  // namespace mape::ct

#endif  // MAPE_CT_MATH_HPP
