#!/usr/bin/env python3
"""Thin shim so `python scripts/fetch_data.py ...` still works.

The real implementation lives in the installable package at
``src/mape_data/fetch_data.py``. Prefer the uv entry point:

    uv run fetch-data AAPL MSFT --max-expiries 3
"""

import os
import sys

# Put src/ on the path so we can import the package without installing.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

from mape_data.fetch_data import main  # noqa: E402

if __name__ == "__main__":
    raise SystemExit(main())
