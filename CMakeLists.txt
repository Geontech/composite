#
# Copyright (C) 2024-2025 Geon Technologies, LLC
# SPDX-License-Identifier: LGPL-3.0-or-later
#

cmake_minimum_required(VERSION 3.15)
project(composite VERSION 0.6.0 LANGUAGES CXX)
include(GNUInstallDirs)
# Set the C++ version required
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# NOTE: warning flags are applied via the composite_warnings INTERFACE target
# created below (see "Warning + sanitizer plumbing"). The previous
# CMAKE_CXX_FLAGS_*_INIT lines here were inert: *_INIT variables are only
# consulted when a language is first enabled (by project()), which already ran
# on line 7 above, so they never reached the compiler. Build-type optimization
# flags (-g for Debug, -O3 -DNDEBUG for Release) come from CMAKE_BUILD_TYPE.

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Threads are used by the library (registry mutexes) and every concurrency test;
# rely on the imported target rather than implicit -pthread.
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# Check if being used directly or via add_subdirectory, but allow overriding
if(NOT DEFINED COMPOSITE_MASTER_PROJECT)
    if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
        set(COMPOSITE_MASTER_PROJECT ON)
    else()
        set(COMPOSITE_MASTER_PROJECT OFF)
    endif()
endif()
option(COMPOSITE_INSTALL "Generate the install target" ${COMPOSITE_MASTER_PROJECT})

# Default a single-config master build to RelWithDebInfo so a bare
# `cmake -B build` (no preset) isn't an unoptimized, assertion-defining build.
get_property(_composite_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(COMPOSITE_MASTER_PROJECT AND NOT _composite_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    message(STATUS "composite: CMAKE_BUILD_TYPE not set; defaulting to RelWithDebInfo")
endif()

# Use FetchContent for dependencies
include(FetchContent)
# Argparse dependency
FetchContent_Declare(argparse
    GIT_REPOSITORY https://github.com/p-ranav/argparse.git
    GIT_TAG v3.0
)
# nlohmann_json is a PUBLIC dependency: it appears in public headers (the property/config JSON
# type) and the exported interface, so it is REQUIRED from the system and find_dependency()'d by
# the installed compositeConfig.cmake — a consumer of <composite/composite.hpp> must have it too.
# spdlog is a PRIVATE implementation dependency (hidden behind composite::logger, used only in
# logger.cpp), so it is fetched if not installed and never appears in the public API.
#
# NOTE: we use find_package() + a guarded FetchContent_MakeAvailable rather than
# FetchContent_Declare(... FIND_PACKAGE_ARGS ...), because that keyword requires CMake >= 3.24 and
# this project's floor is 3.15/3.22 (on 3.22 it mis-parses into a broken `git clone --origin
# FIND_PACKAGE_ARGS ... ""`). Set FETCHCONTENT_SOURCE_DIR_SPDLOG to build spdlog fully offline.
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.14.1
)
# httplib dependency
FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.18.0
)
set(HTTPLIB_INSTALL OFF)
find_package(nlohmann_json 3.11.3 REQUIRED)   # PUBLIC dep — required (system), exported
find_package(spdlog 1.14.1 QUIET)             # PRIVATE impl dep — fetch if absent
FetchContent_MakeAvailable(argparse)
if(NOT spdlog_FOUND)
    FetchContent_MakeAvailable(spdlog)
endif()

# Options for enabling target RUNPATH
option(COMPOSITE_EMBED_MODULE_RPATH "Enable embedding a RUNPATH in composite-cli to find modules" ON)
set(DEFAULT_MODULE_RPATH "$ORIGIN/../lib64" CACHE INTERNAL "Default RPATH for modules")
# Define the user-configurable RUNPATH string variable
set(COMPOSITE_MODULE_RPATH_VALUE "${DEFAULT_MODULE_RPATH}" CACHE STRING
    "The RUNPATH string to embed (e.g., '$ORIGIN/../lib' or '/opt/myapp/lib'). Used only if COMPOSITE_EMBED_MODULE_RPATH is ON.")

