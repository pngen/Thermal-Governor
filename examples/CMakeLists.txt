# Thermal Governor - standalone examples.
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# One executable per example source file. Each example links the public
# ThermalGovernor::ThermalGovernor target and a local warning interface.
#
# The parent scope warning target is deliberately not referenced here: it is a
# build-tree-only detail of the parent directory. This interface is local to
# the examples directory and carries the same strict warning policy.

add_library(thermal_governor_example_warnings INTERFACE)
if(MSVC)
    target_compile_options(thermal_governor_example_warnings INTERFACE
        /W4 /permissive- /utf-8 /Zc:__cplusplus /WX)
endif()

set(thermal_governor_example_sources
    basic_evaluation
    headroom
    derating_and_hysteresis
    recovery_gating
    domain_coupling
    synthetic_rack_envelope
    stale_telemetry_rejection
    deterministic_explanation
    real_nvml_telemetry
    real_cuda_thermal
)

foreach(thermal_governor_example IN LISTS thermal_governor_example_sources)
    set(thermal_governor_example_target "thermal_governor_example_${thermal_governor_example}")
    add_executable(${thermal_governor_example_target} "${thermal_governor_example}.cpp")
    target_link_libraries(${thermal_governor_example_target}
        PRIVATE
            ThermalGovernor::ThermalGovernor
            thermal_governor_example_warnings)
    set_target_properties(${thermal_governor_example_target} PROPERTIES
        OUTPUT_NAME "example_${thermal_governor_example}")
endforeach()
