//! egui desktop front-end for the multi-asset pricing engine.
//!
//! The GUI does no math: it formats inputs, calls the C++ engine over the safe
//! FFI bridge, and renders the results (plan §7).

// The bridge is a complete, intentionally-public binding of the C API. A few
// items (e.g. the Status::Ok variant, mirroring the C ABI) are not used from
// Rust, so dead-code analysis is relaxed at their definitions.
mod bridge;
mod data;

use bridge::{
    AdGreek, BarrierKind, Engine, Exercise, Exotic, Greeks, Model, OptionType, Quote, StressRow,
    SviFit,
};
use data::DataStore;
use eframe::egui;
use egui_plot::{Legend, Line, Plot, PlotPoints, Points};
use std::path::PathBuf;

/// Palette. Two roles, deliberately split so text stays readable on the dark
/// theme while plot fills can stay saturated:
///   - `ACCENT`      — saturated brand blue, for plot lines/fills and the active
///                     tab selection (sits on dark backgrounds as a *fill*).
///   - `ACCENT_TEXT` — a much lighter blue used only for *text* (section titles,
///                     headings, the price). The old dim blue (#60A5FA) was too
///                     low-contrast on the ~#202020 panel; this is near-pastel.
const ACCENT: egui::Color32 = egui::Color32::from_rgb(0x3B, 0x82, 0xF6);
const ACCENT_TEXT: egui::Color32 = egui::Color32::from_rgb(0x9E, 0xC5, 0xFF);
const HEADING: egui::Color32 = egui::Color32::from_rgb(0xE8, 0xF0, 0xFF);
const GOOD: egui::Color32 = egui::Color32::from_rgb(0x4A, 0xDE, 0x80);
const BAD: egui::Color32 = egui::Color32::from_rgb(0xFF, 0x6B, 0x6B);

fn main() -> eframe::Result<()> {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default().with_inner_size([840.0, 640.0]),
        ..Default::default()
    };
    eframe::run_native(
        "Multi-Asset Pricing Engine",
        options,
        Box::new(|cc| {
            configure_style(&cc.egui_ctx);
            Box::new(App::new())
        }),
    )
}

/// Apply a tasteful, consistent look: roomier spacing, softer rounding, an
/// accent-tinted active selection, and monospace-friendly defaults. Builds on
/// egui's dark theme rather than replacing it.
fn configure_style(ctx: &egui::Context) {
    let mut style = (*ctx.style()).clone();
    let s = &mut style.spacing;
    s.item_spacing = egui::vec2(10.0, 8.0);
    s.button_padding = egui::vec2(10.0, 5.0);
    s.menu_margin = egui::Margin::same(8.0);
    s.window_margin = egui::Margin::same(10.0);
    s.interact_size.y = 26.0;

    let v = &mut style.visuals;
    let rounding = egui::Rounding::same(6.0);
    v.widgets.noninteractive.rounding = rounding;
    v.widgets.inactive.rounding = rounding;
    v.widgets.hovered.rounding = rounding;
    v.widgets.active.rounding = rounding;
    v.widgets.open.rounding = rounding;
    v.window_rounding = egui::Rounding::same(8.0);

    // Darken the panel a touch so the (lighter) section cards stand out, and
    // push text to near-white for strong contrast. We set the per-widget text
    // strokes directly rather than `override_text_color`, so `.weak()`,
    // `.strong()`, and colored `RichText` still differ from body text.
    v.panel_fill = egui::Color32::from_gray(0x1C);
    v.window_fill = egui::Color32::from_gray(0x22);
    let body = egui::Color32::from_gray(0xE0); // near-white body text
    v.widgets.noninteractive.fg_stroke.color = body;
    v.widgets.inactive.fg_stroke.color = body;
    v.widgets.hovered.fg_stroke.color = egui::Color32::WHITE;
    v.widgets.active.fg_stroke.color = egui::Color32::WHITE;

    // Tint selections with the brand accent.
    v.selection.bg_fill = ACCENT.linear_multiply(0.55);
    v.selection.stroke = egui::Stroke::new(1.0_f32, ACCENT_TEXT);
    v.hyperlink_color = ACCENT_TEXT;

    // Slightly larger body text for readability.
    use egui::{FontFamily, FontId, TextStyle};
    style.text_styles = [
        (
            TextStyle::Heading,
            FontId::new(20.0, FontFamily::Proportional),
        ),
        (TextStyle::Body, FontId::new(15.0, FontFamily::Proportional)),
        (
            TextStyle::Monospace,
            FontId::new(14.0, FontFamily::Monospace),
        ),
        (
            TextStyle::Button,
            FontId::new(15.0, FontFamily::Proportional),
        ),
        (
            TextStyle::Small,
            FontId::new(12.0, FontFamily::Proportional),
        ),
    ]
    .into();

    ctx.set_style(style);
}

/// Human-friendly model name for the combo box (nicer than the Debug spelling).
fn model_label(m: Model) -> &'static str {
    match m {
        Model::BlackScholes => "Black-Scholes",
        Model::Binomial => "Binomial tree",
        Model::MonteCarlo => "Monte Carlo",
        Model::FiniteDiff => "Finite difference (PDE)",
    }
}

/// A titled, framed section — groups related controls/results with a subtle
/// background and an accent heading, so each tab reads as distinct cards rather
/// than a flat wall of widgets.
fn section<R>(ui: &mut egui::Ui, title: &str, add_contents: impl FnOnce(&mut egui::Ui) -> R) -> R {
    egui::Frame::group(ui.style())
        .inner_margin(egui::Margin::same(12.0))
        .rounding(egui::Rounding::same(8.0))
        .fill(egui::Color32::from_gray(0x28))
        .stroke(egui::Stroke::new(1.0_f32, egui::Color32::from_gray(0x42)))
        .show(ui, |ui| {
            ui.label(
                egui::RichText::new(title)
                    .color(ACCENT_TEXT)
                    .strong()
                    .size(15.0),
            );
            ui.add_space(6.0);
            add_contents(ui)
        })
        .inner
}

#[derive(PartialEq)]
enum Tab {
    Single,
    Compare,
    Portfolio,
    Convergence,
    Smile,
    Calibration,
    Risk,
    FixedIncome,
    Exotics,
}

struct App {
    engine: Engine,
    version: String,

    // Inputs.
    spot: f64,
    strike: f64,
    rate: f64,
    vol: f64,
    maturity: f64,
    dividend: f64,
    model: Model,
    opt_type: OptionType,
    exercise: Exercise,

    // Deterministic Monte Carlo (counter-based, reproducible across threads).
    // When set on the Single tab with the Monte Carlo model, the price is taken
    // from the deterministic path so it's bit-identical run to run.
    mc_deterministic: bool,
    mc_threads: usize,

    // Single-instrument results.
    price: Option<f64>,
    greeks: Greeks,

    // Portfolio state.
    tab: Tab,
    book_strikes: Vec<f64>,
    book_prices: Vec<f64>,
    last_reprice_ms: Option<f64>,
    status: String,
    // Strike-ladder generator inputs (min/max/step) for building the book.
    book_min: f64,
    book_max: f64,
    book_step: f64,

