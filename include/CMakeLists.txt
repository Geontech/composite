#
# Copyright (C) 2024-2025 Geon Technologies, LLC
# SPDX-License-Identifier: LGPL-3.0-or-later
#

# Create composite library
# SHARED library to support compiled code and ensure singleton instances
# are shared across all DSOs (dynamically loaded component libraries)
set(COMPOSITE_SOURCES
    # Always include registry.cpp - required for DSO-safe singleton
    ${PROJECT_SOURCE_DIR}/src/registry.cpp
)

if(COMPOSITE_USE_DPDK)
    list(APPEND COMPOSITE_SOURCES
        ${PROJECT_SOURCE_DIR}/src/dpdk_manager.cpp
    )
endif()

if(COMPOSITE_USE_OPENTELEMETRY)
    list(APPEND COMPOSITE_SOURCES
        ${PROJECT_SOURCE_DIR}/src/telemetry_manager.cpp
    )
endif()

if(COMPOSITE_SOURCES)
    add_library(composite SHARED ${COMPOSITE_SOURCES})
else()
    # Header-only if no compiled sources needed
    add_library(composite INTERFACE)
    target_compile_features(composite INTERFACE cxx_std_20)
    target_include_directories(
        composite
        INTERFACE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
    add_library(composite::composite ALIAS composite)
    if(COMPOSITE_USE_NATS)
        target_compile_definitions(composite INTERFACE COMPOSITE_USE_NATS=1)
        target_link_libraries(composite INTERFACE nats)
    endif()
    return()  # Skip rest of configuration for header-only
endif()

# Export symbols marked with COMPOSITE_API, hide all others
set_target_properties(composite PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON
)

# Define COMPOSITE_EXPORTS when building the library (enables __declspec(dllexport) on Windows)
target_compile_definitions(composite PRIVATE COMPOSITE_EXPORTS)

target_compile_features(composite PUBLIC cxx_std_20)
target_include_directories(
    composite
    PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
    $<BUILD_INTERFACE:${spdlog_SOURCE_DIR}/include>
)

if(COMPOSITE_USE_DPDK)
    target_include_directories(composite PRIVATE ${DPDK_INCLUDE_DIRS})
    target_compile_definitions(composite PUBLIC COMPOSITE_USE_DPDK=1)
    target_link_libraries(composite PRIVATE ${DPDK_LIBRARIES})
    target_link_directories(composite PRIVATE ${DPDK_LIBRARY_DIRS})
    target_compile_options(composite PRIVATE ${DPDK_CFLAGS_OTHER})
endif()

add_library(composite::composite ALIAS composite)

if(COMPOSITE_USE_NATS)
    target_compile_definitions(composite PUBLIC COMPOSITE_USE_NATS=1)
    target_link_libraries(composite PUBLIC nats)
endif()

if(COMPOSITE_USE_OPENTELEMETRY)
    target_compile_definitions(composite PUBLIC COMPOSITE_USE_OPENTELEMETRY=1)
    target_link_libraries(composite
        PUBLIC
        opentelemetry-cpp::api
        opentelemetry-cpp::sdk
        opentelemetry-cpp::otlp_http_metric_exporter
    )
endif()
