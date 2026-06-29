//! Safe Rust wrapper over the `extern "C"` pricing API (`mape_c_api.h`).
//!
//! The raw FFI is confined to the `ffi` module; everything public is safe.
//! `Engine` owns the C handle and frees it on `Drop`, enforcing the plan's
//! ownership rule ("whoever creates must destroy") at the type level.

use std::os::raw::{c_char, c_double};

/// Pricing model selector. Discriminants match the C enum `MapeModel`.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
#[repr(i32)]
pub enum Model {
    BlackScholes = 0,
    Binomial = 1,
    MonteCarlo = 2,
}

/// Call or put. Matches `MapeOptionType`.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
#[repr(i32)]
pub enum OptionType {
    Call = 0,
    Put = 1,
}

/// Exercise style. Matches `MapeExercise`.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
#[repr(i32)]
pub enum Exercise {
    European = 0,
    American = 1,
}

/// Status codes returned by the `_ex` C entry points. Matches `MapeStatus`.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
#[repr(i32)]
pub enum Status {
    Ok = 0,
    NullHandle = 1,
    BadInput = 2,
    Unknown = 99,
}

/// The set of market + contract inputs shared by the pricing calls.
#[derive(Copy, Clone, Debug)]
pub struct Quote {
    pub spot: f64,
    pub strike: f64,
    pub rate: f64,
    pub vol: f64,
    pub maturity: f64,
    pub dividend: f64,
}

mod ffi {
    use super::*;

    // Opaque handle: zero-sized, never constructed in Rust. Matches the C
    // `struct MapeEngine;` forward declaration.
    #[repr(C)]
    pub struct MapeEngine {
        _private: [u8; 0],
    }

    extern "C" {
        pub fn mape_create() -> *mut MapeEngine;
        pub fn mape_destroy(engine: *mut MapeEngine);

        pub fn mape_price(
            engine: *mut MapeEngine,
            model: i32,
            ty: i32,
            exercise: i32,
            spot: c_double,
            strike: c_double,
            rate: c_double,
            vol: c_double,
            maturity: c_double,
            dividend: c_double,
        ) -> c_double;

        pub fn mape_price_ex(
            engine: *mut MapeEngine,
            model: i32,
            ty: i32,
            exercise: i32,
            spot: c_double,
            strike: c_double,
            rate: c_double,
            vol: c_double,
            maturity: c_double,
            dividend: c_double,
            out_price: *mut c_double,
        ) -> i32;

        pub fn mape_delta(
            engine: *mut MapeEngine,
            ty: i32,
            spot: c_double,
            strike: c_double,
            rate: c_double,
            vol: c_double,
            maturity: c_double,
            dividend: c_double,
        ) -> c_double;

        pub fn mape_gamma(
            engine: *mut MapeEngine,
            ty: i32,
            spot: c_double,
            strike: c_double,
            rate: c_double,
            vol: c_double,
            maturity: c_double,
            dividend: c_double,
        ) -> c_double;

        pub fn mape_vega(
            engine: *mut MapeEngine,
            ty: i32,
            spot: c_double,
            strike: c_double,
            rate: c_double,
            vol: c_double,
            maturity: c_double,
            dividend: c_double,
        ) -> c_double;

        pub fn mape_price_portfolio(
            engine: *mut MapeEngine,
            model: i32,
            ty: i32,
            exercise: i32,
            spot: c_double,
            rate: c_double,
            vol: c_double,
            dividend: c_double,
            strikes: *const c_double,
            maturities: *const c_double,
            count: usize,
            out_prices: *mut c_double,
        ) -> i32;

        pub fn mape_ad_greek(
            engine: *mut MapeEngine,
            greek: i32,
            ty: i32,
            spot: c_double,
            strike: c_double,
            rate: c_double,
            vol: c_double,
            maturity: c_double,
            dividend: c_double,
        ) -> c_double;

        pub fn mape_price_exotic(
            engine: *mut MapeEngine,
            exotic: i32,
            ty: i32,
            spot: c_double,
            strike: c_double,
            rate: c_double,
            vol: c_double,
            maturity: c_double,
            dividend: c_double,
            barrier: c_double,
            barrier_kind: i32,
            steps: usize,
            paths: usize,
        ) -> c_double;

        pub fn mape_implied_vol(
            engine: *mut MapeEngine,
            ty: i32,
            market_price: c_double,
            spot: c_double,
            strike: c_double,
            rate: c_double,
            maturity: c_double,
            dividend: c_double,
        ) -> c_double;

        pub fn mape_price_bond(
            engine: *mut MapeEngine,
            face: c_double,
            coupon: c_double,
            maturity: c_double,
            frequency: i32,
            rate: c_double,
        ) -> c_double;

        pub fn mape_price_fx_forward(
            engine: *mut MapeEngine,
            spot: c_double,
            strike: c_double,
            maturity: c_double,
            domestic_rate: c_double,
            foreign_rate: c_double,
        ) -> c_double;

        pub fn mape_fx_forward_rate(
            engine: *mut MapeEngine,
            spot: c_double,
            maturity: c_double,
            domestic_rate: c_double,
            foreign_rate: c_double,
        ) -> c_double;

        pub fn mape_convergence(
            engine: *mut MapeEngine,
            model: i32,
            ty: i32,
            spot: c_double,
            strike: c_double,
            rate: c_double,
            vol: c_double,
            maturity: c_double,
            dividend: c_double,
            sample_sizes: *const c_double,
            n: usize,
            out_prices: *mut c_double,
        ) -> i32;

        pub fn mape_version() -> *const c_char;
    }
}

