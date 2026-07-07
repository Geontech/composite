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
    # Logging facade backend (keeps spdlog out of the public headers)
    ${PROJECT_SOURCE_DIR}/src/logger.cpp
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
    return()  # Skip rest of configuration for header-only
endif()

# Export symbols marked with COMPOSITE_API, hide all others
set_target_properties(composite PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON
    # SOVERSION = major.minor while 0.x (matches the SameMinorVersion package
    # compatibility); switch to PROJECT_VERSION_MAJOR at 1.0. Without this the
    # install tree shipped a bare libcomposite.so with SONAME libcomposite.so,
    # so every ABI-breaking minor bump silently overwrote the same SONAME.
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
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
)

# nlohmann_json is PUBLIC: it appears in public headers (the property/config JSON type), so its
# include path must propagate to consumers and it is recorded in the exported interface. It is a
# system find_package() (an IMPORTED target, safe to export); the installed compositeConfig.cmake
# find_dependency()s it so a consumer of <composite/composite.hpp> re-finds it. (Chosen contract:
# require-system nlohmann_json — see CMakeLists.txt.)
#
# spdlog is PRIVATE: it is hidden behind composite::logger (used only in logger.cpp), so it never
# appears in the public API. PRIVATE keeps it out of the exported interface, so install(EXPORT)
# does not require this (possibly-FetchContent, non-imported) target to be in an export set, and
# consumers need neither spdlog headers nor a find_dependency for it.
target_link_libraries(composite
    PUBLIC nlohmann_json::nlohmann_json
    PRIVATE spdlog::spdlog_header_only)

if(COMPOSITE_USE_DPDK)
    target_include_directories(composite PRIVATE ${DPDK_INCLUDE_DIRS})
    target_compile_definitions(composite PUBLIC COMPOSITE_USE_DPDK=1)
    target_link_libraries(composite PRIVATE ${DPDK_LIBRARIES})
    target_link_directories(composite PRIVATE ${DPDK_LIBRARY_DIRS})
    target_compile_options(composite PRIVATE ${DPDK_CFLAGS_OTHER})
endif()

add_library(composite::composite ALIAS composite)

# Threads are a usage requirement (registry mutexes; consumers using std::thread
# inherit it). composite_warnings applies the house warning flags to the
# library's own TUs only (INTERFACE compile-options, linked PRIVATE).
target_link_libraries(composite PUBLIC Threads::Threads PRIVATE composite_warnings)

if(COMPOSITE_USE_OPENTELEMETRY)
    target_compile_definitions(composite PUBLIC COMPOSITE_USE_OPENTELEMETRY=1)
    target_link_libraries(composite
        PUBLIC
        opentelemetry-cpp::api
        opentelemetry-cpp::sdk
        opentelemetry-cpp::otlp_http_metric_exporter
    )
endif()
