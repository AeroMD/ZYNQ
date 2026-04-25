# Copyright (C) 2023-2025 Advanced Micro Devices, Inc.  All rights reserved.
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.14.7)
project(xiltimer)

include(${CMAKE_CURRENT_SOURCE_DIR}/XiltimerExample.cmake NO_POLICY_SCOPE)
include(${CMAKE_CURRENT_SOURCE_DIR}/xiltimer.cmake NO_POLICY_SCOPE)

find_package(common)
collector_create (PROJECT_LIB_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}")
collector_create (PROJECT_LIB_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}")
include_directories(${CMAKE_BINARY_DIR}/include)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/core)

collect (PROJECT_LIB_SOURCES xiltimer.c)
collect (PROJECT_LIB_HEADERS xiltimer.h)
if (NOT ${YOCTO})
collect (PROJECT_LIB_HEADERS sleep.h)
endif()
collector_list (_sources PROJECT_LIB_SOURCES)
collector_list (_headers PROJECT_LIB_HEADERS)
# Zynq-7000 Cortex-A9 global timer backend — appended directly to _sources
# because collect() in subdirectories does not propagate when
# CMAKE_SYSTEM_PROCESSOR is unset in the gen_bsp cmake context (Vitis 2025.2).
# The IN_LIST guard prevents a duplicate if collect() happened to add it already.
set(_zynq_timer_src ${CMAKE_CURRENT_SOURCE_DIR}/core/default_timer/globaltimer_sleep_zynq.c)
if(NOT _zynq_timer_src IN_LIST _sources)
    list(APPEND _sources ${_zynq_timer_src})
endif()
file(COPY ${_headers} DESTINATION ${CMAKE_BINARY_DIR}/include)
if (NOT ${YOCTO})
file(COPY ${CMAKE_BINARY_DIR}/include/xtimer_config.h DESTINATION ${CMAKE_INCLUDE_PATH}/)
endif()
add_library(xiltimer STATIC ${_sources})
# Force XTIMER_IS_DEFAULT_TIMER so that globaltimer_sleep_zynq.c's #ifdef block
# compiles in XilSleepTimer_Init and XTime_GetTime for the Zynq-7000 Cortex-A9.
# xtimer_config.h leaves this macro as a comment (#undef is commented out), so
# the -D flag here is the authoritative definition and is not overridden.
target_compile_definitions(xiltimer PRIVATE XTIMER_IS_DEFAULT_TIMER=1)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
target_compile_options(xiltimer PRIVATE -iquote ${CMAKE_BINARY_DIR}/include)
endif()
set_target_properties(xiltimer PROPERTIES LINKER_LANGUAGE C)
get_headers(${_headers})
set_target_properties(xiltimer PROPERTIES ADDITIONAL_CLEAN_FILES "${CMAKE_LIBRARY_PATH}/libxiltimer.a;${CMAKE_INCLUDE_PATH}/xtimer_config.h;${clean_headers}")
install(TARGETS xiltimer LIBRARY DESTINATION ${CMAKE_LIBRARY_PATH} ARCHIVE DESTINATION ${CMAKE_LIBRARY_PATH})
install(DIRECTORY ${CMAKE_BINARY_DIR}/include DESTINATION ${CMAKE_INCLUDE_PATH}/..)
