# Executable
add_executable(caddie-cli
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
)
# Includes
target_include_directories(caddie-cli
    PRIVATE ${CMAKE_SOURCE_DIR}/include
    PRIVATE ${CMAKE_BINARY_DIR}/include
)
# Linkage
target_link_libraries(caddie-cli
    PRIVATE
    argparse
    fmt::fmt-header-only
    nlohmann_json::nlohmann_json
    spdlog::spdlog_header_only
    dl
)
