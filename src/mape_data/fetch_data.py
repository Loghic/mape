#!/usr/bin/env python3
"""Fetch market data with yfinance and write it into the SQLite cache.

This is the only Python in the project and the only thing that touches the
network. It knows nothing about pricing — it just populates `data/market.db`
against `data/schema.sql`. The Rust GUI reads that DB; the C++ engine never
sees it.

Usage (with uv — recommended, deps are locked):
    uv sync
    uv run fetch-data AAPL MSFT --max-expiries 3
    uv run fetch-data AAPL --db data/market.db --rate 0.043

Usage (plain Python):
    pip install yfinance
    python scripts/fetch_data.py AAPL MSFT --max-expiries 3

yfinance has no SLA and its schema drifts; everything here is best-effort and
defensive. A failure on one ticker or expiry is logged and skipped, not fatal.
"""

from __future__ import annotations

import argparse
import datetime as dt
import math
import os
import sqlite3
import sys

# Paths are resolved relative to the current working directory, not this
# file's location. The script is meant to run from the repo root (e.g.
# `uv run fetch-data ...`), and once installed into a venv `__file__` points
# into site-packages, which is the wrong anchor for the repo's data/ folder.
DEFAULT_DB = os.path.join("data", "market.db")
DEFAULT_SCHEMA = os.path.join("data", "schema.sql")

# Fallback risk-free rate (continuously compounded) if not supplied and we
# can't derive one. ~4.3% is a reasonable short-Treasury proxy; override with
# --rate. A live curve is a future enhancement.
DEFAULT_RATE = 0.043

# Embedded copy of the schema so the fetcher works even when run from a
# directory without data/schema.sql (e.g. installed as a standalone tool).
# Kept in sync with data/schema.sql.
EMBEDDED_SCHEMA = """
PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;
CREATE TABLE IF NOT EXISTS snapshots (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ticker TEXT NOT NULL,
    fetched_at TEXT NOT NULL,
    spot REAL NOT NULL,
    rate REAL NOT NULL,
    dividend REAL NOT NULL DEFAULT 0.0,
    UNIQUE (ticker, fetched_at)
);
CREATE INDEX IF NOT EXISTS idx_snapshots_ticker_time
    ON snapshots (ticker, fetched_at DESC);
CREATE TABLE IF NOT EXISTS option_quotes (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    snapshot_id INTEGER NOT NULL REFERENCES snapshots(id) ON DELETE CASCADE,
    expiry TEXT NOT NULL,
    maturity_yrs REAL NOT NULL,
    strike REAL NOT NULL,
    option_type TEXT NOT NULL CHECK (option_type IN ('call', 'put')),
    market_price REAL NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_quotes_snapshot
    ON option_quotes (snapshot_id, expiry, option_type, strike);
CREATE VIEW IF NOT EXISTS latest_snapshots AS
SELECT s.* FROM snapshots s
JOIN (SELECT ticker, MAX(fetched_at) AS max_t FROM snapshots GROUP BY ticker) m
  ON s.ticker = m.ticker AND s.fetched_at = m.max_t;
"""


def init_db(path: str, schema_path: str) -> sqlite3.Connection:
    """Open (creating if needed) the DB and apply the schema.

    Prefers an on-disk schema file (so edits to data/schema.sql take effect),
    falling back to the embedded copy if the file isn't found.
    """
    parent = os.path.dirname(path)
    if parent:
        os.makedirs(parent, exist_ok=True)
    conn = sqlite3.connect(path)
    if schema_path and os.path.exists(schema_path):
        with open(schema_path) as f:
            conn.executescript(f.read())
    else:
        conn.executescript(EMBEDDED_SCHEMA)
    conn.commit()
    return conn


def year_fraction(from_date: dt.date, to_date: dt.date) -> float:
    """ACT/365 year fraction between two dates (never negative)."""
    days = (to_date - from_date).days
    return max(days, 0) / 365.0


