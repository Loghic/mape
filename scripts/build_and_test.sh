#!/usr/bin/env bash
# Build and run the pricing-core tests.
#
# Primary path uses CMake. If CMake isn't installed, falls back to a direct
# g++ invocation that compiles the same test harness (header-only core).
#
# Usage:
#   ./scripts/build_and_test.sh [-v|--verbose] [-h|--help]
#
#   -v, --verbose   Show the full compiler/linker invocations (passes -v to the
#                   cmake build and --output-on-failure -V to ctest). Handy when
#                   diagnosing link errors.
#   -h, --help      Show this help and exit.
#
# A compile_commands.json (compilation database for editors / clangd) is
# always generated under build/ and symlinked to the repo root.
set -euo pipefail

VERBOSE=0
for arg in "$@"; do
    case "$arg" in
        -v|--verbose) VERBOSE=1 ;;
        -h|--help)
            sed -n '2,16p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *)
            echo "Unknown option: $arg" >&2
            echo "Try '$0 --help'." >&2
            exit 2 ;;
    esac
done

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if command -v cmake >/dev/null 2>&1; then
    echo ">> Building with CMake$([ "$VERBOSE" = 1 ] && echo ' (verbose)')"

    # -DCMAKE_EXPORT_COMPILE_COMMANDS=ON writes build/compile_commands.json.
    cmake -S "$ROOT" -B "$ROOT/build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

    if [ "$VERBOSE" = 1 ]; then
        cmake --build "$ROOT/build" -j -v
    else
        cmake --build "$ROOT/build" -j
    fi

    # Make the compilation database discoverable at the repo root (most editors
    # and clangd look for it there).
    if [ -f "$ROOT/build/compile_commands.json" ]; then
        ln -sf build/compile_commands.json "$ROOT/compile_commands.json"
        echo ">> compile_commands.json ready (symlinked at repo root)"
    fi

    if [ "$VERBOSE" = 1 ]; then
        ctest --test-dir "$ROOT/build" --output-on-failure -V
    else
        ctest --test-dir "$ROOT/build" --output-on-failure
    fi
else
    echo ">> CMake not found — compiling tests directly with g++"
    CXX="${CXX:-g++}"
    GPP_FLAGS=(-std=c++20 -O2 -Wall -Wextra -pthread)
    [ "$VERBOSE" = 1 ] && GPP_FLAGS+=(-v)
    "$CXX" "${GPP_FLAGS[@]}" \
        -I"$ROOT/core/include" \
        "$ROOT/core/tests/test_main.cpp" -o "$ROOT/build_mape_tests"
    "$ROOT/build_mape_tests"

    # Compile-time pricing tests (plan §5.4): static_asserts validated by the
    # build itself, plus a runtime cross-check.
    "$CXX" "${GPP_FLAGS[@]}" \
        -I"$ROOT/core/include" \
        "$ROOT/core/tests/test_compile_time.cpp" -o "$ROOT/build_mape_ct_tests"
    "$ROOT/build_mape_ct_tests"
fi
