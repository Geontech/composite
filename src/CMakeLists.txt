#
# Copyright (C) 2024-2025 Geon Technologies, LLC
# SPDX-License-Identifier: LGPL-3.0-or-later
#

# Executable
# cpu_affinity.cpp moved into libcomposite (its declarations live in an installed header);
# the CLI gets it by linking composite::composite like any other consumer.
add_executable(composite-cli
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/server.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/helpers.cpp
)
# Set RPATH property
if(COMPOSITE_EMBED_MODULE_RPATH)
    set_target_properties(composite-cli PROPERTIES
        INSTALL_RPATH "${COMPOSITE_MODULE_RPATH_VALUE}"
        BUILD_WITH_INSTALL_RPATH ON
        SKIP_BUILD_RPATH OFF
    )
    message(STATUS "composite-cli: Enabled embedded RUNPATH: ${COMPOSITE_MODULE_RPATH_VALUE}")
else()
    message(STATUS "composite-cli: Disabled embedded RUNPATH.")
endif()
# Compile definitions
# httplib serves each request (and each long-lived SSE stream) on a pooled worker
# thread. The pool must comfortably exceed the SSE stream cap (8, see server.cpp)
# so streams cannot starve ordinary control-plane requests.
target_compile_definitions(composite-cli PRIVATE CPPHTTPLIB_THREAD_POOL_COUNT=16)
if(COMPOSITE_USE_DPDK)
    target_compile_definitions(composite-cli PRIVATE COMPOSITE_USE_DPDK=1)
endif()
if(COMPOSITE_USE_OPENSSL)
    target_compile_definitions(composite-cli PRIVATE COMPOSITE_USE_OPENSSL=1)
endif()
if(COMPOSITE_USE_OPENTELEMETRY)
    target_compile_definitions(composite-cli PRIVATE COMPOSITE_USE_OPENTELEMETRY=1)
endif()
# Includes
target_include_directories(composite-cli
    PRIVATE ${PROJECT_SOURCE_DIR}/include
    PRIVATE ${PROJECT_BINARY_DIR}/include
)
# Linkage
target_link_libraries(composite-cli
    PRIVATE
    composite::composite
    composite_warnings
    argparse
    nlohmann_json::nlohmann_json
    spdlog::spdlog_header_only
    httplib::httplib
    Threads::Threads
    ${CMAKE_DL_LIBS}   # 'dl' on glibc; empty where dlopen is in libc (musl/macOS/BSD)
)
if(COMPOSITE_USE_DPDK)
    target_link_libraries(composite-cli
        PRIVATE
        ${DPDK_LIBRARIES}
    )
endif()
if(COMPOSITE_USE_OPENTELEMETRY)
    target_link_libraries(composite-cli
        PRIVATE
        opentelemetry-cpp::api
        opentelemetry-cpp::sdk
        opentelemetry-cpp::otlp_http_exporter
    )
endif()