def fetch_ticker(conn: sqlite3.Connection, ticker: str, rate: float, max_expiries: int) -> None:
    """Fetch one ticker's spot + option chain and store a snapshot."""
    import yfinance as yf  # imported lazily so --help works without it

    tk = yf.Ticker(ticker)

    # --- Spot + dividend yield -----------------------------------------
    spot = None
    dividend = 0.0
    try:
        info = tk.fast_info  # faster, fewer surprises than .info
        spot = float(info.get("last_price") or info.get("lastPrice"))
    except Exception:
        pass
    if spot is None:
        # Fall back to the latest close from a 1-day history.
        try:
            hist = tk.history(period="1d")
            if not hist.empty:
                spot = float(hist["Close"].iloc[-1])
        except Exception:
            pass
    if spot is None or not math.isfinite(spot) or spot <= 0:
        print(f"  [skip] {ticker}: could not determine spot", file=sys.stderr)
        return
    try:
        dy = tk.info.get("dividendYield")
        if dy:
            dividend = float(dy)
            if dividend > 1.0:  # yfinance sometimes reports percent, not fraction
                dividend /= 100.0
    except Exception:
        dividend = 0.0

    now = dt.datetime.now(dt.timezone.utc)
    fetched_at = now.strftime("%Y-%m-%dT%H:%M:%SZ")
    today = now.date()

    cur = conn.cursor()
    cur.execute(
        "INSERT OR REPLACE INTO snapshots (ticker, fetched_at, spot, rate, dividend) "
        "VALUES (?, ?, ?, ?, ?)",
        (ticker, fetched_at, spot, rate, dividend),
    )
    snapshot_id = cur.lastrowid
    print(f"  {ticker}: spot={spot:.2f} div={dividend:.4f} rate={rate:.4f}")

    # --- Option chain ---------------------------------------------------
    try:
        expiries = list(tk.options or [])
    except Exception as e:
        print(f"  [warn] {ticker}: no option expiries ({e})", file=sys.stderr)
        conn.commit()
        return

    n_quotes = 0
    for expiry in expiries[:max_expiries]:
        try:
            chain = tk.option_chain(expiry)
        except Exception as e:
            print(f"  [warn] {ticker} {expiry}: chain fetch failed ({e})", file=sys.stderr)
            continue
        exp_date = dt.date.fromisoformat(expiry)
        t_yrs = year_fraction(today, exp_date)
        if t_yrs <= 0:
            continue

        for df, otype in ((chain.calls, "call"), (chain.puts, "put")):
            for _, row in df.iterrows():
                try:
                    strike = float(row["strike"])
                    bid = float(row.get("bid") or 0.0)
                    ask = float(row.get("ask") or 0.0)
                    last = float(row.get("lastPrice") or 0.0)
                    # Prefer the bid/ask mid; fall back to last traded.
                    price = (bid + ask) / 2.0 if (bid > 0 and ask > 0) else last
                    if not math.isfinite(price) or price <= 0 or strike <= 0:
                        continue
                    cur.execute(
                        "INSERT INTO option_quotes "
                        "(snapshot_id, expiry, maturity_yrs, strike, option_type, market_price) "
                        "VALUES (?, ?, ?, ?, ?, ?)",
                        (snapshot_id, expiry, t_yrs, strike, otype, price),
                    )
                    n_quotes += 1
                except Exception:
                    continue
    conn.commit()
    print(f"    stored {n_quotes} option quotes across {min(len(expiries), max_expiries)} expiries")


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    ap.add_argument("tickers", nargs="+", help="ticker symbols, e.g. AAPL MSFT")
    ap.add_argument(
        "--db", default=DEFAULT_DB, help="SQLite path (default: data/market.db, relative to CWD)"
    )
    ap.add_argument(
        "--schema",
        default=DEFAULT_SCHEMA,
        help="schema.sql path (default: data/schema.sql; an embedded copy is used if not found)",
    )
    ap.add_argument(
        "--rate",
        type=float,
        default=DEFAULT_RATE,
        help=f"risk-free rate, continuously compounded (default {DEFAULT_RATE})",
    )
    ap.add_argument(
        "--max-expiries",
        type=int,
        default=3,
        help="number of nearest expiries to fetch per ticker (default 3)",
    )
    args = ap.parse_args()

    conn = init_db(args.db, args.schema)
    print(f">> writing to {os.path.abspath(args.db)}")
    for ticker in args.tickers:
        try:
            fetch_ticker(conn, ticker.upper(), args.rate, args.max_expiries)
        except Exception as e:
            print(f"  [error] {ticker}: {e}", file=sys.stderr)
    conn.close()
    print(">> done")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
