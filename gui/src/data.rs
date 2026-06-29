//! Read-only access to the market-data SQLite cache (`data/market.db`).
//!
//! Populated by `scripts/fetch_data.py`. This module is the Rust side of the
//! data contract; it only reads. If the DB is missing the GUI degrades to
//! manual input, so every entry point returns a `Result` the UI can show.

use rusqlite::Connection;
use std::path::Path;

/// One quoted option contract loaded from the cache.
#[derive(Clone, Debug)]
pub struct OptionQuote {
    pub strike: f64,
    pub maturity: f64,
    pub market_price: f64,
}

/// The market context for a ticker's latest snapshot.
#[derive(Clone, Debug)]
pub struct Snapshot {
    pub spot: f64,
    pub rate: f64,
    pub dividend: f64,
    pub fetched_at: String,
}

/// Handle to the read-only data cache.
pub struct DataStore {
    conn: Connection,
}

impl DataStore {
    /// Open the cache read-only. Returns an error (not a panic) if the file is
    /// absent, so the GUI can fall back to manual entry.
    pub fn open(path: impl AsRef<Path>) -> Result<Self, String> {
        let path = path.as_ref();
        if !path.exists() {
            return Err(format!(
                "no market data at {} — run scripts/fetch_data.py first",
                path.display()
            ));
        }
        Connection::open(path)
            .map(|conn| DataStore { conn })
            .map_err(|e| format!("failed to open {}: {e}", path.display()))
    }

    /// All tickers that have at least one snapshot, alphabetically.
    pub fn tickers(&self) -> Result<Vec<String>, String> {
        let mut stmt = self
            .conn
            .prepare("SELECT DISTINCT ticker FROM snapshots ORDER BY ticker")
            .map_err(|e| e.to_string())?;
        let rows = stmt
            .query_map([], |r| r.get::<_, String>(0))
            .map_err(|e| e.to_string())?;
        rows.collect::<Result<_, _>>().map_err(|e| e.to_string())
    }

    /// Latest snapshot (market context) for a ticker.
    pub fn latest_snapshot(&self, ticker: &str) -> Result<Snapshot, String> {
        self.conn
            .query_row(
                "SELECT spot, rate, dividend, fetched_at \
                 FROM latest_snapshots WHERE ticker = ?1",
                [ticker],
                |r| {
                    Ok(Snapshot {
                        spot: r.get(0)?,
                        rate: r.get(1)?,
                        dividend: r.get(2)?,
                        fetched_at: r.get(3)?,
                    })
                },
            )
            .map_err(|e| format!("no snapshot for {ticker}: {e}"))
    }

    /// Distinct expiries available for a ticker's latest snapshot.
    pub fn expiries(&self, ticker: &str) -> Result<Vec<String>, String> {
        let mut stmt = self
            .conn
            .prepare(
                "SELECT DISTINCT q.expiry \
                 FROM option_quotes q \
                 JOIN latest_snapshots s ON q.snapshot_id = s.id \
                 WHERE s.ticker = ?1 ORDER BY q.expiry",
            )
            .map_err(|e| e.to_string())?;
        let rows = stmt
            .query_map([ticker], |r| r.get::<_, String>(0))
            .map_err(|e| e.to_string())?;
        rows.collect::<Result<_, _>>().map_err(|e| e.to_string())
    }

    /// Load the option chain for a ticker + expiry + type ("call"/"put"),
    /// ordered by strike. Pulls from the latest snapshot.
    pub fn chain(
        &self,
        ticker: &str,
        expiry: &str,
        option_type: &str,
    ) -> Result<Vec<OptionQuote>, String> {
        let mut stmt = self
            .conn
            .prepare(
                "SELECT q.strike, q.maturity_yrs, q.market_price \
                 FROM option_quotes q \
                 JOIN latest_snapshots s ON q.snapshot_id = s.id \
                 WHERE s.ticker = ?1 AND q.expiry = ?2 AND q.option_type = ?3 \
                 ORDER BY q.strike",
            )
            .map_err(|e| e.to_string())?;
        let rows = stmt
            .query_map([ticker, expiry, option_type], |r| {
                Ok(OptionQuote {
                    strike: r.get(0)?,
                    maturity: r.get(1)?,
                    market_price: r.get(2)?,
                })
            })
            .map_err(|e| e.to_string())?;
        rows.collect::<Result<_, _>>().map_err(|e| e.to_string())
    }
}
