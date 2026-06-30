#!/usr/bin/env python3
"""Verify the C ABI header and the Rust bindings agree (plan §14.1).

Parses every `MAPE_API` function in ffi/include/mape_c_api.h and every
`extern "C"` declaration in gui/src/bridge.rs, normalises each parameter to a
small set of ABI-level kinds (double, usize, i32, pointers, ...), and checks
that both sides declare the same functions with the same arity and parameter
types. Exits non-zero on any mismatch, so CI catches a drifted binding before
it becomes a runtime crash across the FFI seam.

Run from the repo root:  python scripts/check_abi_consistency.py
"""
from __future__ import annotations

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "ffi", "include", "mape_c_api.h")
BRIDGE = os.path.join(ROOT, "gui", "src", "bridge.rs")


def c_kind(tok: str) -> str:
    tok = re.sub(r"\bconst\b", "", tok).strip()
    if "*" in tok and "char" in tok:
        return "cstr_ptr"
    if "MapeEngine" in tok and "*" in tok:
        return "engine_ptr"
    if "double" in tok and "*" in tok:
        return "double_ptr"
    if "double" in tok:
        return "double"
    if "size_t" in tok:
        return "usize"
    if "int" in tok and "*" in tok:
        return "int_ptr"
    if "int" in tok and "*" not in tok:
        return "i32"
    enums = ("MapeModel", "MapeOptionType", "MapeExercise", "MapeStatus",
             "MapeGreek", "MapeExotic", "MapeBarrierKind")
    if any(e in tok for e in enums):
        return "i32"
    if "void" in tok:
        return "void"
    return tok


def parse_c() -> dict[str, list[str]]:
    text = open(HEADER).read()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//.*", "", text)
    funcs: dict[str, list[str]] = {}
    for m in re.finditer(
        r"MAPE_API\s+([\w\s\*]+?)\s+(\w+)\s*\(([^;]*?)\)\s*;", text, flags=re.S
    ):
        args = m.group(3).strip()
        params = [] if args in ("void", "") else [c_kind(a) for a in args.split(",")]
        funcs[m.group(2)] = params
    return funcs


def rust_kind(tok: str) -> str:
    tok = tok.strip()
    if "c_char" in tok and "*const" in tok:
        return "cstr_ptr"
    if "MapeEngine" in tok and "*mut" in tok:
        return "engine_ptr"
    if "c_double" in tok and "*" in tok:
        return "double_ptr"
    if "c_double" in tok or tok == "f64":
        return "double"
    if "usize" in tok:
        return "usize"
    if "i32" in tok and "*" in tok:
        return "int_ptr"
    if tok == "i32":
        return "i32"
    return tok


def parse_rust() -> dict[str, list[str]]:
    text = open(BRIDGE).read()
    block = re.search(r'extern "C"\s*\{(.*?)\n    \}', text, flags=re.S)
    if not block:
        print("ERROR: could not find the extern \"C\" block in bridge.rs", file=sys.stderr)
        sys.exit(2)
    funcs: dict[str, list[str]] = {}
    for m in re.finditer(
        r"pub fn (\w+)\s*\(([^;]*?)\)\s*(->\s*[\w :\*]+)?\s*;", block.group(1), flags=re.S
    ):
        params = []
        for a in m.group(2).split(","):
            a = a.strip()
            if not a:
                continue
            t = a.split(":", 1)[1] if ":" in a else a
            params.append(rust_kind(t))
        funcs[m.group(1)] = params
    return funcs


def main() -> int:
    c = parse_c()
    r = parse_rust()
    problems = []
    for name, cp in c.items():
        if name not in r:
            problems.append(f"missing in Rust: {name}")
        elif cp != r[name]:
            problems.append(f"mismatch {name}: C={cp} Rust={r[name]}")
    for name in r:
        if name not in c:
            problems.append(f"extra in Rust (no C decl): {name}")

    if problems:
        print("ABI consistency check FAILED:", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        return 1
    print(f"ABI consistency OK: {len(c)} functions match across C header and Rust.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