    // Convergence chart state: (sample_size, price) pairs for the chosen model
    // plus the flat Black-Scholes reference line.
    conv_model: Model,
    conv_series: Vec<[f64; 2]>,
    conv_reference: f64,

    // Volatility-smile state (real market data via SQLite).
    store: Option<DataStore>,
    db_status: String,
    smile_tickers: Vec<String>,
    smile_ticker: Option<String>,
    smile_expiries: Vec<String>,
    smile_expiry: Option<String>,
    smile_type: OptionType,
    smile_points: Vec<[f64; 2]>, // (strike, implied vol)
    smile_skipped: usize,
    smile_spot: f64,
    smile_asof: String, // snapshot timestamp shown for context

    // Fixed-income tab state.
    bond_face: f64,
    bond_coupon: f64,
    bond_maturity: f64,
    bond_frequency: i32,
    bond_rate: f64,
    fx_spot: f64,
    fx_strike: f64,
    fx_maturity: f64,
    fx_domestic_rate: f64,
    fx_foreign_rate: f64,

    // Exotics tab state.
    exotic_kind: Exotic,
    exotic_type: OptionType,
    exotic_barrier: f64,
    exotic_barrier_kind: BarrierKind,
    exotic_steps: usize,
    exotic_paths: usize,
    exotic_price: Option<f64>,
    exotic_ms: Option<f64>,
    // Background-pricing channel: Some while a computation is in flight.
    exotic_rx: Option<std::sync::mpsc::Receiver<(f64, f64)>>,

    // Calibration tab: SVI fit to the current Vol-smile points.
    svi_fit: Option<SviFit>,
    svi_forward: f64,
    calib_status: String,

    // Risk tab: stress-scenario table for the Single-tab option.
    stress_rows: Vec<StressRow>,
    stress_vol_shock: f64,
    risk_status: String,
}

impl App {
    fn new() -> Self {
        let engine = Engine::new().expect("failed to create pricing engine");
        let version = bridge::version();
        let mut app = App {
            engine,
            version,
            spot: 100.0,
            strike: 100.0,
            rate: 0.05,
            vol: 0.20,
            maturity: 1.0,
            dividend: 0.0,
            model: Model::BlackScholes,
            opt_type: OptionType::Call,
            exercise: Exercise::European,
            mc_deterministic: false,
            mc_threads: 0, // 0 = all hardware threads
            price: None,
            greeks: Greeks::default(),
            tab: Tab::Single,
            // A small default book spanning strikes around spot.
            book_strikes: (80..=120).step_by(5).map(|k| k as f64).collect(),
            book_prices: Vec::new(),
            last_reprice_ms: None,
            status: String::new(),
            book_min: 80.0,
            book_max: 120.0,
            book_step: 5.0,
            conv_model: Model::Binomial,
            conv_series: Vec::new(),
            conv_reference: 0.0,
            store: None,
            db_status: String::new(),
            smile_tickers: Vec::new(),
            smile_ticker: None,
            smile_expiries: Vec::new(),
            smile_expiry: None,
            smile_type: OptionType::Call,
            smile_points: Vec::new(),
            smile_skipped: 0,
            smile_spot: 0.0,
            smile_asof: String::new(),
            bond_face: 100.0,
            bond_coupon: 0.05,
            bond_maturity: 5.0,
            bond_frequency: 2,
            bond_rate: 0.05,
            fx_spot: 1.25,
            fx_strike: 1.28,
            fx_maturity: 1.0,
            fx_domestic_rate: 0.05,
            fx_foreign_rate: 0.03,
            exotic_kind: Exotic::Asian,
            exotic_type: OptionType::Call,
            exotic_barrier: 130.0,
            exotic_barrier_kind: BarrierKind::UpAndOut,
            exotic_steps: 50,
            exotic_paths: 200_000,
            exotic_price: None,
            exotic_ms: None,
            exotic_rx: None,
            svi_fit: None,
            svi_forward: 0.0,
            calib_status: String::new(),
            stress_rows: Vec::new(),
            stress_vol_shock: 0.10,
            risk_status: String::new(),
        };
        app.recompute();
        app.open_data_store();
        // If a ticker was preloaded, compute its smile up front so the Vol
        // smile tab shows a curve the moment it's opened.
        if app.smile_ticker.is_some() {
            app.recompute_smile();
        }
        app
    }

    /// Look for the market-data cache and load the ticker list if present.
    /// Tries a couple of likely locations relative to where the binary runs.
    fn open_data_store(&mut self) {
        let candidates = [
            PathBuf::from("data/market.db"),
            PathBuf::from("../data/market.db"),
        ];
        for path in candidates {
            match DataStore::open(&path) {
                Ok(store) => match store.tickers() {
                    Ok(t) if !t.is_empty() => {
                        // Pre-select the first ticker + its first expiry so the
                        // Vol smile tab is usable immediately and "Compute
                        // smile" works without the user first picking from the
                        // dropdowns.
                        let first = t[0].clone();
                        let expiries = store.expiries(&first).unwrap_or_default();
                        self.smile_expiry = expiries.first().cloned();
                        self.smile_expiries = expiries;
                        self.smile_ticker = Some(first);
                        self.smile_tickers = t;
                        self.store = Some(store);
                        self.db_status = format!("loaded {}", path.display());
                        return;
                    }
                    Ok(_) => self.db_status = "market.db is empty".into(),
                    Err(e) => self.db_status = e,
                },
                Err(e) => self.db_status = e,
            }
        }
    }

    fn quote(&self) -> Quote {
        Quote {
            spot: self.spot,
            strike: self.strike,
            rate: self.rate,
            vol: self.vol,
            maturity: self.maturity,
            dividend: self.dividend,
        }
    }

    /// Recompute the single-instrument price and Greeks from current inputs.
    fn recompute(&mut self) {
        let q = self.quote();
        // Monte Carlo with the deterministic toggle takes the counter-based
        // path (reproducible across thread counts); everything else uses the
        // standard model dispatch.
        self.price = if self.model == Model::MonteCarlo && self.mc_deterministic {
            self.engine
                .price_mc_deterministic(self.opt_type, q, 1_000_000, self.mc_threads, 0x5EED)
        } else {
            self.engine
                .price(self.model, self.opt_type, self.exercise, q)
        };
        self.greeks = self.engine.greeks(self.opt_type, q);
        self.status = match self.price {
            Some(_) => String::new(),
            None => "Invalid inputs (check vol ≥ 0, maturity > 0, strike > 0).".into(),
        };
    }

