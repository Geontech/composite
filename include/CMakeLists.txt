add_library(composite INTERFACE)
add_library(composite::composite ALIAS composite)
target_compile_features(composite INTERFACE cxx_std_20)
target_include_directories(
    composite
    INTERFACE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
