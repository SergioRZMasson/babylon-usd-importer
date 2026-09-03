cmake_minimum_required(VERSION 3.24)

# vcpkg provides the static monolithic OpenUSD build used by WebAssembly.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "vcpkg toolchain file")
    else()
        message(FATAL_ERROR
            "VCPKG_ROOT is not set and no CMAKE_TOOLCHAIN_FILE was provided.\n"
            "Configure with the `wasm` preset or export VCPKG_ROOT.")
    endif()
endif()

project(babylon_usd_importer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(BUILD_NPM_PACKAGE "Build the TypeScript package after the Wasm module" OFF)

find_package(pxr CONFIG REQUIRED)
find_package(ZLIB REQUIRED)

set(_pxr_targets
    arch tf gf pegtl js trace work plug vt ts ar kind sdf sdr pcp usd usdGeom usdVol
    usdMedia usdShade usdLod usdLux usdProc usdProfiles usdRender usdHydra usdRi
    usdSemantics usdSkel usdUI usdUtils usdPhysics usd_m
)

if(NOT EMSCRIPTEN)
    message(FATAL_ERROR
        "This repository currently builds the browser WebAssembly importer only. "
        "Configure with the `wasm` preset.")
endif()

get_filename_component(PXR_INSTALL_PREFIX "${PXR_CMAKE_DIR}/../.." ABSOLUTE)
set(PXR_USD_PLUGIN_RESOURCES "${PXR_INSTALL_PREFIX}/lib/usd")
if(NOT EXISTS "${PXR_USD_PLUGIN_RESOURCES}")
    message(FATAL_ERROR
        "Expected OpenUSD resources at '${PXR_USD_PLUGIN_RESOURCES}'. "
        "The overlay port must build OpenUSD as a static monolithic library.")
endif()

# OpenUSD exports per-module resource embedding options. The importer stages those resources
# once below, avoiding hundreds of duplicate --embed-file linker arguments.
foreach(_target IN LISTS _pxr_targets)
    if(TARGET ${_target})
        set_property(TARGET ${_target} PROPERTY INTERFACE_LINK_OPTIONS "")
    endif()
endforeach()

add_library(babylonUsdScene STATIC
    src/directScene.cpp
)
target_include_directories(babylonUsdScene PUBLIC src)
target_compile_options(babylonUsdScene PRIVATE -O3 -msimd128 -flto)
target_link_libraries(babylonUsdScene PUBLIC
    usd usdGeom usdSkel usdShade ZLIB::ZLIB
)
if(TARGET usd_m)
    target_link_libraries(babylonUsdScene PUBLIC usd_m)
endif()

add_executable(babylon-usd-importer
    src/directBindings.cpp
    src/webResolver.cpp
)
target_include_directories(babylon-usd-importer PRIVATE src)
target_link_libraries(babylon-usd-importer PRIVATE babylonUsdScene)
target_compile_options(babylon-usd-importer PRIVATE -O3 -msimd128 -flto)
target_link_options(babylon-usd-importer PRIVATE
    "SHELL:-Wl,--whole-archive $<TARGET_FILE:usd_m> -Wl,--no-whole-archive"
)

set(_resource_stage "${CMAKE_BINARY_DIR}/usd-resources")
file(REMOVE_RECURSE "${_resource_stage}")
file(COPY "${PXR_USD_PLUGIN_RESOURCES}/" DESTINATION "${_resource_stage}")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/resources/usdWebResolver"
     DESTINATION "${_resource_stage}")

target_link_options(babylon-usd-importer PRIVATE
    --bind
    -sMODULARIZE=1
    -sEXPORT_NAME=createBabylonUsdImporterModule
    -sENVIRONMENT=web,worker,node
    -sALLOW_MEMORY_GROWTH=1
    -sMEMORY_GROWTH_GEOMETRIC_STEP=0.5
    -sMAXIMUM_MEMORY=2GB
    -sFORCE_FILESYSTEM=1
    -sEXPORTED_RUNTIME_METHODS=FS,HEAPU8
    -sSTACK_SIZE=1MB
    -sEXPORT_ES6=1
    -sASSERTIONS=0
    -msimd128
    -flto
    --strip-debug
    "SHELL:--preload-file ${_resource_stage}@/usd"
)
set_target_properties(babylon-usd-importer PROPERTIES
    OUTPUT_NAME "babylon-usd-importer"
    SUFFIX ".js"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(BUILD_NPM_PACKAGE)
    find_program(NPM_EXECUTABLE npm REQUIRED)
    set(_package_dir "${CMAKE_CURRENT_SOURCE_DIR}/package")
    set(_npm_stamp "${CMAKE_CURRENT_BINARY_DIR}/package-npm-install.stamp")
    add_custom_command(
        OUTPUT "${_npm_stamp}"
        COMMAND "${NPM_EXECUTABLE}" install --no-audit --no-fund
        COMMAND "${CMAKE_COMMAND}" -E touch "${_npm_stamp}"
        WORKING_DIRECTORY "${_package_dir}"
        DEPENDS "${_package_dir}/package.json" "${_package_dir}/package-lock.json"
        COMMENT "Installing importer package dependencies"
        USES_TERMINAL
        VERBATIM
    )
    add_custom_target(npm-package ALL
        COMMAND "${CMAKE_COMMAND}" -E env
                "BABYLON_USD_IMPORTER_BIN=$<TARGET_FILE_DIR:babylon-usd-importer>"
                "${NPM_EXECUTABLE}" run build
        WORKING_DIRECTORY "${_package_dir}"
        DEPENDS "${_npm_stamp}"
        COMMENT "Building babylon-usd-importer package"
        USES_TERMINAL
        VERBATIM
    )
    add_dependencies(npm-package babylon-usd-importer)
endif()
