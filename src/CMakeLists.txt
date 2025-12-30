#
# Copyright (C) 2024-2025 Geon Technologies, LLC
# SPDX-License-Identifier: LGPL-3.0-or-later
#

# Executable
add_executable(composite-cli
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/server.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/helpers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/cpu_affinity.cpp
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
target_compile_definitions(composite-cli PRIVATE CPPHTTPLIB_THREAD_POOL_COUNT=2)
if(COMPOSITE_USE_DPDK)
    target_compile_definitions(composite-cli PRIVATE COMPOSITE_USE_DPDK=1)
endif()
if(COMPOSITE_USE_OPENSSL)
    target_compile_definitions(composite-cli PRIVATE COMPOSITE_USE_OPENSSL=1)
endif()
if(COMPOSITE_USE_NATS)
    target_compile_definitions(composite-cli PRIVATE COMPOSITE_USE_NATS=1)
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
    argparse
    nlohmann_json::nlohmann_json
    spdlog::spdlog_header_only
    httplib::httplib
    dl
)
if(COMPOSITE_USE_NATS)
    target_link_libraries(composite-cli
        PRIVATE
        nats
    )
endif()
if(COMPOSITE_USE_DPDK)
    target_link_libraries(composite-cli
        PRIVATE
        ${DPDK_LIBRARIES}
    )
endif()
