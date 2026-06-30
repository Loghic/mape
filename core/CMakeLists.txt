# pricing-core — the C++ pricing engine (the "heart"). Header-only for now:
# all logic lives in templates/inline functions under include/mape/. We still
# expose it as a proper INTERFACE library so dependents just link `mape::core`.

add_library(mape_core INTERFACE)
add_library(mape::core ALIAS mape_core)

target_include_directories(mape_core INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_compile_features(mape_core INTERFACE cxx_std_20)

# Threads (std::thread / std::async).
find_package(Threads REQUIRED)
target_link_libraries(mape_core INTERFACE Threads::Threads)

# ---- Tests --------------------------------------------------------------
if(MAPE_BUILD_TESTS)
    # enable_testing() is called once at the top level so `ctest` from the
    # root build directory finds this test.

    # Prefer GoogleTest if available; otherwise fall back to the bundled
    # dependency-free harness (test_main.cpp), which runs the same checks.
    add_executable(mape_core_tests tests/test_main.cpp)
    target_link_libraries(mape_core_tests PRIVATE mape::core)
    target_compile_features(mape_core_tests PRIVATE cxx_std_20)
    if(MAPE_ENABLE_WARNINGS)
        target_compile_options(mape_core_tests PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>)
    endif()

    add_test(NAME mape_core_tests COMMAND mape_core_tests)

    # Compile-time pricing tests (plan §5.4). Most checks are static_asserts,
    # so simply building this target validates them; it also runs a runtime
    # cross-check against the analytic model.
    add_executable(mape_ct_tests tests/test_compile_time.cpp)
    target_link_libraries(mape_ct_tests PRIVATE mape::core)
    target_compile_features(mape_ct_tests PRIVATE cxx_std_20)
    if(MAPE_ENABLE_WARNINGS)
        target_compile_options(mape_ct_tests PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>)
    endif()
    add_test(NAME mape_ct_tests COMMAND mape_ct_tests)
endif()
