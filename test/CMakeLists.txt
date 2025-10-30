FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.8.1
)
FetchContent_MakeAvailable(Catch2)

enable_testing()

add_executable(test_properties
  test_properties.cpp
  ${PROJECT_SOURCE_DIR}/src/helpers.cpp
  ${PROJECT_SOURCE_DIR}/src/property_rest_api.cpp
)

target_include_directories(test_properties
  PRIVATE
  ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(test_properties
  PRIVATE
  composite::composite
  nlohmann_json::nlohmann_json
  spdlog::spdlog_header_only
  httplib::httplib
  Catch2::Catch2WithMain
  dl
)

add_test(NAME TestProperties COMMAND test_properties)

# HTTP Integration Tests
add_executable(test_http_integration
  test_http_integration.cpp
  ${PROJECT_SOURCE_DIR}/src/helpers.cpp
  ${PROJECT_SOURCE_DIR}/src/property_rest_api.cpp
)

target_include_directories(test_http_integration
  PRIVATE
  ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(test_http_integration
  PRIVATE
  composite::composite
  nlohmann_json::nlohmann_json
  spdlog::spdlog_header_only
  httplib::httplib
  Catch2::Catch2WithMain
  dl
)

add_test(NAME TestHttpIntegration COMMAND test_http_integration)