    /// Reprice the whole book on the engine's thread pool.
    fn reprice_book(&mut self) {
        let maturities = vec![self.maturity; self.book_strikes.len()];
        let t0 = std::time::Instant::now();
        let result = self.engine.price_portfolio(
            self.model,
            self.opt_type,
            self.exercise,
            self.spot,
            self.rate,
            self.vol,
            self.dividend,
            &self.book_strikes,
            &maturities,
        );
        self.last_reprice_ms = Some(t0.elapsed().as_secs_f64() * 1000.0);
        match result {
            Ok(prices) => {
                self.book_prices = prices;
                self.status = String::new();
            }
            Err(e) => self.status = format!("Portfolio pricing failed: {:?}", e),
        }
    }

    /// Build the convergence series for the selected model against the
    /// Black-Scholes reference price.
    fn recompute_convergence(&mut self) {
        let q = self.quote();
        self.conv_reference = self
            .engine
            .price(Model::BlackScholes, self.opt_type, Exercise::European, q)
            .unwrap_or(f64::NAN);

        // Sample sizes vary by model: binomial steps, MC paths, or FD grid
        // resolution (kept modest since the PDE solve is O(grid^2) per point).
        let sizes: Vec<f64> = match self.conv_model {
            Model::Binomial => (1..=10).map(|k| (1u64 << k) as f64).collect(), // 2..1024
            Model::FiniteDiff => (1..=8).map(|k| (25 * k) as f64).collect(),   // 25..200
            _ => (1..=10).map(|k| (1000u64 << (k - 1)) as f64).collect(),      // 1k..512k
        };
        match self
            .engine
            .convergence(self.conv_model, self.opt_type, q, &sizes)
        {
            Ok(prices) => {
                self.conv_series = sizes.iter().zip(prices).map(|(&x, y)| [x, y]).collect();
                self.status = String::new();
            }
            Err(e) => self.status = format!("Convergence failed: {:?}", e),
        }
    }

    /// Load the selected chain from the DB and compute implied vol per strike,
    /// skipping strikes with no valid IV (below intrinsic, noise, etc.).
    fn recompute_smile(&mut self) {
        self.smile_points.clear();
        self.smile_skipped = 0;

        let (Some(store), Some(ticker), Some(expiry)) =
            (&self.store, &self.smile_ticker, &self.smile_expiry)
        else {
            self.db_status = "select a ticker and expiry".into();
            return;
        };

        let snap = match store.latest_snapshot(ticker) {
            Ok(s) => s,
            Err(e) => {
                self.db_status = e;
                return;
            }
        };
        self.smile_spot = snap.spot;
        self.smile_asof = snap.fetched_at.clone();

        let type_str = match self.smile_type {
            OptionType::Call => "call",
            OptionType::Put => "put",
        };
        let chain = match store.chain(ticker, expiry, type_str) {
            Ok(c) => c,
            Err(e) => {
                self.db_status = e;
                return;
            }
        };

        for q in &chain {
            match self.engine.implied_vol(
                self.smile_type,
                q.market_price,
                snap.spot,
                q.strike,
                snap.rate,
                q.maturity,
                snap.dividend,
            ) {
                Some(iv) => self.smile_points.push([q.strike, iv]),
                None => self.smile_skipped += 1,
            }
        }
        self.db_status = format!(
            "{} strikes -> {} solved, {} skipped",
            chain.len(),
            self.smile_points.len(),
            self.smile_skipped
        );
    }

    /// Fit an SVI smile to the implied-vol points currently on the Vol-smile
    /// tab. Uses the snapshot spot/rate to form the forward.
    fn calibrate(&mut self) {
        self.svi_fit = None;
        if self.smile_points.len() < 5 {
            self.calib_status =
                "Need at least 5 implied-vol points — compute a smile on the Vol smile tab first."
                    .into();
            return;
        }
        // Forward F = S * e^{(r - q) T}; reuse the snapshot spot and a single
        // representative maturity from the chain.
        let (spot, rate) = (self.smile_spot.max(self.spot), self.rate);
        let maturity = self.maturity.max(1e-6);
        let forward = spot * ((rate - self.dividend) * maturity).exp();
        self.svi_forward = forward;

        let strikes: Vec<f64> = self.smile_points.iter().map(|p| p[0]).collect();
        let mats = vec![maturity; strikes.len()];
        let ivs: Vec<f64> = self.smile_points.iter().map(|p| p[1]).collect();

        match self.engine.calibrate_svi(&strikes, &mats, &ivs, forward) {
            Some(fit) => {
                self.calib_status = format!(
                    "fit {} points · RMSE {:.4e} · {} iters",
                    strikes.len(),
                    fit.rmse,
                    fit.iterations
                );
                self.svi_fit = Some(fit);
            }
            None => self.calib_status = "Calibration failed (check inputs).".into(),
        }
    }

    /// Run the stress set for the Single-tab option under the current model.
    fn run_stress(&mut self) {
        let q = self.quote();
        match self.engine.run_stress(
            self.model,
            self.opt_type,
            self.exercise,
            q,
            self.stress_vol_shock,
        ) {
            Some(rows) => {
                self.stress_rows = rows;
                self.risk_status = String::new();
            }
            None => {
                self.stress_rows.clear();
                self.risk_status =
                    "Invalid inputs (check vol ≥ 0, maturity > 0, strike > 0).".into();
            }
        }
    }
}

impl eframe::App for App {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        egui::TopBottomPanel::top("header").show(ctx, |ui| {
            ui.add_space(4.0);
            ui.horizontal(|ui| {
                ui.label(
                    egui::RichText::new("Multi-Asset Pricing Engine")
                        .heading()
                        .color(HEADING),
                );
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    ui.hyperlink_to(
                        "guide",
                        "https://github.com/Loghic/mape/blob/main/docs/user-guide.md",
                    );
                    ui.weak(format!("core v{}  ·", self.version));
                });
            });
            ui.add_space(2.0);
            // Tabs can overflow a narrow window — let the bar scroll sideways.
            egui::ScrollArea::horizontal()
                .auto_shrink([false, true])
                .show(ui, |ui| {
                    ui.horizontal(|ui| {
                        ui.selectable_value(&mut self.tab, Tab::Single, "Single");
                        ui.selectable_value(&mut self.tab, Tab::Compare, "Compare");
                        ui.selectable_value(&mut self.tab, Tab::Portfolio, "Portfolio");
                        ui.selectable_value(&mut self.tab, Tab::Convergence, "Convergence");
                        ui.selectable_value(&mut self.tab, Tab::Smile, "Vol smile");
                        ui.selectable_value(&mut self.tab, Tab::Calibration, "Calibration");
                        ui.selectable_value(&mut self.tab, Tab::Risk, "Risk");
                        ui.selectable_value(&mut self.tab, Tab::FixedIncome, "Fixed income");
                        ui.selectable_value(&mut self.tab, Tab::Exotics, "Exotics");
                    });
                });
            ui.add_space(2.0);
        });

        // Every tab body scrolls vertically, so long tables/plots never clip
        // the bottom of the window (fixes the Portfolio overflow).
        egui::CentralPanel::default().show(ctx, |ui| {
            egui::ScrollArea::vertical()
                .auto_shrink([false, false])
                .show(ui, |ui| match self.tab {
                    Tab::Single => self.single_tab(ui),
                    Tab::Compare => self.compare_tab(ui),
                    Tab::Portfolio => self.portfolio_tab(ui),
                    Tab::Convergence => self.convergence_tab(ui),
                    Tab::Smile => self.smile_tab(ui),
                    Tab::Calibration => self.calibration_tab(ui),
                    Tab::Risk => self.risk_tab(ui),
                    Tab::FixedIncome => self.fixed_income_tab(ui),
                    Tab::Exotics => self.exotics_tab(ui),
                });
        });
    }
}

