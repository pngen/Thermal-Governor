# Thermal Governor — test suites.
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.

add_library(thermal_governor_test_framework STATIC
    framework.cpp
    process_util.cpp)
target_link_libraries(thermal_governor_test_framework PUBLIC ThermalGovernor::ThermalGovernor)
target_include_directories(thermal_governor_test_framework PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(thermal_governor_test_framework PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /WX)
endif()

set(THERMAL_GOVERNOR_TEST_SUITES
    test_quantity
    test_identity
    test_policy
    test_headroom
    test_evidence
    test_coupling
    test_protocol
    test_persistence
    test_transport
    test_explanation
    test_state_machine
    test_governance
    test_actions
    test_recovery
    test_fencing
    test_restart
    test_property
    test_adversarial
    test_concurrency
    test_multiprocess
    test_nvml
    test_cuda)

foreach(_tg_suite ${THERMAL_GOVERNOR_TEST_SUITES})
    add_executable(${_tg_suite} ${_tg_suite}.cpp)
    target_link_libraries(${_tg_suite} PRIVATE thermal_governor_test_framework)
    if(MSVC)
        target_compile_options(${_tg_suite} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
        if(THERMAL_GOVERNOR_WARNINGS_AS_ERRORS)
            target_compile_options(${_tg_suite} PRIVATE /WX)
        endif()
    endif()
    thermal_governor_link_asan_runtime(${_tg_suite})
    add_test(NAME ${_tg_suite} COMMAND ${_tg_suite})
endforeach()

# A deliberate sanitizer self-test. It must produce an AddressSanitizer
# report, so it is never registered as a passing test.
add_executable(thermal_governor_asan_self_test asan_self_test.cpp)
target_link_libraries(thermal_governor_asan_self_test PRIVATE ThermalGovernor::ThermalGovernor)
if(MSVC)
    target_compile_options(thermal_governor_asan_self_test PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
endif()
thermal_governor_link_asan_runtime(thermal_governor_asan_self_test)
