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
)

target_link_libraries(test_properties
  PRIVATE
  composite::composite
  nlohmann_json::nlohmann_json
  spdlog::spdlog_header_only
  fmt::fmt
  httplib::httplib
  Catch2::Catch2WithMain
  dl
)

add_test(NAME TestProperties COMMAND test_properties)
