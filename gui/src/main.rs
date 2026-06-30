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
    AdGreek, BarrierKind, Engine, Exercise, Exotic, Greeks, Model, OptionType, Quote,
};
use data::DataStore;
use eframe::egui;
use egui_plot::{Legend, Line, Plot, PlotPoints, Points};
use std::path::PathBuf;

fn main() -> eframe::Result<()> {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default().with_inner_size([720.0, 560.0]),
        ..Default::default()
    };
    eframe::run_native(
        "Multi-Asset Pricing Engine",
        options,
        Box::new(|_cc| Box::new(App::new())),
    )
}

#[derive(PartialEq)]
enum Tab {
    Single,
    Portfolio,
    Convergence,
    Smile,
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

    // Single-instrument results.
    price: Option<f64>,
    greeks: Greeks,

    // Portfolio state.
    tab: Tab,
    book_strikes: Vec<f64>,
    book_prices: Vec<f64>,
    last_reprice_ms: Option<f64>,
    status: String,

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
            price: None,
            greeks: Greeks::default(),
            tab: Tab::Single,
            // A small default book spanning strikes around spot.
            book_strikes: (80..=120).step_by(5).map(|k| k as f64).collect(),
            book_prices: Vec::new(),
            last_reprice_ms: None,
            status: String::new(),
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
        self.price = self
            .engine
            .price(self.model, self.opt_type, self.exercise, q);
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

        // Log-spaced sample sizes: steps for binomial, paths for Monte Carlo.
        let sizes: Vec<f64> = match self.conv_model {
            Model::Binomial => (1..=10).map(|k| (1u64 << k) as f64).collect(), // 2..1024
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
}

impl eframe::App for App {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        egui::TopBottomPanel::top("header").show(ctx, |ui| {
            ui.horizontal(|ui| {
                ui.heading("Multi-Asset Pricing Engine");
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    ui.hyperlink_to(
                        "guide",
                        "https://github.com/Loghic/mape/blob/main/docs/user-guide.md",
                    );
                    ui.label(format!("core v{}  ·", self.version));
                });
            });
            ui.horizontal(|ui| {
                ui.selectable_value(&mut self.tab, Tab::Single, "Single instrument");
                ui.selectable_value(&mut self.tab, Tab::Portfolio, "Portfolio");
                ui.selectable_value(&mut self.tab, Tab::Convergence, "Convergence");
                ui.selectable_value(&mut self.tab, Tab::Smile, "Vol smile");
                ui.selectable_value(&mut self.tab, Tab::FixedIncome, "Fixed income");
                ui.selectable_value(&mut self.tab, Tab::Exotics, "Exotics");
            });
        });

        egui::CentralPanel::default().show(ctx, |ui| match self.tab {
            Tab::Single => self.single_tab(ui),
            Tab::Portfolio => self.portfolio_tab(ui),
            Tab::Convergence => self.convergence_tab(ui),
            Tab::Smile => self.smile_tab(ui),
            Tab::FixedIncome => self.fixed_income_tab(ui),
            Tab::Exotics => self.exotics_tab(ui),
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
                    .selected_text(format!("{:?}", self.model))
                    .show_ui(ui, |ui| {
                        for m in [Model::BlackScholes, Model::Binomial, Model::MonteCarlo] {
                            changed |= ui
                                .selectable_value(&mut self.model, m, format!("{:?}", m))
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
        if self.input_controls(ui) {
            self.recompute();
        }
        ui.separator();

        match self.price {
            Some(p) => {
                ui.heading(format!("Price: {:.4}", p));
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
            }
            None => {
                ui.colored_label(egui::Color32::LIGHT_RED, &self.status);
            }
        }
    }

    fn portfolio_tab(&mut self, ui: &mut egui::Ui) {
        ui.label(
            "Shared market: spot, rate, vol, dividend from the Single tab. \
                  Each row is a call/put at its own strike, same maturity.",
        );
        self.input_controls(ui);
        ui.separator();

        ui.horizontal(|ui| {
            if ui.button("Reprice all").clicked() {
                self.reprice_book();
            }
            if ui.button("Add strike").clicked() {
                let next = self.book_strikes.last().copied().unwrap_or(100.0) + 5.0;
                self.book_strikes.push(next);
            }
            if let Some(ms) = self.last_reprice_ms {
                ui.weak(format!(
                    "{} instruments in {:.2} ms (threaded)",
                    self.book_strikes.len(),
                    ms
                ));
            }
        });
        ui.add_space(6.0);

        if !self.status.is_empty() {
            ui.colored_label(egui::Color32::LIGHT_RED, &self.status);
        }

        egui::Grid::new("book")
            .num_columns(2)
            .striped(true)
            .show(ui, |ui| {
                ui.strong("Strike");
                ui.strong("Price");
                ui.end_row();
                for (i, k) in self.book_strikes.iter().enumerate() {
                    ui.monospace(format!("{:.2}", k));
                    match self.book_prices.get(i) {
                        Some(p) => ui.monospace(format!("{:.4}", p)),
                        None => ui.weak("—"),
                    };
                    ui.end_row();
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
            if ui.button("Compute").clicked() {
                self.recompute_convergence();
            }
        });
        ui.separator();

        if !self.status.is_empty() {
            ui.colored_label(egui::Color32::LIGHT_RED, &self.status);
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
            None => ui.colored_label(egui::Color32::LIGHT_RED, "invalid bond inputs"),
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
                ui.colored_label(egui::Color32::LIGHT_RED, "invalid FX inputs");
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
