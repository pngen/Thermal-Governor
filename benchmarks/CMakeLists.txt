# Thermal Governor - micro-benchmarks.
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# A single benchmark executable. The warning interface is defined locally so
# that the benchmarks never depend on a parent-scope build-tree target.

add_library(thermal_governor_benchmark_warnings INTERFACE)
if(MSVC)
    target_compile_options(thermal_governor_benchmark_warnings INTERFACE
        /W4 /permissive- /utf-8 /Zc:__cplusplus /WX)
endif()

find_package(Threads REQUIRED)

add_executable(thermal_governor_bench thermal_governor_bench.cpp)
target_link_libraries(thermal_governor_bench
    PRIVATE
        ThermalGovernor::ThermalGovernor
        thermal_governor_benchmark_warnings
        Threads::Threads)
set_target_properties(thermal_governor_bench PROPERTIES OUTPUT_NAME thermal_governor_bench)