impl App {
    fn input_controls(&mut self, ui: &mut egui::Ui) -> bool {
        let mut changed = false;
        egui::Grid::new("inputs")
            .num_columns(2)
            .spacing([12.0, 6.0])
            .show(ui, |ui| {
                ui.label("Spot");
                changed |= ui
                    .add(egui::DragValue::new(&mut self.spot).speed(0.5))
                    .changed();
                ui.end_row();
                ui.label("Strike");
                changed |= ui
                    .add(egui::DragValue::new(&mut self.strike).speed(0.5))
                    .changed();
                ui.end_row();
                ui.label("Rate");
                changed |= ui
                    .add(
                        egui::DragValue::new(&mut self.rate)
                            .speed(0.001)
                            .max_decimals(4),
                    )
                    .changed();
                ui.end_row();
                ui.label("Volatility");
                changed |= ui
                    .add(
                        egui::DragValue::new(&mut self.vol)
                            .speed(0.005)
                            .max_decimals(4),
                    )
                    .changed();
                ui.end_row();
                ui.label("Maturity (yrs)");
                changed |= ui
                    .add(
                        egui::DragValue::new(&mut self.maturity)
                            .speed(0.05)
                            .max_decimals(4),
                    )
                    .changed();
                ui.end_row();
                ui.label("Dividend yield");
                changed |= ui
                    .add(
                        egui::DragValue::new(&mut self.dividend)
                            .speed(0.001)
                            .max_decimals(4),
                    )
                    .changed();
                ui.end_row();

                ui.label("Model");
                egui::ComboBox::from_id_source("model")
                    .selected_text(model_label(self.model))
                    .show_ui(ui, |ui| {
                        for m in [
                            Model::BlackScholes,
                            Model::Binomial,
                            Model::MonteCarlo,
                            Model::FiniteDiff,
                        ] {
                            changed |= ui
                                .selectable_value(&mut self.model, m, model_label(m))
                                .changed();
                        }
                    });
                ui.end_row();

                ui.label("Type");
                egui::ComboBox::from_id_source("type")
                    .selected_text(format!("{:?}", self.opt_type))
                    .show_ui(ui, |ui| {
                        for t in [OptionType::Call, OptionType::Put] {
                            changed |= ui
                                .selectable_value(&mut self.opt_type, t, format!("{:?}", t))
                                .changed();
                        }
                    });
                ui.end_row();

                ui.label("Exercise");
                egui::ComboBox::from_id_source("exercise")
                    .selected_text(format!("{:?}", self.exercise))
                    .show_ui(ui, |ui| {
                        for e in [Exercise::European, Exercise::American] {
                            changed |= ui
                                .selectable_value(&mut self.exercise, e, format!("{:?}", e))
                                .changed();
                        }
                    });
                ui.end_row();
            });
        changed
    }

    fn single_tab(&mut self, ui: &mut egui::Ui) {
        let mut changed = section(ui, "Contract & market", |ui| self.input_controls(ui));

        // Deterministic Monte Carlo toggle — only relevant for the MC model.
        if self.model == Model::MonteCarlo {
            changed |= section(ui, "Monte Carlo options", |ui| {
                let mut c = false;
                c |= ui
                    .checkbox(
                        &mut self.mc_deterministic,
                        "Deterministic (reproducible across threads)",
                    )
                    .changed();
                if self.mc_deterministic {
                    ui.horizontal(|ui| {
                        ui.label("Threads (0 = all):");
                        c |= ui
                            .add(egui::DragValue::new(&mut self.mc_threads).clamp_range(0..=64))
                            .changed();
                    });
                    ui.weak(
                        "Counter-based RNG: the price is a pure function of path \
                         count + key, so it's bit-identical at any thread count.",
                    );
                }
                c
            });
        }

        if changed {
            self.recompute();
        }

        match self.price {
            Some(p) => {
                section(ui, "Result", |ui| {
                ui.label(
                    egui::RichText::new(format!("Price  {:.4}", p))
                        .heading()
                        .color(HEADING),
                );
                ui.add_space(6.0);

                // Exact Greeks via automatic differentiation (dual numbers),
                // shown beside the closed-form values as a cross-check.
                let q = self.quote();
                let fmt = |o: Option<f64>| match o {
                    Some(v) => format!("{:.4}", v),
                    None => "—".to_string(),
                };
                let ad_delta = self.engine.ad_greek(AdGreek::Delta, self.opt_type, q);
                let ad_gamma = self.engine.ad_greek(AdGreek::Gamma, self.opt_type, q);
                let ad_vega = self.engine.ad_greek(AdGreek::Vega, self.opt_type, q);
                let ad_rho = self.engine.ad_greek(AdGreek::Rho, self.opt_type, q);

                egui::Grid::new("greeks")
                    .num_columns(3)
                    .spacing([16.0, 4.0])
                    .show(ui, |ui| {
                        ui.strong("Greek");
                        ui.strong("closed-form");
                        ui.strong("AD (exact)");
                        ui.end_row();
                        ui.label("Delta");
                        ui.label(format!("{:.4}", self.greeks.delta));
                        ui.label(fmt(ad_delta));
                        ui.end_row();
                        ui.label("Gamma");
                        ui.label(format!("{:.4}", self.greeks.gamma));
                        ui.label(fmt(ad_gamma));
                        ui.end_row();
                        ui.label("Vega");
                        ui.label(format!("{:.4}", self.greeks.vega));
                        ui.label(fmt(ad_vega));
                        ui.end_row();
                        ui.label("Rho");
                        ui.weak("—");
                        ui.label(fmt(ad_rho));
                        ui.end_row();
                    });
                ui.add_space(4.0);
                ui.weak(
                    "Closed-form Greeks are Black-Scholes, European-style. AD \
                     Greeks are exact (forward-mode dual numbers).",
                );
                });
            }
            None => {
                ui.colored_label(BAD, &self.status);
            }
        }
    }

