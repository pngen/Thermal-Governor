# Thermal Governor — coordinator, worker and command line tools.
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.

add_library(thermal_governor_tools_support STATIC session.cpp)
target_link_libraries(thermal_governor_tools_support PUBLIC ThermalGovernor::ThermalGovernor)
target_include_directories(thermal_governor_tools_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(thermal_governor_tools_support PRIVATE /W4 /permissive- /utf-8
        /Zc:__cplusplus)
    if(THERMAL_GOVERNOR_WARNINGS_AS_ERRORS)
        target_compile_options(thermal_governor_tools_support PRIVATE /WX)
    endif()
endif()

add_executable(thermal_governor_coordinator tg_coordinator.cpp)
target_link_libraries(thermal_governor_coordinator PRIVATE thermal_governor_tools_support)

add_executable(thermal_governor_worker tg_worker.cpp)
target_link_libraries(thermal_governor_worker PRIVATE thermal_governor_tools_support)

add_executable(thermal_governor_cli tg_cli.cpp)
target_link_libraries(thermal_governor_cli PRIVATE thermal_governor_tools_support)

foreach(_tg_tool thermal_governor_coordinator thermal_governor_worker thermal_governor_cli)
    if(MSVC)
        target_compile_options(${_tg_tool} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
        if(THERMAL_GOVERNOR_WARNINGS_AS_ERRORS)
            target_compile_options(${_tg_tool} PRIVATE /WX)
        endif()
    endif()
    thermal_governor_link_asan_runtime(${_tg_tool})
endforeach()

install(TARGETS thermal_governor_coordinator thermal_governor_worker thermal_governor_cli
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
