# Thermal Governor — independent downstream consumer.
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0.
#
# This project is deliberately NOT part of the Thermal Governor build. It
# exists to prove that an unrelated downstream project can consume only the
# installed artifacts:
#
#   cmake -S consumer -B <build> -DCMAKE_PREFIX_PATH=<install-prefix>
#   cmake --build <build>
#   <build>/thermal_governor_consumer
#
# It must resolve ThermalGovernor through find_package(CONFIG) alone, with no
# reference to the source tree, the build tree or any private header.

cmake_minimum_required(VERSION 3.21)
project(ThermalGovernorConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(ThermalGovernor CONFIG REQUIRED)

add_executable(thermal_governor_consumer consumer.cpp)

if(MSVC)
    target_compile_options(thermal_governor_consumer PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /WX)
endif()

target_link_libraries(thermal_governor_consumer PRIVATE ThermalGovernor::ThermalGovernor)

# Fail loudly if a private header ever leaks into the public include path.
get_target_property(_tg_includes ThermalGovernor::ThermalGovernor INTERFACE_INCLUDE_DIRECTORIES)
if(NOT _tg_includes)
    message(FATAL_ERROR "ThermalGovernor::ThermalGovernor exposes no include directories")
endif()
message(STATUS "ThermalGovernor consumer include directories: ${_tg_includes}")
message(STATUS "ThermalGovernor consumer version: ${ThermalGovernor_VERSION}")