/// Forward-mode AD Greek selector. Discriminants match the C enum `MapeGreek`.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
#[repr(i32)]
pub enum AdGreek {
    Delta = 0,
    Vega = 1,
    Rho = 2,
}

/// Exotic, path-dependent option family. Matches `MapeExotic`.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
#[repr(i32)]
pub enum Exotic {
    Asian = 0,
    Barrier = 1,
    Lookback = 2,
}

/// Barrier monitoring style. Matches `MapeBarrierKind`.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
#[repr(i32)]
pub enum BarrierKind {
    UpAndOut = 0,
    DownAndOut = 1,
    UpAndIn = 2,
    DownAndIn = 3,
}

/// Closed-form Greeks for an option (all per-unit).
#[derive(Copy, Clone, Debug, Default)]
pub struct Greeks {
    pub delta: f64,
    pub gamma: f64,
    pub vega: f64,
}

/// Safe owner of a C pricing engine.
pub struct Engine {
    handle: *mut ffi::MapeEngine,
}

impl Engine {
    /// Create a new engine. Returns `None` if the C side failed to allocate.
    pub fn new() -> Option<Self> {
        // SAFETY: mape_create has no preconditions and returns NULL on failure.
        let handle = unsafe { ffi::mape_create() };
        if handle.is_null() {
            None
        } else {
            Some(Engine { handle })
        }
    }

    /// Price a single option. Returns `None` if the C side reported NaN
    /// (invalid input).
    pub fn price(
        &self,
        model: Model,
        ty: OptionType,
        exercise: Exercise,
        q: Quote,
    ) -> Option<f64> {
        // SAFETY: handle is a valid, non-null pointer owned by self.
        let v = unsafe {
            ffi::mape_price(
                self.handle,
                model as i32,
                ty as i32,
                exercise as i32,
                q.spot,
                q.strike,
                q.rate,
                q.vol,
                q.maturity,
                q.dividend,
            )
        };
        if v.is_nan() {
            None
        } else {
            Some(v)
        }
    }

    /// Closed-form Greeks (delta/gamma/vega) for the given option.
    pub fn greeks(&self, ty: OptionType, q: Quote) -> Greeks {
        // SAFETY: same as above; each call is independent and side-effect free.
        unsafe {
            Greeks {
                delta: ffi::mape_delta(
                    self.handle, ty as i32, q.spot, q.strike, q.rate, q.vol,
                    q.maturity, q.dividend,
                ),
                gamma: ffi::mape_gamma(
                    self.handle, ty as i32, q.spot, q.strike, q.rate, q.vol,
                    q.maturity, q.dividend,
                ),
                vega: ffi::mape_vega(
                    self.handle, ty as i32, q.spot, q.strike, q.rate, q.vol,
                    q.maturity, q.dividend,
                ),
            }
        }
    }

    /// Price a book of options (same market, varying strike/maturity) using
    /// the engine's thread pool. `strikes` and `maturities` must be equal
    /// length; returns one price per instrument.
    pub fn price_portfolio(
        &self,
        model: Model,
        ty: OptionType,
        exercise: Exercise,
        spot: f64,
        rate: f64,
        vol: f64,
        dividend: f64,
        strikes: &[f64],
        maturities: &[f64],
    ) -> Result<Vec<f64>, Status> {
        assert_eq!(
            strikes.len(),
            maturities.len(),
            "strikes and maturities must be the same length"
        );
        let count = strikes.len();
        let mut out = vec![0.0_f64; count];
        // SAFETY: pointers are valid for `count` elements; out has room for
        // `count` doubles; the C side only writes within bounds.
        let status = unsafe {
            ffi::mape_price_portfolio(
                self.handle,
                model as i32,
                ty as i32,
                exercise as i32,
                spot,
                rate,
                vol,
                dividend,
                strikes.as_ptr(),
                maturities.as_ptr(),
                count,
                out.as_mut_ptr(),
            )
        };
        match status {
            0 => Ok(out),
            1 => Err(Status::NullHandle),
            2 => Err(Status::BadInput),
            _ => Err(Status::Unknown),
        }
    }

    /// Exact (AD) Greek for a European option. Returns `None` on invalid input.
    pub fn ad_greek(&self, greek: AdGreek, ty: OptionType, q: Quote) -> Option<f64> {
        // SAFETY: valid handle; scalar args; side-effect free.
        let v = unsafe {
            ffi::mape_ad_greek(
                self.handle, greek as i32, ty as i32, q.spot, q.strike, q.rate,
                q.vol, q.maturity, q.dividend,
            )
        };
        if v.is_nan() { None } else { Some(v) }
    }

