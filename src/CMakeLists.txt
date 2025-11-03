#
# Copyright (C) 2024-2025 Geon Technologies, LLC
#
# This file is part of composite.
#
# composite is free software: you can redistribute it and/or modify it under the
# terms of the GNU Lesser General Public License as published by the Free
# Software Foundation, either version 3 of the License, or (at your option)
# any later version.
#
# composite is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
# more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with this program.  If not, see http://www.gnu.org/licenses/.
#
 
# Executable
add_executable(composite-cli
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/server.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/helpers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/property_rest_api.cpp
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
