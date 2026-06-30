#!/usr/bin/env bash
# Reformat (or check) the C/C++ sources with clang-format.
#
# Discovers the same files the CI clang-format job checks (core/, ffi/, bench/)
# and runs clang-format against .clang-format. By default it rewrites files in
# place; --check only reports divergences (and exits non-zero) without touching
# anything — exactly what CI does, so you can reproduce the gate locally.
#
# Usage:
#   ./scripts/format.sh            # fix every file in place
#   ./scripts/format.sh --check    # report-only, non-zero exit if any diverge
#   ./scripts/format.sh -h|--help  # show this help
#
# Run from the repo root (it resolves paths relative to the script location, so
# it works from anywhere).
set -euo pipefail

CHECK=0
for arg in "$@"; do
    case "$arg" in
        --check) CHECK=1 ;;
        -h|--help)
            sed -n '2,14p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *)
            echo "Unknown argument: $arg (try --help)" >&2
            exit 2 ;;
    esac
done

# Repo root = parent of this script's directory.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# Prefer a versioned binary if present, else plain clang-format.
CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"
if ! command -v "$CLANG_FORMAT" >/dev/null 2>&1; then
    echo "error: '$CLANG_FORMAT' not found on PATH." >&2
    echo "Install it (e.g. 'brew install clang-format' or" \
         "'apt-get install clang-format'), or set CLANG_FORMAT=clang-format-17." >&2
    exit 1
fi

# Same file set as .github/workflows/static-analysis.yml.
mapfile -t FILES < <(find core ffi bench \
    \( -name '*.hpp' -o -name '*.cpp' -o -name '*.c' -o -name '*.h' \) | sort)

if [ "${#FILES[@]}" -eq 0 ]; then
    echo "No C/C++ files found under core/ ffi/ bench/." >&2
    exit 0
fi

echo "clang-format: $("$CLANG_FORMAT" --version)"
echo "Files: ${#FILES[@]}"

if [ "$CHECK" -eq 1 ]; then
    # Matches CI: dry-run, error on any divergence.
    if "$CLANG_FORMAT" --dry-run --Werror "${FILES[@]}"; then
        echo "OK: all files match .clang-format."
    else
        echo "Some files diverge. Run './scripts/format.sh' to fix them." >&2
        exit 1
    fi
else
    "$CLANG_FORMAT" -i "${FILES[@]}"
    echo "Reformatted ${#FILES[@]} files in place."
fi