    /// Price an exotic, path-dependent option via parallel Monte Carlo.
    /// `barrier`/`barrier_kind` are ignored unless `exotic == Exotic::Barrier`.
    #[allow(clippy::too_many_arguments)]
    pub fn price_exotic(
        &self,
        exotic: Exotic,
        ty: OptionType,
        q: Quote,
        barrier: f64,
        barrier_kind: BarrierKind,
        steps: usize,
        paths: usize,
    ) -> Option<f64> {
        // SAFETY: valid handle; scalar args.
        let v = unsafe {
            ffi::mape_price_exotic(
                self.handle, exotic as i32, ty as i32, q.spot, q.strike,
                q.rate, q.vol, q.maturity, q.dividend, barrier,
                barrier_kind as i32, steps, paths,
            )
        };
        if v.is_nan() { None } else { Some(v) }
    }

    /// Convergence series: price the chosen model at each `sample_size`
    /// (binomial step count, or MC path count). Returns one price per size.
    pub fn convergence(
        &self,
        model: Model,
        ty: OptionType,
        q: Quote,
        sample_sizes: &[f64],
    ) -> Result<Vec<f64>, Status> {
        let n = sample_sizes.len();
        let mut out = vec![0.0_f64; n];
        // SAFETY: sample_sizes valid for n reads; out has room for n writes.
        let status = unsafe {
            ffi::mape_convergence(
                self.handle, model as i32, ty as i32, q.spot, q.strike, q.rate,
                q.vol, q.maturity, q.dividend, sample_sizes.as_ptr(), n,
                out.as_mut_ptr(),
            )
        };
        match status {
            0 => Ok(out),
            1 => Err(Status::NullHandle),
            2 => Err(Status::BadInput),
            _ => Err(Status::Unknown),
        }
    }

    /// Implied volatility that reproduces `market_price` for a European
    /// option. Returns `None` when no valid implied vol exists (price below
    /// intrinsic, above the no-arbitrage bound, or numerically undefined).
    pub fn implied_vol(
        &self,
        ty: OptionType,
        market_price: f64,
        spot: f64,
        strike: f64,
        rate: f64,
        maturity: f64,
        dividend: f64,
    ) -> Option<f64> {
        // SAFETY: valid handle; scalar args; side-effect free.
        let v = unsafe {
            ffi::mape_implied_vol(
                self.handle, ty as i32, market_price, spot, strike, rate,
                maturity, dividend,
            )
        };
        if v.is_nan() { None } else { Some(v) }
    }

    /// Present value of a fixed-coupon bond (continuous discounting at `rate`).
    pub fn price_bond(
        &self,
        face: f64,
        coupon: f64,
        maturity: f64,
        frequency: i32,
        rate: f64,
    ) -> Option<f64> {
        // SAFETY: valid handle; scalar args.
        let v = unsafe {
            ffi::mape_price_bond(self.handle, face, coupon, maturity, frequency, rate)
        };
        if v.is_nan() { None } else { Some(v) }
    }

    /// Present value of an FX forward (long, per unit foreign notional).
    pub fn price_fx_forward(
        &self,
        spot: f64,
        strike: f64,
        maturity: f64,
        domestic_rate: f64,
        foreign_rate: f64,
    ) -> Option<f64> {
        // SAFETY: valid handle; scalar args.
        let v = unsafe {
            ffi::mape_price_fx_forward(
                self.handle, spot, strike, maturity, domestic_rate, foreign_rate,
            )
        };
        if v.is_nan() { None } else { Some(v) }
    }

    /// Fair forward FX rate by covered interest parity.
    pub fn fx_forward_rate(
        &self,
        spot: f64,
        maturity: f64,
        domestic_rate: f64,
        foreign_rate: f64,
    ) -> Option<f64> {
        // SAFETY: valid handle; scalar args.
        let v = unsafe {
            ffi::mape_fx_forward_rate(
                self.handle, spot, maturity, domestic_rate, foreign_rate,
            )
        };
        if v.is_nan() { None } else { Some(v) }
    }
}

impl Drop for Engine {
    fn drop(&mut self) {
        // SAFETY: handle was produced by mape_create and is destroyed exactly
        // once here. mape_destroy tolerates (but won't receive) NULL.
        unsafe { ffi::mape_destroy(self.handle) };
    }
}

// The C engine guards its shared state (the thread pool) internally; a handle
// is fine to move between threads. We do NOT implement Sync because concurrent
// calls on one engine aren't part of the documented contract.
unsafe impl Send for Engine {}

/// Library version reported by the C core, e.g. `"0.1.0"`.
pub fn version() -> String {
    // SAFETY: mape_version returns a pointer to a static, NUL-terminated
    // string with 'static lifetime.
    unsafe {
        let ptr = ffi::mape_version();
        if ptr.is_null() {
            return String::new();
        }
        std::ffi::CStr::from_ptr(ptr).to_string_lossy().into_owned()
    }
}
