#
# Copyright (C) 2024 Geon Technologies, LLC
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

cmake_minimum_required(VERSION 3.15)
project(composite VERSION 0.1.0 LANGUAGES CXX)
include(GNUInstallDirs)
# Set the C++ version required
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Set compile flags
set(CMAKE_CXX_FLAGS_INIT "-Wall -Wextra -Wpedantic")
set(CMAKE_CXX_FLAGS_DEBUG_INIT "-g -ggdb -O0")
set(CMAKE_CXX_FLAGS_RELEASE_INIT "-O3")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Check if being used directly or via add_subdirectory, but allow overriding
if(NOT DEFINED COMPOSITE_MASTER_PROJECT)
    if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
        set(COMPOSITE_MASTER_PROJECT ON)
    else()
        set(COMPOSITE_MASTER_PROJECT OFF)
    endif()
endif()
option(COMPOSITE_INSTALL "Generate the install target" ${COMPOSITE_MASTER_PROJECT})

# Use FetchContent for dependencies
include(FetchContent)
# Argparse dependency
FetchContent_Declare(argparse
    GIT_REPOSITORY https://github.com/p-ranav/argparse.git
    GIT_TAG v3.0
)
# Fmt dependency
FetchContent_Declare(fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 10.2.1
)
# JSON dependency
FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
# spdlog dependency
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.14.1
)
set(FMT_INSTALL OFF)
# Make all deps available
FetchContent_MakeAvailable(argparse fmt json spdlog)

configure_file(include/version.hpp.in include/composite/version.hpp)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "compositeConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion
)
if(NOT INCLUDE_INSTALL_DIR)
    set(INCLUDE_INSTALL_DIR include)
endif()
configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/compositeConfig.cmake.in"
    "${PROJECT_BINARY_DIR}/compositeConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/composite/cmake
    PATH_VARS INCLUDE_INSTALL_DIR
)

# Create a pkg-config file, so other tools can find this.
CONFIGURE_FILE(
    "${PROJECT_SOURCE_DIR}/cmake/composite.pc.in"
    "${PROJECT_BINARY_DIR}/composite.pc"
)

add_subdirectory(include)
add_subdirectory(src)

# Install
if(COMPOSITE_INSTALL)
    install(
        DIRECTORY $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/composite>
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(
        FILES $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include/composite>
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(
        FILES
        "${PROJECT_BINARY_DIR}/compositeConfig.cmake"
        "${PROJECT_BINARY_DIR}/compositeConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/composite/cmake
    )
    install(
        TARGETS composite
        EXPORT composite_Targets
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(
        EXPORT composite_Targets
        FILE compositeTargets.cmake
        NAMESPACE composite::
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/composite/cmake
    )
    install(
        FILES "${PROJECT_BINARY_DIR}/composite.pc"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/pkgconfig
    )
    install(TARGETS composite-cli
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif(COMPOSITE_INSTALL)
