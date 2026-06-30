#ifndef MAPE_COUNTER_RNG_HPP
#define MAPE_COUNTER_RNG_HPP

#include <cmath>
#include <cstdint>

// Counter-based RNG for *deterministic* parallel Monte Carlo (plan §16.5).
//
// A normal stateful generator (std::mt19937_64) ties its output to the order in
// which it's consumed — so when we chunk paths across threads, changing the
// thread count changes which draw each path sees, and the price wobbles. A
// counter-based RNG removes that: the n-th draw is a pure stateless function of
// (key, n). Path number `i` therefore always gets the same value no matter
// which thread computes it, so a fixed seed yields an identical price at 1, 2,
// or 8 threads.
//
// This is a small SplitMix64-style bijection used as a counter mixer — not a
// full Philox/Threefry, but it gives the property that matters here
// (statelessness + good per-index decorrelation) in a dependency-free header.

namespace mape {

// Stateless 64-bit mix of (key, counter) -> uniform bits. Two independent
// SplitMix64 finalizers keep nearby (key, counter) pairs well-separated.
inline std::uint64_t counter_bits(std::uint64_t key, std::uint64_t counter) {
    std::uint64_t z = key * 0x9E3779B97F4A7C15ULL + counter + 0xD1B54A32D192ED03ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    // A second pass mixing the counter back in, so the (key, counter) lattice
    // doesn't leave structure.
    z += counter * 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 29)) * 0xBF58476D1CE4E5B9ULL;
    return z ^ (z >> 32);
}

// Uniform double in (0, 1) from 53 high bits (avoids exactly 0, which would
// break the inverse-normal below).
inline double counter_uniform(std::uint64_t key, std::uint64_t counter) {
    const std::uint64_t bits = counter_bits(key, counter) >> 11;  // 53 bits
    const double u = (static_cast<double>(bits) + 0.5) * (1.0 / 9007199254740992.0);
    return u;  // in (0, 1)
}

// Inverse standard-normal CDF (Acklam's rational approximation, |err| < 1.2e-9).
// Maps a uniform draw to a standard normal — the counter-based analogue of
// std::normal_distribution, but stateless and reproducible per index.
inline double inverse_normal_cdf(double p) {
    // Coefficients for Acklam's algorithm.
    static const double a[] = {-3.969683028665376e+01, 2.209460984245205e+02,
                               -2.759285104469687e+02, 1.383577518672690e+02,
                               -3.066479806614716e+01, 2.506628277459239e+00};
    static const double b[] = {-5.447609879822406e+01, 1.615858368580409e+02,
                               -1.556989798598866e+02, 6.680131188771972e+01,
                               -1.328068155288572e+01};
    static const double c[] = {-7.784894002430293e-03, -3.223964580411365e-01,
                               -2.400758277161838e+00, -2.549732539343734e+00,
                               4.374664141464968e+00, 2.938163982698783e+00};
    static const double d[] = {7.784695709041462e-03, 3.224671290700398e-01,
                               2.445134137142996e+00, 3.754408661907416e+00};
    const double plow = 0.02425, phigh = 1.0 - 0.02425;
    if (p < plow) {
        const double q = std::sqrt(-2.0 * std::log(p));
        return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q +
                c[5]) /
               ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
    }
    if (p > phigh) {
        const double q = std::sqrt(-2.0 * std::log(1.0 - p));
        return -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q +
                 c[5]) /
               ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
    }
    const double q = p - 0.5;
    const double r = q * q;
    return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r + a[5]) *
           q /
           (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r + b[4]) * r + 1.0);
}

// The n-th standard-normal draw for a given key — pure function of (key, n).
inline double counter_normal(std::uint64_t key, std::uint64_t index) {
    return inverse_normal_cdf(counter_uniform(key, index));
}

}  // namespace mape

#endif  // MAPE_COUNTER_RNG_HPP