    fn compare_tab(&mut self, ui: &mut egui::Ui) {
        ui.label(
            "Price the same option with every method at once. The Model selected \
             below is the baseline everything else is compared against — pick \
             Black-Scholes for the exact reference, or any numerical model to see \
             how the others differ from it.",
        );
        ui.add_space(6.0);

        section(ui, "Contract & market", |ui| {
            self.input_controls(ui);
        });

        let q = self.quote();
        let baseline = self.model; // the Model dropdown drives the comparison
        let exercise = self.exercise;
        let opt_type = self.opt_type;
        let engine = &self.engine;

        // Price each model once, timing the call. Returns (price, millis).
        let time_price = |model: Model| -> (Option<f64>, f64) {
            let t0 = std::time::Instant::now();
            let p = engine.price(model, opt_type, exercise, q);
            (p, t0.elapsed().as_secs_f64() * 1000.0)
        };

        let models = [
            Model::BlackScholes,
            Model::Binomial,
            Model::MonteCarlo,
            Model::FiniteDiff,
        ];
        let rows: Vec<(Model, Option<f64>, f64)> = models
            .iter()
            .map(|&m| {
                let (p, ms) = time_price(m);
                (m, p, ms)
            })
            .collect();
        let base_price = rows
            .iter()
            .find(|(m, _, _)| *m == baseline)
            .and_then(|(_, p, _)| *p);

        section(ui, "Prices by method", |ui| {
            ui.label(egui::RichText::new(format!("Baseline: {}", model_label(baseline))).strong());
            if exercise == Exercise::American {
                ui.add_space(2.0);
                ui.weak(
                    "American exercise: only the binomial and finite-difference \
                     models honour early exercise; Black-Scholes and Monte Carlo \
                     price the European value here.",
                );
            }
            ui.add_space(6.0);
            egui::Grid::new("compare_table")
                .num_columns(3)
                .striped(true)
                .min_col_width(150.0)
                .show(ui, |ui| {
                    ui.strong("Method");
                    ui.strong("Price");
                    ui.strong("Δ vs baseline");
                    ui.end_row();
                    for (m, p, _ms) in &rows {
                        // Highlight the baseline row.
                        if *m == baseline {
                            ui.label(
                                egui::RichText::new(model_label(*m)).color(ACCENT_TEXT).strong(),
                            );
                        } else {
                            ui.label(model_label(*m));
                        }
                        match p {
                            Some(v) => ui.monospace(format!("{:.4}", v)),
                            None => ui.weak("—"),
                        };
                        if *m == baseline {
                            ui.weak("baseline");
                        } else {
                            match (p, base_price) {
                                (Some(v), Some(b)) => {
                                    let d = v - b;
                                    let color = if d.abs() < 1e-2 { GOOD } else { ACCENT_TEXT };
                                    ui.monospace(
                                        egui::RichText::new(format!("{:+.4}", d)).color(color),
                                    );
                                }
                                _ => {
                                    ui.weak("—");
                                }
                            };
                        }
                        ui.end_row();
                    }
                });
        });

        section(ui, "Timing", |ui| {
            egui::Grid::new("compare_timing")
                .num_columns(2)
                .spacing([24.0, 4.0])
                .show(ui, |ui| {
                    for (m, _p, ms) in &rows {
                        ui.label(model_label(*m));
                        ui.monospace(format!("{:.3} ms", ms));
                        ui.end_row();
                    }
                });
            ui.add_space(2.0);
            ui.weak(
                "Wall-clock per single call (binomial 512 steps, Monte Carlo \
                 200k paths, FD 400×400 grid). Indicative, not a benchmark — the \
                 bench/ harness measures properly.",
            );
        });

        // Scatter of each model's price, with a reference line at the baseline.
        let pts: Vec<[f64; 2]> = rows
            .iter()
            .enumerate()
            .filter_map(|(i, (_m, p, _))| p.map(|v| [i as f64, v]))
            .collect();
        if !pts.is_empty() {
            Plot::new("compare_plot")
                .x_axis_label("method (0=BS, 1=Binomial, 2=MC, 3=FD)")
                .y_axis_label("price")
                .height(220.0)
                .show(ui, |plot_ui| {
                    plot_ui.points(
                        Points::new(PlotPoints::from(pts))
                            .radius(5.0_f32)
                            .color(ACCENT)
                            .name("price"),
                    );
                    if let Some(b) = base_price {
                        plot_ui.line(
                            Line::new(PlotPoints::from(vec![[0.0, b], [3.0, b]]))
                                .color(ACCENT_TEXT)
                                .width(2.0_f32)
                                .name(model_label(baseline)),
                        );
                    }
                });
        }
    }

    /// Rebuild the book as a strike ladder from min..=max stepping by step,
    /// clamped to a sane count so a tiny step can't spawn a runaway list.
    fn rebuild_book(&mut self) {
        self.book_strikes.clear();
        self.book_prices.clear();
        let (lo, hi, step) = (self.book_min, self.book_max, self.book_step.max(0.01));
        if hi < lo {
            return;
        }
        let mut k = lo;
        while k <= hi + 1e-9 && self.book_strikes.len() < 500 {
            self.book_strikes.push((k * 100.0).round() / 100.0);
            k += step;
        }
    }

    fn portfolio_tab(&mut self, ui: &mut egui::Ui) {
        ui.label(
            "A \"book\" is a collection of option positions valued together. This \
             tab prices many strikes of the same underlying at once — the shared \
             market (spot, rate, vol, dividend) and type/exercise come from the \
             controls below, and each row is one option at its own strike, same \
             maturity. It exists to exercise the engine's thread pool: the whole \
             book is priced in parallel, and the timing shows how that scales \
             versus pricing one option at a time.",
        );
        ui.add_space(6.0);

        section(ui, "Market & contract", |ui| {
            self.input_controls(ui);
        });

        section(ui, "Build the book", |ui| {
            ui.label("Generate a strike ladder, then edit individual rows below.");
            ui.add_space(4.0);
            egui::Grid::new("book_gen")
                .num_columns(2)
                .spacing([12.0, 6.0])
                .show(ui, |ui| {
                    ui.label("Min strike");
                    ui.add(egui::DragValue::new(&mut self.book_min).speed(1.0));
                    ui.end_row();
                    ui.label("Max strike");
                    ui.add(egui::DragValue::new(&mut self.book_max).speed(1.0));
                    ui.end_row();
                    ui.label("Step");
                    ui.add(
                        egui::DragValue::new(&mut self.book_step)
                            .speed(0.5)
                            .clamp_range(0.01..=1000.0),
                    );
                    ui.end_row();
                });
            ui.add_space(4.0);
            ui.horizontal(|ui| {
                if ui.button(egui::RichText::new("Generate ladder").strong()).clicked() {
                    self.rebuild_book();
                }
                if ui.button("Add row").clicked() {
                    let next = self.book_strikes.last().copied().unwrap_or(100.0) + self.book_step;
                    self.book_strikes.push(next);
                    self.book_prices.clear();
                }
                if ui.button("Clear").clicked() {
                    self.book_strikes.clear();
                    self.book_prices.clear();
                }
            });
        });

        section(ui, "Book", |ui| {
            ui.horizontal(|ui| {
                if ui.button(egui::RichText::new("Reprice all").strong()).clicked() {
                    self.reprice_book();
                }
                if let Some(ms) = self.last_reprice_ms {
                    ui.label(
                        egui::RichText::new(format!(
                            "{} instruments · {:.2} ms (threaded)",
                            self.book_strikes.len(),
                            ms
                        ))
                        .color(ACCENT_TEXT),
                    );
                }
            });
            ui.add_space(6.0);

            if !self.status.is_empty() {
                ui.colored_label(BAD, &self.status);
            }
            if self.book_strikes.is_empty() {
                ui.weak("Book is empty — generate a ladder or add a row above.");
                return;
            }

            // Editable rows: strike DragValue + price + a delete button. Deletes
            // are deferred to after the loop so we don't mutate while iterating.
            // Rendered inline (no nested scroll) so the whole tab scrolls as one.
            let mut delete: Option<usize> = None;
            egui::Grid::new("book")
                .num_columns(3)
                .striped(true)
                .min_col_width(110.0)
                .show(ui, |ui| {
                    ui.strong("Strike");
                    ui.strong("Price");
                    ui.strong("");
                    ui.end_row();
                    for i in 0..self.book_strikes.len() {
                        ui.add(egui::DragValue::new(&mut self.book_strikes[i]).speed(0.5));
                        match self.book_prices.get(i) {
                            Some(p) => ui.monospace(format!("{:.4}", p)),
                            None => ui.weak("—"),
                        };
                        if ui.small_button("✕").clicked() {
                            delete = Some(i);
                        }
                        ui.end_row();
                    }
                });
            if let Some(i) = delete {
                self.book_strikes.remove(i);
                self.book_prices.clear(); // prices no longer line up; reprice
            }
        });
    }

