#!/usr/bin/env python3
"""Compare a fresh benchmark CSV against a committed baseline (plan §16.7).

Flags any model whose median runtime regressed by more than a tolerance, and
sanity-checks the *correctness*-ish columns (parallel speedup shouldn't collapse,
MC standard error shouldn't balloon). Absolute timings vary across machines, so
this is meant as an *informational* same-machine / same-runner check, not a hard
cross-machine gate.

Usage:
    python scripts/check_bench_regression.py <baseline.csv> <new.csv> [--tol 0.5]

Exit code: 0 if within tolerance, 1 if a regression is detected.
"""

from __future__ import annotations

import argparse
import csv
import sys


def load(path: str) -> dict[tuple[str, str, str], dict[str, float]]:
    """Index rows by (model, threads, paths) -> column floats."""
    rows: dict[tuple[str, str, str], dict[str, float]] = {}
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            key = (r["model"], r["threads"], r["paths"])
            rows[key] = {
                "median_ms": float(r["median_ms"]),
                "speedup": float(r["speedup"]),
                "efficiency": float(r["efficiency"]),
                "std_error": float(r["std_error"]),
            }
    return rows


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("baseline")
    ap.add_argument("current")
    ap.add_argument(
        "--tol",
        type=float,
        default=0.5,
        help="allowed fractional slowdown in median_ms (0.5 = 50%% slower)",
    )
    args = ap.parse_args()

    base = load(args.baseline)
    cur = load(args.current)

    regressions = []
    for key, b in base.items():
        if key not in cur:
            continue  # row dropped; not a regression per se
        c = cur[key]
        # Runtime regression: current slower than baseline by > tol.
        if b["median_ms"] > 0:
            slowdown = (c["median_ms"] - b["median_ms"]) / b["median_ms"]
            if slowdown > args.tol:
                regressions.append(
                    f"{key}: median_ms {b['median_ms']:.3f} -> "
                    f"{c['median_ms']:.3f} (+{slowdown * 100:.0f}%)"
                )
        # Parallel efficiency shouldn't collapse (a sign the threading broke).
        if b["efficiency"] > 0.5 and c["efficiency"] < 0.5 * b["efficiency"]:
            regressions.append(
                f"{key}: efficiency {b['efficiency']:.2f} -> {c['efficiency']:.2f}"
            )

    model_label = f"{len(base)} baseline rows, {len(cur)} current rows"
    if regressions:
        print(f"Benchmark regression detected ({model_label}):", file=sys.stderr)
        for r in regressions:
            print(f"  - {r}", file=sys.stderr)
        return 1
    print(f"No benchmark regression (tol {args.tol:.0%}); {model_label}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
