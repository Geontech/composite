# Standalone build of the passthrough_gain example component.
#
# This is the canonical "out-of-tree component" build: find the installed (or build-tree) composite
# package and link the imported composite::composite target as a loadable MODULE. CI builds this
# against the freshly-built framework to keep the README's "Implementing a Component" example honest.
cmake_minimum_required(VERSION 3.20)
project(passthrough_gain LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(composite REQUIRED)

add_library(passthrough_gain MODULE component.cpp)   # -> libpassthrough_gain.so (dlopen'd at runtime)
target_link_libraries(passthrough_gain PRIVATE composite::composite)

include(GNUInstallDirs)
install(TARGETS passthrough_gain LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