    fn convergence_tab(&mut self, ui: &mut egui::Ui) {
        ui.label(
            "Numerical models converge to the Black-Scholes closed form as \
                  resolution increases (binomial: steps; Monte Carlo: paths).",
        );
        ui.horizontal(|ui| {
            ui.label("Model:");
            ui.selectable_value(&mut self.conv_model, Model::Binomial, "Binomial");
            ui.selectable_value(&mut self.conv_model, Model::MonteCarlo, "Monte Carlo");
            ui.selectable_value(&mut self.conv_model, Model::FiniteDiff, "Finite diff");
            if ui.button(egui::RichText::new("Compute").strong()).clicked() {
                self.recompute_convergence();
            }
        });
        ui.separator();

        if !self.status.is_empty() {
            ui.colored_label(BAD, &self.status);
        }
        if self.conv_series.is_empty() {
            ui.weak("Press Compute to plot the convergence series.");
            return;
        }

        let reference = self.conv_reference;
        let model_points: PlotPoints = self.conv_series.iter().map(|p| [p[0], p[1]]).collect();
        // Flat reference line spanning the x-range of the series.
        let x_min = self.conv_series.first().map(|p| p[0]).unwrap_or(0.0);
        let x_max = self.conv_series.last().map(|p| p[0]).unwrap_or(1.0);
        let ref_points: PlotPoints = vec![[x_min, reference], [x_max, reference]].into();

        Plot::new("convergence_plot")
            .legend(Legend::default())
            .x_axis_label("sample size")
            .y_axis_label("price")
            .height(360.0)
            .show(ui, |plot_ui| {
                plot_ui.line(Line::new(model_points).name(format!("{:?}", self.conv_model)));
                plot_ui.line(Line::new(ref_points).name("Black-Scholes"));
            });

        ui.weak(format!("Black-Scholes reference: {:.4}", reference));
    }

    fn smile_tab(&mut self, ui: &mut egui::Ui) {
        ui.label(
            "Implied volatility per strike, inverted from real market \
                  option prices. Each point solves Black-Scholes backwards.",
        );

        if self.store.is_none() {
            ui.add_space(8.0);
            ui.colored_label(egui::Color32::LIGHT_YELLOW, &self.db_status);
            ui.add_space(4.0);
            ui.label("To populate market data:");
            ui.monospace("pip install yfinance");
            ui.monospace("python scripts/fetch_data.py AAPL MSFT");
            ui.weak(
                "Then restart the app (run it from the repo root so it \
                     finds data/market.db).",
            );
            return;
        }

        // Ticker / expiry / type selectors. Changing any triggers a recompute.
        let mut dirty = false;
        ui.horizontal(|ui| {
            ui.label("Ticker:");
            let current = self.smile_ticker.clone().unwrap_or_else(|| "—".into());
            egui::ComboBox::from_id_source("smile_ticker")
                .selected_text(current)
                .show_ui(ui, |ui| {
                    for t in &self.smile_tickers {
                        if ui
                            .selectable_label(self.smile_ticker.as_deref() == Some(t), t)
                            .clicked()
                        {
                            self.smile_ticker = Some(t.clone());
                            // Refresh expiries for the newly chosen ticker.
                            if let Some(store) = &self.store {
                                self.smile_expiries = store.expiries(t).unwrap_or_default();
                                self.smile_expiry = self.smile_expiries.first().cloned();
                            }
                            dirty = true;
                        }
                    }
                });

            ui.label("Expiry:");
            let exp = self.smile_expiry.clone().unwrap_or_else(|| "—".into());
            egui::ComboBox::from_id_source("smile_expiry")
                .selected_text(exp)
                .show_ui(ui, |ui| {
                    for e in &self.smile_expiries {
                        if ui
                            .selectable_label(self.smile_expiry.as_deref() == Some(e), e)
                            .clicked()
                        {
                            self.smile_expiry = Some(e.clone());
                            dirty = true;
                        }
                    }
                });

            dirty |= ui
                .selectable_value(&mut self.smile_type, OptionType::Call, "Call")
                .clicked();
            dirty |= ui
                .selectable_value(&mut self.smile_type, OptionType::Put, "Put")
                .clicked();
        });
        // The smile recomputes automatically whenever the ticker, expiry, or
        // option type changes — no explicit "compute" button needed.

        if dirty {
            self.recompute_smile();
        }

        ui.separator();
        if !self.db_status.is_empty() {
            ui.weak(&self.db_status);
        }
        if self.smile_points.is_empty() {
            ui.weak("No implied vols to plot — pick a ticker and expiry above.");
            return;
        }

        let points: PlotPoints = self.smile_points.iter().map(|p| [p[0], p[1]]).collect();
        let line_points: PlotPoints = self.smile_points.iter().map(|p| [p[0], p[1]]).collect();
        let spot = self.smile_spot;

        Plot::new("smile_plot")
            .legend(Legend::default())
            .x_axis_label("strike")
            .y_axis_label("implied vol")
            .height(360.0)
            .show(ui, |plot_ui| {
                plot_ui.line(Line::new(line_points).name("implied vol"));
                plot_ui.points(Points::new(points).radius(3.0_f32).name("strikes"));
                // Vertical marker at spot (ATM) helps read the smile.
                if spot > 0.0 {
                    let y0 = self
                        .smile_points
                        .iter()
                        .map(|p| p[1])
                        .fold(f64::INFINITY, f64::min);
                    let y1 = self
                        .smile_points
                        .iter()
                        .map(|p| p[1])
                        .fold(f64::NEG_INFINITY, f64::max);
                    plot_ui.line(
                        Line::new(PlotPoints::from(vec![[spot, y0], [spot, y1]]))
                            .name("spot (ATM)"),
                    );
                }
            });

        ui.weak(format!("spot {:.2}  ·  as of {}", spot, self.smile_asof));
    }