# DPDK support
option(COMPOSITE_USE_DPDK "Compile with DPDK support" OFF)
if(COMPOSITE_USE_DPDK)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DPDK REQUIRED libdpdk)
    message(STATUS "Found DPDK: ${DPDK_VERSION}")
endif()

# OpenTelemetry support
option(COMPOSITE_USE_OPENTELEMETRY "Compile with OpenTelemetry support" OFF)
if(COMPOSITE_USE_OPENTELEMETRY)
    # A version argument on find_package() would be a PIN here, not a floor: opentelemetry-cpp's
    # package config declares same-minor compatibility, so `find_package(opentelemetry-cpp 1.19)`
    # actively REJECTS 1.22. Compare the version ourselves to get the floor we actually want.
    find_package(opentelemetry-cpp REQUIRED)
    # 1.19 is the oldest release this compiles against — see the nostd::shared_ptr note in
    # telemetry_manager.cpp. CI and the published images both use the single revision pinned in
    # docker/Dockerfile.rocky9, so there is no longer a tested-vs-shipped split; this floor exists
    # for anyone building against a distro package, and fails at configure rather than at a call
    # site that happens to touch a newer API.
    if(opentelemetry-cpp_VERSION VERSION_LESS 1.19)
        message(FATAL_ERROR
                "opentelemetry-cpp ${opentelemetry-cpp_VERSION} is too old; 1.19 or newer required "
                "(CI and the published images use the revision pinned in docker/Dockerfile.rocky9)")
    endif()
    message(STATUS "Found OpenTelemetry: ${opentelemetry-cpp_VERSION}")
endif()

# OpenSSL support
option(COMPOSITE_USE_OPENSSL "Compile with OpenSSL support" OFF)
if(COMPOSITE_USE_OPENSSL)
    # OpenSSL 3 required
    find_package(OpenSSL 3 REQUIRED)
    set(HTTPLIB_REQUIRE_OPENSSL ON)
endif()
FetchContent_MakeAvailable(httplib)

# ---------------------------------------------------------------------------
# Warning + sanitizer plumbing
#
# composite_warnings is an INTERFACE target carrying the house warning flags.
# It is linked PRIVATE to composite's own targets (library, CLI, tests) only —
# never to FetchContent dependencies — so a future -Werror cannot be tripped by
# third-party code. The flags are populated only for a master-project build so
# downstream add_subdirectory() consumers are not forced into our warning level.
#
# COMPOSITE_WERROR (default OFF) promotes warnings to errors; CI turns it on.
# Kept opt-in because the tree is not yet warning-clean under -Wpedantic and the
# plan ratchets -Werror on per-directory as code is cleaned.
# ---------------------------------------------------------------------------
option(COMPOSITE_WERROR "Treat compiler warnings as errors (CI)" OFF)
add_library(composite_warnings INTERFACE)
if(COMPOSITE_MASTER_PROJECT)
    target_compile_options(composite_warnings INTERFACE
        $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>)
    if(COMPOSITE_WERROR)
        target_compile_options(composite_warnings INTERFACE
            $<$<COMPILE_LANGUAGE:CXX>:-Werror>)
    endif()
endif()

# Sanitizers: COMPOSITE_SANITIZER holds the -fsanitize argument, e.g. "thread"
# or "address,undefined" (empty = off). Applied at directory scope to every
# target defined below (library + CLI + tests, plus Catch2 which is fetched in
# test/) so all linked TUs are instrumented consistently. The pre-configured
# header deps above are unaffected. This replaces the hand-rolled `g++
# -fsanitize=...` lines in .gitlab-ci.yml with one source of truth (the tsan /
# asan-ubsan presets in CMakePresets.json).
set(COMPOSITE_SANITIZER "" CACHE STRING
    "Sanitizer to build with: '' (off), 'thread', 'address,undefined'")
if(COMPOSITE_SANITIZER)
    message(STATUS "composite: building with -fsanitize=${COMPOSITE_SANITIZER}")
    add_compile_options(-fsanitize=${COMPOSITE_SANITIZER} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${COMPOSITE_SANITIZER})
endif()

