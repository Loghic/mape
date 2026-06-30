# Building & Testing

How to build the C++ core + FFI, run the tests, generate a compilation database
for your editor, and run the sanitizers. The README has the one-liner; this is
the detailed version.

- [Build & test with CMake](#build--test-with-cmake)
- [What each command does](#what-each-command-does)
- [Without CMake](#without-cmake)
- [Editing the code: compile_commands.json](#editing-the-code-compile_commandsjson)
- [Sanitizers](#sanitizers)
- [Benchmarks](#benchmarks)

## Build & test with CMake

With CMake (≥ 3.20):

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Or just run `scripts/build_and_test.sh` (add `-v` for verbose).

## What each command does

CMake works in two stages: a **configure** stage that reads the
`CMakeLists.txt` files and generates a build system (e.g. Makefiles), and a
**build** stage that actually compiles.

`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` — the *configure* step.

- `-S .` — **S**ource directory: where the top-level `CMakeLists.txt` lives
  (`.` = the current folder).
- `-B build` — **B**uild directory: where CMake writes all generated files and
  compiled output. Keeping it separate from the source ("out-of-source build")
  means you can delete `build/` anytime to start clean without touching code.
- `-DCMAKE_BUILD_TYPE=Release` — sets a CMake cache variable. `-D<NAME>=<value>`
  is how you pass options. `Release` turns on optimisation (`-O3`) and disables
  debug assertions; use `Debug` instead for debugging symbols and no
  optimisation.

`cmake --build build -j` — the *build* step.

- `--build build` — compile the project that was configured into the `build`
  directory. (This calls the underlying make/ninja for you, so the commands are
  the same on every platform.)
- `-j` — build in parallel using all available CPU cores. You can also write
  `-j8` to cap it at 8 jobs.

`ctest --test-dir build --output-on-failure` — run the tests.

- `--test-dir build` — point ctest at the build directory (where the test
  registry was generated).
- `--output-on-failure` — print a test's full output only if it fails; passing
  tests stay quiet. Add `-V` if you want output from every test, pass or fail.

> If ctest ever reports **"No tests were found"**, delete the build directory
> and reconfigure (`rm -rf build` then re-run the configure step). ctest reads
> a cached test registry, so it must be regenerated after CMake changes.

## Without CMake

You can compile the tests directly — the core is header-only, so one `g++`
call is enough:

```bash
g++ -std=c++20 -O2 -pthread -Icore/include core/tests/test_main.cpp -o mape_tests
./mape_tests
```

- `-std=c++20` — the core uses C++20 concepts, coroutines, and `<latch>`/
  `<semaphore>`, so this is required.
- `-O2` — optimisation (Monte Carlo runs much faster).
- `-pthread` — links the threading runtime (`std::thread`, `std::async`).
- `-Icore/include` — adds the header search path so `#include "mape/..."`
  resolves.

Either path is wrapped by `scripts/build_and_test.sh`.

## Editing the code: `compile_commands.json`

Editors and language servers (VS Code C/C++, clangd, CLion) understand your
include paths and compiler flags by reading a `compile_commands.json` file — a
"compilation database". Generate it by adding one flag at the configure step:

```bash
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

This writes `build/compile_commands.json`. Most tools look for it at the project
root, so symlink it there once:

```bash
ln -sf build/compile_commands.json compile_commands.json
```

After that, code navigation, autocomplete, and inline error checking will work
in your editor. Re-run the configure step whenever you add new source files so
the database stays current.

> Note: the core is header-only, so the database is most useful once the FFI
> source files (`ffi/src/*.cpp`) are part of the build. If you use **clangd**,
> that's all you need; for the **VS Code C/C++ extension**, point
> `C_Cpp.default.compileCommands` at the file in your settings.

## Sanitizers

The concurrency code (parallel Monte Carlo, the thread pool) must stay clean
under ThreadSanitizer:

```bash
g++ -std=c++20 -O1 -g -fsanitize=thread -pthread -Icore/include \
    core/tests/test_main.cpp -o mape_tsan && ./mape_tsan
```

Address + UndefinedBehavior sanitizers catch memory and UB issues:

```bash
g++ -std=c++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    -pthread -Icore/include core/tests/test_main.cpp -o mape_asan && ./mape_asan
```

CI runs both (see `.github/workflows/ci.yml`).

## Benchmarks

Performance benchmarks (plan §12) are off by default — they want an optimised
build and aren't part of the correctness suite:

```bash
cmake -S . -B build -DMAPE_BUILD_BENCH=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --target mape_bench
./build/bench/mape_bench > results.csv     # CSV on stdout, summary on stderr
```

The CSV columns are `model, threads, paths, median_ms, speedup, efficiency,
std_error` — diffable across machines and commits.