    fn calibration_tab(&mut self, ui: &mut egui::Ui) {
        ui.label(
            "Fit a Gatheral SVI smile to the implied-vol points from the Vol \
             smile tab. The fitted curve is the smooth, arbitrage-aware surface \
             the engine prices off.",
        );
        ui.add_space(6.0);

        section(ui, "Fit", |ui| {
            ui.horizontal(|ui| {
                if ui.button(egui::RichText::new("Calibrate SVI").strong()).clicked() {
                    self.calibrate();
                }
                ui.weak(format!(
                    "{} smile points available",
                    self.smile_points.len()
                ));
            });
            if !self.calib_status.is_empty() {
                ui.add_space(4.0);
                ui.label(egui::RichText::new(&self.calib_status).color(ACCENT_TEXT));
            }
        });

        let Some(fit) = self.svi_fit else {
            section(ui, "Parameters", |ui| {
                ui.weak(
                    "No fit yet. Open the Vol smile tab, compute a smile, then \
                     press Calibrate SVI here.",
                );
            });
            return;
        };

        section(ui, "SVI parameters", |ui| {
            egui::Grid::new("svi_params")
                .num_columns(2)
                .spacing([24.0, 4.0])
                .show(ui, |ui| {
                    let names = [
                        "a (level)",
                        "b (slope)",
                        "ρ (skew)",
                        "m (shift)",
                        "σ (curvature)",
                    ];
                    for (n, v) in names.iter().zip(fit.params.iter()) {
                        ui.label(*n);
                        ui.monospace(format!("{:+.5}", v));
                        ui.end_row();
                    }
                    ui.label("RMSE");
                    ui.monospace(format!("{:.4e}", fit.rmse));
                    ui.end_row();
                });
        });

        // Plot: raw implied-vol points + the fitted SVI curve sampled densely.
        let forward = self.svi_forward;
        let maturity = self.maturity.max(1e-6);
        let raw: PlotPoints = self.smile_points.iter().map(|p| [p[0], p[1]]).collect();

        let mut fitted: Vec<[f64; 2]> = Vec::new();
        if !self.smile_points.is_empty() {
            let k_lo = self
                .smile_points
                .iter()
                .map(|p| p[0])
                .fold(f64::INFINITY, f64::min);
            let k_hi = self
                .smile_points
                .iter()
                .map(|p| p[0])
                .fold(f64::NEG_INFINITY, f64::max);
            let n = 80;
            for i in 0..=n {
                let k = k_lo + (k_hi - k_lo) * (i as f64) / (n as f64);
                if let Some(v) = fit.vol(k, forward, maturity) {
                    fitted.push([k, v]);
                }
            }
        }

        Plot::new("svi_plot")
            .legend(Legend::default())
            .x_axis_label("strike")
            .y_axis_label("implied vol")
            .height(340.0)
            .show(ui, |plot_ui| {
                plot_ui.points(
                    Points::new(raw)
                        .radius(3.0_f32)
                        .color(ACCENT_TEXT)
                        .name("market IV"),
                );
                plot_ui.line(
                    Line::new(PlotPoints::from(fitted))
                        .color(ACCENT)
                        .width(2.0_f32)
                        .name("fitted SVI"),
                );
            });
        ui.weak(format!(
            "forward {:.2} · maturity {:.2}y",
            forward, maturity
        ));
    }

    fn risk_tab(&mut self, ui: &mut egui::Ui) {
        ui.label(
            "Reprice the Single-tab option under a set of market stress \
             scenarios, concurrently on the engine's thread pool. P&L is versus \
             the unstressed price.",
        );
        ui.add_space(6.0);

        section(ui, "Scenario set", |ui| {
            ui.horizontal(|ui| {
                ui.label("Model:");
                ui.label(egui::RichText::new(model_label(self.model)).color(ACCENT_TEXT));
                ui.separator();
                ui.label("Vol shock:");
                ui.add(
                    egui::DragValue::new(&mut self.stress_vol_shock)
                        .speed(0.01)
                        .clamp_range(0.0..=1.0)
                        .max_decimals(3),
                );
                if ui.button(egui::RichText::new("Run stress").strong()).clicked() {
                    self.run_stress();
                }
            });
            if !self.risk_status.is_empty() {
                ui.add_space(4.0);
                ui.colored_label(BAD, &self.risk_status);
            }
        });

        if self.stress_rows.is_empty() {
            section(ui, "Results", |ui| {
                ui.weak("Press Run stress to populate the scenario table.");
            });
            return;
        }

        section(ui, "Scenario P&L", |ui| {
            egui::Grid::new("stress_table")
                .num_columns(3)
                .striped(true)
                .min_col_width(110.0)
                .show(ui, |ui| {
                    ui.strong("Scenario");
                    ui.strong("Price");
                    ui.strong("P&L");
                    ui.end_row();
                    for row in &self.stress_rows {
                        ui.label(&row.name);
                        ui.monospace(format!("{:.4}", row.price));
                        let color = if row.pnl >= 0.0 { GOOD } else { BAD };
                        ui.monospace(egui::RichText::new(format!("{:+.4}", row.pnl)).color(color));
                        ui.end_row();
                    }
                });
        });

        // A simple P&L bar-style plot for quick visual scanning.
        let bars: Vec<[f64; 2]> = self
            .stress_rows
            .iter()
            .enumerate()
            .map(|(i, r)| [i as f64, r.pnl])
            .collect();
        Plot::new("stress_plot")
            .x_axis_label("scenario #")
            .y_axis_label("P&L")
            .height(220.0)
            .show(ui, |plot_ui| {
                plot_ui.line(
                    Line::new(PlotPoints::from(bars))
                        .color(ACCENT)
                        .width(2.0_f32)
                        .name("P&L"),
                );
            });
    }

