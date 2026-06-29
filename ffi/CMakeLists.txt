# pricing-ffi — the extern "C" wrapper around the core. This is what produces
# `libmape` (the library Rust links). Built as a static lib by default; flip
# BUILD_SHARED_LIBS=ON for a .so/.dylib/.dll.

add_library(mape_ffi src/mape_c_api.cpp)
add_library(mape::ffi ALIAS mape_ffi)

target_include_directories(mape_ffi PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_link_libraries(mape_ffi PUBLIC mape::core)
target_compile_features(mape_ffi PRIVATE cxx_std_20)

if(MAPE_ENABLE_WARNINGS)
    target_compile_options(mape_ffi PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra>)
endif()

# A friendly alias so `libmape` is the on-disk name regardless of target name.
set_target_properties(mape_ffi PROPERTIES OUTPUT_NAME mape)

# Install rule so `cmake --install` (used by the Rust build.rs via the `cmake`
# crate) drops libmape and the public header in a predictable layout.
include(GNUInstallDirs)
install(TARGETS mape_ffi
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES include/mape_c_api.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# ---- C-side smoke test --------------------------------------------------
if(MAPE_BUILD_TESTS)
    # Compiled as C to prove the ABI is consumable from a non-C++ caller.
    add_executable(mape_ffi_c_test tests/c_smoke_test.c)
    set_target_properties(mape_ffi_c_test PROPERTIES C_STANDARD 11)
    target_link_libraries(mape_ffi_c_test PRIVATE mape_ffi)
    # The driver is C but links a C++ library, so the C++ runtime must be on
    # the final link line. mape_ffi is a CXX target, so CMake propagates the
    # C++ linker language to this executable automatically — no explicit
    # libstdc++/libc++ needed (adding it produced a duplicate-library warning).
    add_test(NAME mape_ffi_c_test COMMAND mape_ffi_c_test)
endif()
