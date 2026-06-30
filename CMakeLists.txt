cmake_minimum_required(VERSION 3.20)
project(multi_asset_pricing_engine
        VERSION 0.1.0
        DESCRIPTION "Multi-asset quantitative pricing engine (C++ core)"
        # C is needed for the FFI C-side smoke test (tests/c_smoke_test.c);
        # without it CMake can't compile .c sources and silently drops them.
        LANGUAGES CXX C)

# Default to a release build if the user didn't pick one.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emit build/compile_commands.json so editors/clangd get include paths & flags.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MAPE_BUILD_TESTS    "Build the pricing-core tests"   ON)
option(MAPE_BUILD_BENCH    "Build the performance benchmarks" OFF)
option(MAPE_ENABLE_WARNINGS "Enable -Wall -Wextra on targets" ON)

# enable_testing() must be called at the top level (before add_subdirectory)
# so ctest run from the root build dir discovers tests registered in subdirs.
if(MAPE_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(core)
add_subdirectory(ffi)   # extern "C" bridge producing libmape (links the core)
add_subdirectory(bench) # performance benchmarks (guarded by MAPE_BUILD_BENCH)