    fn fixed_income_tab(&mut self, ui: &mut egui::Ui) {
        ui.label(
            "Analytic pricing for the non-option instruments: a fixed-coupon \
             bond and an FX forward.",
        );
        ui.add_space(8.0);

        // --- Bond ---------------------------------------------------------
        ui.heading("Fixed-coupon bond");
        egui::Grid::new("bond_inputs")
            .num_columns(2)
            .spacing([12.0, 6.0])
            .show(ui, |ui| {
                ui.label("Face");
                ui.add(egui::DragValue::new(&mut self.bond_face).speed(1.0));
                ui.end_row();
                ui.label("Coupon (annual)");
                ui.add(
                    egui::DragValue::new(&mut self.bond_coupon)
                        .speed(0.001)
                        .max_decimals(4),
                );
                ui.end_row();
                ui.label("Maturity (yrs)");
                ui.add(egui::DragValue::new(&mut self.bond_maturity).speed(0.5));
                ui.end_row();
                ui.label("Coupons / year");
                ui.add(egui::DragValue::new(&mut self.bond_frequency).speed(1));
                ui.end_row();
                ui.label("Rate (cont.)");
                ui.add(
                    egui::DragValue::new(&mut self.bond_rate)
                        .speed(0.001)
                        .max_decimals(4),
                );
                ui.end_row();
            });
        let bond_pv = self.engine.price_bond(
            self.bond_face,
            self.bond_coupon,
            self.bond_maturity,
            self.bond_frequency,
            self.bond_rate,
        );
        match bond_pv {
            Some(pv) => ui.strong(format!("Present value: {:.4}", pv)),
            None => ui.colored_label(BAD, "invalid bond inputs"),
        };

        ui.add_space(12.0);
        ui.separator();

        // --- FX forward ---------------------------------------------------
        ui.heading("FX forward");
        egui::Grid::new("fx_inputs")
            .num_columns(2)
            .spacing([12.0, 6.0])
            .show(ui, |ui| {
                ui.label("Spot (dom/for)");
                ui.add(
                    egui::DragValue::new(&mut self.fx_spot)
                        .speed(0.01)
                        .max_decimals(4),
                );
                ui.end_row();
                ui.label("Strike");
                ui.add(
                    egui::DragValue::new(&mut self.fx_strike)
                        .speed(0.01)
                        .max_decimals(4),
                );
                ui.end_row();
                ui.label("Maturity (yrs)");
                ui.add(egui::DragValue::new(&mut self.fx_maturity).speed(0.25));
                ui.end_row();
                ui.label("Domestic rate");
                ui.add(
                    egui::DragValue::new(&mut self.fx_domestic_rate)
                        .speed(0.001)
                        .max_decimals(4),
                );
                ui.end_row();
                ui.label("Foreign rate");
                ui.add(
                    egui::DragValue::new(&mut self.fx_foreign_rate)
                        .speed(0.001)
                        .max_decimals(4),
                );
                ui.end_row();
            });
        let fx_pv = self.engine.price_fx_forward(
            self.fx_spot,
            self.fx_strike,
            self.fx_maturity,
            self.fx_domestic_rate,
            self.fx_foreign_rate,
        );
        let fair = self.engine.fx_forward_rate(
            self.fx_spot,
            self.fx_maturity,
            self.fx_domestic_rate,
            self.fx_foreign_rate,
        );
        match (fx_pv, fair) {
            (Some(pv), Some(f)) => {
                ui.strong(format!("Present value: {:.6}", pv));
                ui.weak(format!("fair forward rate: {:.6}", f));
            }
            _ => {
                ui.colored_label(BAD, "invalid FX inputs");
            }
        }
    }

    fn exotics_tab(&mut self, ui: &mut egui::Ui) {
        ui.label(
            "Path-dependent options priced by parallel Monte Carlo. Uses spot, \
             rate, vol, dividend and strike from the Single tab.",
        );
        let q = self.quote();

        egui::Grid::new("exotic_inputs")
            .num_columns(2)
            .spacing([12.0, 6.0])
            .show(ui, |ui| {
                ui.label("Exotic");
                egui::ComboBox::from_id_source("exotic_kind")
                    .selected_text(format!("{:?}", self.exotic_kind))
                    .show_ui(ui, |ui| {
                        for e in [Exotic::Asian, Exotic::Barrier, Exotic::Lookback] {
                            ui.selectable_value(&mut self.exotic_kind, e, format!("{:?}", e));
                        }
                    });
                ui.end_row();

                ui.label("Type");
                egui::ComboBox::from_id_source("exotic_type")
                    .selected_text(format!("{:?}", self.exotic_type))
                    .show_ui(ui, |ui| {
                        for t in [OptionType::Call, OptionType::Put] {
                            ui.selectable_value(&mut self.exotic_type, t, format!("{:?}", t));
                        }
                    });
                ui.end_row();

                // Barrier-only inputs.
                if self.exotic_kind == Exotic::Barrier {
                    ui.label("Barrier");
                    ui.add(egui::DragValue::new(&mut self.exotic_barrier).speed(1.0));
                    ui.end_row();
                    ui.label("Barrier kind");
                    egui::ComboBox::from_id_source("barrier_kind")
                        .selected_text(format!("{:?}", self.exotic_barrier_kind))
                        .show_ui(ui, |ui| {
                            for k in [
                                BarrierKind::UpAndOut,
                                BarrierKind::DownAndOut,
                                BarrierKind::UpAndIn,
                                BarrierKind::DownAndIn,
                            ] {
                                ui.selectable_value(
                                    &mut self.exotic_barrier_kind,
                                    k,
                                    format!("{:?}", k),
                                );
                            }
                        });
                    ui.end_row();
                }

                ui.label("Steps / path");
                ui.add(egui::DragValue::new(&mut self.exotic_steps).speed(1));
                ui.end_row();
                ui.label("Paths");
                ui.add(egui::DragValue::new(&mut self.exotic_paths).speed(1000));
                ui.end_row();
            });

        // Drain a finished background computation, if any.
        if let Some(rx) = &self.exotic_rx {
            if let Ok((price, ms)) = rx.try_recv() {
                self.exotic_price = Some(price);
                self.exotic_ms = Some(ms);
                self.exotic_rx = None; // done
            }
        }

        let computing = self.exotic_rx.is_some();
        ui.add_enabled_ui(!computing, |ui| {
            if ui.button("Price exotic").clicked() {
                // Run the Monte Carlo off the UI thread so the window stays
                // responsive; the worker owns its own engine and sends the
                // result back over a channel, then wakes the UI.
                let (tx, rx) = std::sync::mpsc::channel();
                self.exotic_rx = Some(rx);
                let ctx = ui.ctx().clone();
                let (kind, otype, barrier, bkind, steps, paths) = (
                    self.exotic_kind,
                    self.exotic_type,
                    self.exotic_barrier,
                    self.exotic_barrier_kind,
                    self.exotic_steps,
                    self.exotic_paths,
                );
                std::thread::spawn(move || {
                    let engine = match Engine::new() {
                        Some(e) => e,
                        None => return,
                    };
                    let t0 = std::time::Instant::now();
                    let price = engine
                        .price_exotic(kind, otype, q, barrier, bkind, steps, paths)
                        .unwrap_or(f64::NAN);
                    let ms = t0.elapsed().as_secs_f64() * 1000.0;
                    let _ = tx.send((price, ms));
                    ctx.request_repaint(); // wake the UI to pick up the result
                });
            }
        });

        ui.add_space(8.0);
        if computing {
            ui.horizontal(|ui| {
                ui.spinner();
                ui.label("pricing…");
            });
        }
        match self.exotic_price {
            Some(p) => {
                ui.heading(format!("Price: {:.4}", p));
                if let Some(ms) = self.exotic_ms {
                    ui.weak(format!(
                        "{} paths × {} steps in {:.1} ms (threaded)",
                        self.exotic_paths, self.exotic_steps, ms
                    ));
                }
            }
            None => {
                ui.weak("Set parameters and press Price exotic.");
            }
        }
    }
}
