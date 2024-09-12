cmake_minimum_required(VERSION 3.15)
project(caddie VERSION 0.1.0 LANGUAGES CXX)
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
if(NOT DEFINED CADDIE_MASTER_PROJECT)
    if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
        set(CADDIE_MASTER_PROJECT ON)
    else()
        set(CADDIE_MASTER_PROJECT OFF)
    endif()
endif()
option(CADDIE_INSTALL "Generate the install target" ${CADDIE_MASTER_PROJECT})

find_package(Threads REQUIRED)

# Use FetchContent for dependencies
include(FetchContent)
# JSON dependency
FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
# Argparse dependency
FetchContent_Declare(argparse
    GIT_REPOSITORY https://github.com/p-ranav/argparse.git
    GIT_TAG v3.1
)
# Make all deps available
FetchContent_MakeAvailable(json argparse)

configure_file(include/version.hpp.in include/caddie/version.hpp)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "caddieConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion
)
if(NOT INCLUDE_INSTALL_DIR)
    set(INCLUDE_INSTALL_DIR include)
endif()
configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/caddieConfig.cmake.in"
    "${PROJECT_BINARY_DIR}/caddieConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/caddie/cmake
    PATH_VARS INCLUDE_INSTALL_DIR
)

# Create a pkg-config file, so other tools can find this.
CONFIGURE_FILE(
    "${PROJECT_SOURCE_DIR}/cmake/caddie.pc.in"
    "${PROJECT_BINARY_DIR}/caddie.pc"
)

add_subdirectory(include)
add_subdirectory(src)

# Install
if(CADDIE_INSTALL)
    install(
        DIRECTORY $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/caddie>
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(
        FILES $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include/caddie>
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(
        FILES
        "${PROJECT_BINARY_DIR}/caddieConfig.cmake"
        "${PROJECT_BINARY_DIR}/caddieConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/caddie/cmake
    )
    install(
        TARGETS caddie
        EXPORT caddie_Targets
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(
        EXPORT caddie_Targets
        FILE caddieTargets.cmake
        NAMESPACE caddie::
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/caddie/cmake
    )
    install(
        FILES "${PROJECT_BINARY_DIR}/caddie.pc"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/pkgconfig
    )
    install(TARGETS caddie-cli
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif(CADDIE_INSTALL)
