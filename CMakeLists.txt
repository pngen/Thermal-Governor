cmake_minimum_required(VERSION 3.21)

# Thermal Governor — vendor-neutral C++20 thermal governance runtime.
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.

project(ThermalGovernor
    VERSION 1.0.0
    DESCRIPTION "Vendor-neutral C++20 runtime for governing thermal domains, temperature headroom, derating, throttling constraints, recovery hysteresis and temperature-driven execution authority"
    HOMEPAGE_URL "https://github.com/pngen/Thermal-Governor"
    LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(THERMAL_GOVERNOR_IS_TOP_LEVEL ON)
else()
    set(THERMAL_GOVERNOR_IS_TOP_LEVEL OFF)
endif()

option(THERMAL_GOVERNOR_BUILD_TESTS "Build the Thermal Governor test suites" ${THERMAL_GOVERNOR_IS_TOP_LEVEL})
option(THERMAL_GOVERNOR_BUILD_EXAMPLES "Build the Thermal Governor examples" ${THERMAL_GOVERNOR_IS_TOP_LEVEL})
option(THERMAL_GOVERNOR_BUILD_BENCHMARKS "Build the Thermal Governor benchmarks" ${THERMAL_GOVERNOR_IS_TOP_LEVEL})
option(THERMAL_GOVERNOR_BUILD_CLI "Build the Thermal Governor CLI and processes" ${THERMAL_GOVERNOR_IS_TOP_LEVEL})
option(THERMAL_GOVERNOR_ENABLE_NVML "Enable the dynamically loaded NVML thermal backend" ON)
option(THERMAL_GOVERNOR_ENABLE_CUDA "Enable the real CUDA completed-work proof" OFF)
option(THERMAL_GOVERNOR_ENABLE_ASAN "Build with MSVC AddressSanitizer" OFF)
option(THERMAL_GOVERNOR_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

if(THERMAL_GOVERNOR_ENABLE_ASAN AND NOT MSVC)
    message(FATAL_ERROR "THERMAL_GOVERNOR_ENABLE_ASAN is implemented for MSVC only")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Warning configuration
# ---------------------------------------------------------------------------
add_library(thermal_governor_warnings INTERFACE)
if(MSVC)
    target_compile_options(thermal_governor_warnings INTERFACE
        /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /EHsc /MP)
    if(THERMAL_GOVERNOR_WARNINGS_AS_ERRORS)
        target_compile_options(thermal_governor_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(thermal_governor_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
    if(THERMAL_GOVERNOR_WARNINGS_AS_ERRORS)
        target_compile_options(thermal_governor_warnings INTERFACE -Werror)
    endif()
endif()

# ---------------------------------------------------------------------------
# Core library. Never requires CUDA or NVML.
# ---------------------------------------------------------------------------
add_library(ThermalGovernor
    src/version.cpp
    src/format.cpp
    src/time.cpp
    src/error.cpp
    src/quantity.cpp
    src/capability.cpp
    src/throttle.cpp
    src/evidence.cpp
    src/policy.cpp
    src/headroom.cpp
    src/domain.cpp
    src/coupling.cpp
    src/evaluation.cpp
    src/recovery.cpp
    src/action.cpp
    src/verification.cpp
    src/decision.cpp
    src/explanation.cpp
    src/backend.cpp
    src/nvml_backend.cpp
    src/cuda_workload.cpp
    src/protocol.cpp
    src/transport.cpp
    src/persistence.cpp
    src/wire.cpp
    src/snapshot.cpp
    src/governance.cpp
)
add_library(ThermalGovernor::ThermalGovernor ALIAS ThermalGovernor)

target_include_directories(ThermalGovernor
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# The warning interface is a build-time-only dependency: it is deliberately
# excluded from the exported target so consumers never inherit /W4 or /WX.
target_link_libraries(ThermalGovernor PRIVATE $<BUILD_INTERFACE:thermal_governor_warnings>)

target_compile_definitions(ThermalGovernor
    PUBLIC
        THERMAL_GOVERNOR_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
        THERMAL_GOVERNOR_VERSION_MINOR=${PROJECT_VERSION_MINOR}
        THERMAL_GOVERNOR_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    PRIVATE
        $<$<BOOL:${THERMAL_GOVERNOR_ENABLE_NVML}>:THERMAL_GOVERNOR_HAS_NVML=1>
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

if(MSVC)
    target_compile_options(ThermalGovernor PRIVATE /bigobj)
endif()

if(WIN32)
    target_link_libraries(ThermalGovernor PRIVATE ws2_32)
endif()

if(THERMAL_GOVERNOR_ENABLE_ASAN)
    message(STATUS "Thermal Governor: AddressSanitizer enabled")
    target_compile_options(ThermalGovernor PUBLIC /fsanitize=address /Zi)
    target_link_options(ThermalGovernor PUBLIC /INCREMENTAL:NO)
    # Locate the ASan runtime next to the compiler rather than hardcoding a
    # Visual Studio installation path.
    get_filename_component(_tg_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    if(EXISTS "${_tg_cl_dir}/clang_rt.asan_dynamic-x86_64.dll")
        set(THERMAL_GOVERNOR_ASAN_RUNTIME_DIR "${_tg_cl_dir}" CACHE INTERNAL "")
        message(STATUS "Thermal Governor: ASan runtime found at ${_tg_cl_dir}")
    else()
        message(WARNING "Thermal Governor: clang_rt.asan_dynamic-x86_64.dll not found next to the compiler; ASan binaries may not run")
    endif()
endif()

# Copy the AddressSanitizer runtime next to every first-party executable so
# sanitizer builds run without depending on a machine-wide PATH entry.
function(thermal_governor_link_asan_runtime target)
    if(THERMAL_GOVERNOR_ENABLE_ASAN AND DEFINED THERMAL_GOVERNOR_ASAN_RUNTIME_DIR)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${THERMAL_GOVERNOR_ASAN_RUNTIME_DIR}/clang_rt.asan_dynamic-x86_64.dll"
                "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endif()
endfunction()

if(THERMAL_GOVERNOR_ENABLE_CUDA)
    find_package(CUDAToolkit REQUIRED)
    # Target the architecture of the build host by default. Override with
    # -DCMAKE_CUDA_ARCHITECTURES for a redistributable build.
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
        set(CMAKE_CUDA_ARCHITECTURES native CACHE STRING "CUDA architectures to generate code for")
    endif()
    enable_language(CUDA)
    message(STATUS "Thermal Governor: CUDA toolkit ${CUDAToolkit_VERSION} enabled")
    target_compile_definitions(ThermalGovernor PRIVATE THERMAL_GOVERNOR_HAS_CUDA=1)
    # The kernel is an OBJECT library, so its objects are archived directly
    # into ThermalGovernor. Nothing about the kernel then appears in the
    # exported target: a CUDA-enabled install declares only the CUDA runtime
    # it genuinely needs, and the default CUDA-free install needs none.
    add_library(thermal_governor_cuda_kernel OBJECT src/cuda_kernel.cu)
    set_target_properties(thermal_governor_cuda_kernel PROPERTIES CUDA_STANDARD 17 CUDA_STANDARD_REQUIRED ON)
    target_include_directories(thermal_governor_cuda_kernel PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_definitions(thermal_governor_cuda_kernel PRIVATE
        THERMAL_GOVERNOR_HAS_CUDA=1)
    # The kernel objects are added to the archive explicitly. A generator
    # expression on the target name would not carry the objects across, and
    # adding them this way keeps the kernel entirely out of the exported
    # interface.
    target_sources(ThermalGovernor PRIVATE $<TARGET_OBJECTS:thermal_governor_cuda_kernel>)
    target_link_libraries(ThermalGovernor PRIVATE CUDA::cudart)
endif()

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
if(THERMAL_GOVERNOR_BUILD_CLI)
    add_subdirectory(tools)
endif()
if(THERMAL_GOVERNOR_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
if(THERMAL_GOVERNOR_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()
if(THERMAL_GOVERNOR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Installation and package export
# ---------------------------------------------------------------------------
install(TARGETS ThermalGovernor
    EXPORT ThermalGovernorTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(DIRECTORY include/thermal_governor
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    FILES_MATCHING PATTERN "*.hpp")

install(EXPORT ThermalGovernorTargets
    FILE ThermalGovernorTargets.cmake
    NAMESPACE ThermalGovernor::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ThermalGovernor")

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ThermalGovernorConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/ThermalGovernorConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ThermalGovernor")

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/ThermalGovernorConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/ThermalGovernorConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/ThermalGovernorConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ThermalGovernor")

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/ThermalGovernor")

message(STATUS "Thermal Governor ${PROJECT_VERSION}: tests=${THERMAL_GOVERNOR_BUILD_TESTS} examples=${THERMAL_GOVERNOR_BUILD_EXAMPLES} benchmarks=${THERMAL_GOVERNOR_BUILD_BENCHMARKS} cli=${THERMAL_GOVERNOR_BUILD_CLI} nvml=${THERMAL_GOVERNOR_ENABLE_NVML} cuda=${THERMAL_GOVERNOR_ENABLE_CUDA} asan=${THERMAL_GOVERNOR_ENABLE_ASAN}")
