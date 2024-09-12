add_library(caddie INTERFACE)
add_library(caddie::caddie ALIAS caddie)
target_compile_features(caddie INTERFACE cxx_std_20)
target_include_directories(
    caddie
    INTERFACE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
