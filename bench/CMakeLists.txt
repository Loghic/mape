# Performance benchmarks (plan §12). Off by default — benchmarks want a Release
# build with optimisation, and aren't part of the correctness test suite.
#
#   cmake -S . -B build -DMAPE_BUILD_BENCH=ON -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --target mape_bench
#   ./build/bench/mape_bench > results.csv     # CSV on stdout, summary on stderr

if(MAPE_BUILD_BENCH)
    add_executable(mape_bench bench_main.cpp)
    target_link_libraries(mape_bench PRIVATE mape::core)
    target_include_directories(mape_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_features(mape_bench PRIVATE cxx_std_20)

    # Benchmarks are meaningless unoptimised; force -O3/NDEBUG for this target
    # regardless of the overall build type.
    target_compile_options(mape_bench PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-O3 -DNDEBUG>)
    if(MAPE_ENABLE_WARNINGS)
        target_compile_options(mape_bench PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>)
    endif()
endif()