# Add subdirectories
add_subdirectory(include)
add_subdirectory(src)

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
# When built with OTel, the composite target PUBLIC-links opentelemetry-cpp::*,
# so the installed config must find_dependency it (else a downstream
# find_package(composite) fails at generate time on the dangling imported target).
if(COMPOSITE_USE_OPENTELEMETRY)
    set(COMPOSITE_CONFIG_OTEL_DEP "find_dependency(opentelemetry-cpp)")
else()
    set(COMPOSITE_CONFIG_OTEL_DEP "")
endif()
# INSTALL-tree config -> a NON-searchable build subdir (install-cmake/), NOT the build root. This
# config is install-relative: it includes the install-only compositeTargets.cmake and resolves
# include paths against the install prefix, so it only works once installed. Writing it to the build
# root made find_package(composite) pointed at the BUILD tree discover a config that then failed
# (no compositeTargets.cmake there, and the prefix math is off by the install-dir depth). It is
# installed to share/composite/cmake below; the build root instead gets a working build-tree config
# (the export() + compositeBuildConfig further down).
configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/compositeConfig.cmake.in"
    "${PROJECT_BINARY_DIR}/install-cmake/compositeConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/composite/cmake
    PATH_VARS INCLUDE_INSTALL_DIR
)

# Create a pkg-config file, so other tools can find this. @ONLY so the pc-file's
# own ${prefix}/${pcfiledir}/${libdir} runtime variables survive configure.
CONFIGURE_FILE(
    "${PROJECT_SOURCE_DIR}/cmake/composite.pc.in"
    "${PROJECT_BINARY_DIR}/composite.pc"
    @ONLY
)

# Install
if(COMPOSITE_INSTALL)
    # Source headers. (A $<BUILD_INTERFACE:...> wrapper here would expand to an
    # empty string at install time — install paths are not target usage
    # requirements — so the headers must be referenced by plain path.)
    install(
        DIRECTORY ${PROJECT_SOURCE_DIR}/include/composite
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        COMPONENT Development
    )
    # Generated headers (e.g. composite/version.hpp under the build tree).
    install(
        DIRECTORY ${PROJECT_BINARY_DIR}/include/composite
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        COMPONENT Development
    )
    install(
        FILES
        "${PROJECT_BINARY_DIR}/install-cmake/compositeConfig.cmake"
        "${PROJECT_BINARY_DIR}/compositeConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/composite/cmake
        COMPONENT Development
    )
    install(
        TARGETS composite
        EXPORT composite_Targets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            COMPONENT Runtime
            NAMELINK_COMPONENT Development
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            COMPONENT Runtime
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            COMPONENT Development
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
    install(
        EXPORT composite_Targets
        FILE compositeTargets.cmake
        NAMESPACE composite::
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/composite/cmake
        COMPONENT Development
    )
    # BUILD-tree package: make find_package(composite) work against the build tree (point
    # CMAKE_PREFIX_PATH at this build dir) without an install step. export() writes a build-tree
    # compositeTargets.cmake carrying the BUILD_INTERFACE include dirs and the just-built
    # libcomposite; compositeBuildConfig (a deps + targets-include shim, no install-relative paths)
    # lands in the build root as compositeConfig.cmake. The version file already sits in the build
    # root and is tree-agnostic, so build-tree find_package gets Config + ConfigVersion + Targets.
    export(
        EXPORT composite_Targets
        NAMESPACE composite::
        FILE "${PROJECT_BINARY_DIR}/compositeTargets.cmake"
    )
    configure_file(
        "${PROJECT_SOURCE_DIR}/cmake/compositeBuildConfig.cmake.in"
        "${PROJECT_BINARY_DIR}/compositeConfig.cmake"
        @ONLY
    )
    # Arch-dependent .pc (it carries Libs: -L.../lib64) belongs in
    # libdir/pkgconfig, not the arch-independent share/pkgconfig.
    install(
        FILES "${PROJECT_BINARY_DIR}/composite.pc"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
        COMPONENT Development
    )
    install(TARGETS composite-cli
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        COMPONENT Runtime
    )
endif(COMPOSITE_INSTALL)

# Tests
option(COMPOSITE_BUILD_TESTS "Build unit tests" OFF)
if (COMPOSITE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()
