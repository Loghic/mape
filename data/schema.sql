-- SQLite schema for the market-data cache.
--
-- This file is the contract between the Python fetcher (writer) and the Rust
-- GUI (reader). The C++ engine never sees it — data lives strictly outside the
-- pricing core, consistent with the project's separation of concerns.
--
-- Design: time-stamped history (not just a latest snapshot). Each fetch appends
-- rows tagged with a UTC `fetched_at`, so the same ticker can accumulate
-- snapshots over time. A "latest" view exposes the most recent fetch per
-- ticker for the common case (live smile), while history enables later work
-- (IV-over-time, P&L) without a schema change.

PRAGMA journal_mode = WAL;       -- concurrent reader (GUI) + writer (fetcher)
PRAGMA foreign_keys = ON;

-- One row per (ticker, fetch). Holds the underlying market state used to price
-- that ticker's options: spot, the risk-free proxy, and dividend yield.
CREATE TABLE IF NOT EXISTS snapshots (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    ticker      TEXT    NOT NULL,
    fetched_at  TEXT    NOT NULL,         -- ISO-8601 UTC, e.g. 2026-06-29T14:00:00Z
    spot        REAL    NOT NULL,
    rate        REAL    NOT NULL,         -- continuously-compounded risk-free
    dividend    REAL    NOT NULL DEFAULT 0.0,
    UNIQUE (ticker, fetched_at)
);

CREATE INDEX IF NOT EXISTS idx_snapshots_ticker_time
    ON snapshots (ticker, fetched_at DESC);

-- One row per quoted option contract within a snapshot. `market_price` is the
-- mid (or last) price the implied-vol solver will invert.
CREATE TABLE IF NOT EXISTS option_quotes (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    snapshot_id   INTEGER NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE,
    expiry        TEXT    NOT NULL,        -- ISO date, e.g. 2026-09-18
    maturity_yrs  REAL    NOT NULL,        -- year fraction from snapshot to expiry
    strike        REAL    NOT NULL,
    option_type   TEXT    NOT NULL CHECK (option_type IN ('call', 'put')),
    market_price  REAL    NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_quotes_snapshot
    ON option_quotes (snapshot_id, expiry, option_type, strike);

-- Convenience view: the most recent snapshot per ticker.
CREATE VIEW IF NOT EXISTS latest_snapshots AS
SELECT s.*
FROM snapshots s
JOIN (
    SELECT ticker, MAX(fetched_at) AS max_t
    FROM snapshots
    GROUP BY ticker
) m ON s.ticker = m.ticker AND s.fetched_at = m.max_t;
