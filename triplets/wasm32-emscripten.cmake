# Overlay triplet for the Babylon USD Importer WebAssembly build.
#
# It mirrors vcpkg's community `wasm32-emscripten` triplet (static libraries,
# Emscripten toolchain chainloaded through vcpkg) with two deliberate choices:
#
#   * VCPKG_BUILD_TYPE is pinned to `release`. The wasm module only ever ships a
#     release build, and building each heavy dependency (OpenUSD above all) twice
#     would roughly double an already long cold build.
#   * No `-pthread` is added anywhere. The whole dependency chain — oneTBB (built
#     with EMSCRIPTEN_WITHOUT_PTHREAD by the overlay tbb port), zlib and OpenUSD —
#     is single-threaded, so the final module needs neither SharedArrayBuffer nor a
#     cross-origin-isolated host. Keep this file and the tbb overlay in agreement:
#     enabling threads here without rebuilding tbb (or vice versa) will not link.
#   * Release libraries are built with wasm SIMD and LTO. SIMD accelerates OpenUSD's
#     math-heavy traversal; LTO lets the final monolithic
#     link remove unused OpenUSD code across archive boundaries.
#
# Requires Emscripten on PATH, or the EMSDK / EMSCRIPTEN_ROOT environment variable.

set(VCPKG_ENV_PASSTHROUGH_UNTRACKED EMSCRIPTEN_ROOT EMSDK PATH)

set(VCPKG_TARGET_ARCHITECTURE wasm32)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Emscripten)
set(VCPKG_BUILD_TYPE release)
set(VCPKG_C_FLAGS_RELEASE "-O3 -msimd128 -flto")
set(VCPKG_CXX_FLAGS_RELEASE "-O3 -msimd128 -flto")
set(VCPKG_LINKER_FLAGS_RELEASE "-msimd128 -flto")

# Chainload vcpkg's wrapper toolchain rather than Emscripten.cmake directly: the
# wrapper includes Emscripten.cmake and then applies VCPKG_C(XX)_FLAGS and
# VCPKG_LINKER_FLAGS, which would otherwise be silently dropped.
get_filename_component(
    VCPKG_CHAINLOAD_TOOLCHAIN_FILE
    "${CMAKE_CURRENT_LIST_DIR}/../cmake/emscripten.cmake"
    ABSOLUTE
)
