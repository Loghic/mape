// build.rs — builds the C++ pricing library and points the linker at it.
//
// Strategy (plan §9): drive CMake via the `cmake` crate. We configure the repo
// root with tests OFF, build the `mape_ffi` target, install it into Cargo's
// OUT_DIR, then emit the link directives. This keeps all platform-specific
// linking isolated here; main.rs/bridge.rs stay platform-agnostic.

use std::path::PathBuf;

fn main() {
    // The C++ project root is the parent of this `gui/` crate.
    let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let cpp_root = manifest_dir
        .parent()
        .expect("gui/ must have a parent (the repo root)")
        .to_path_buf();

    // Configure + build + install the FFI library (which links the core).
    // `cmake::Config::build()` runs configure, build, then `cmake --install`.
    let dst = cmake::Config::new(&cpp_root)
        .define("MAPE_BUILD_TESTS", "OFF") // no test binaries in the GUI build
        .define("MAPE_ENABLE_WARNINGS", "OFF") // keep dependency builds quiet
        .build_target("mape_ffi")
        .build();

    // The `cmake` crate installs into `<OUT_DIR>`. Libraries land in lib/ (or
    // lib64/ on some distros); add both search paths to be safe.
    println!(
        "cargo:rustc-link-search=native={}",
        dst.join("lib").display()
    );
    println!(
        "cargo:rustc-link-search=native={}",
        dst.join("lib64").display()
    );
    // When only the target is built (not installed), the artifact lives under
    // the build tree; add that too as a fallback.
    println!(
        "cargo:rustc-link-search=native={}",
        dst.join("build").join("ffi").display()
    );

    // Link our static library (libmape.a / mape.lib).
    println!("cargo:rustc-link-lib=static=mape");

    // The library is C++; pull in the C++ standard library so symbols like
    // operator new / std::* resolve. The name differs per platform.
    link_cpp_stdlib();

    // Rebuild if any C++ source or build file changes.
    for rel in ["core", "ffi", "CMakeLists.txt"] {
        println!("cargo:rerun-if-changed={}", cpp_root.join(rel).display());
    }
}

fn link_cpp_stdlib() {
    let target = std::env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
    match target.as_str() {
        "macos" | "ios" => println!("cargo:rustc-link-lib=dylib=c++"),
        "windows" => { /* MSVC links the C++ runtime automatically */ }
        // Linux and most others use libstdc++ with GCC/Clang.
        _ => println!("cargo:rustc-link-lib=dylib=stdc++"),
    }
}
