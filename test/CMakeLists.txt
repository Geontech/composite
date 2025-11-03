FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.8.1
)
FetchContent_MakeAvailable(Catch2)

##############################
# Properties Tests
##############################

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

##############################
# Ports Tests
##############################

add_executable(test_ports
  test_ports.cpp
)

target_link_libraries(test_ports
  PRIVATE
  composite::composite
  spdlog::spdlog_header_only
  Catch2::Catch2WithMain
)

add_test(NAME TestPorts COMMAND test_ports)
add_test(NAME TestPorts_Buffer COMMAND test_ports "[buffer]")
add_test(NAME TestPorts_Connection COMMAND test_ports "[connection]")
add_test(NAME TestPorts_Fanout COMMAND test_ports "[fanout]")
add_test(NAME TestPorts_Integration COMMAND test_ports "[integration]")
add_test(NAME TestPorts_Benchmark COMMAND test_ports "[.benchmark]")

##############################
# Lifecycle Tests
##############################

add_executable(test_lifecycle_simple
  test_lifecycle_simple.cpp
)

target_link_libraries(test_lifecycle_simple
  PRIVATE
  composite::composite
  spdlog::spdlog_header_only
  Catch2::Catch2WithMain
)

add_test(NAME TestLifecycleSimple COMMAND test_lifecycle_simple)

##############################
# Aligned Memory Tests
##############################

add_executable(test_aligned_mem
  test_aligned_mem.cpp
)

target_link_libraries(test_aligned_mem
  PRIVATE
  composite::composite
  spdlog::spdlog_header_only
  Catch2::Catch2WithMain
)

add_test(NAME TestAlignedMem COMMAND test_aligned_mem)
add_test(NAME TestAlignedMem_Basic COMMAND test_aligned_mem "[basic]")
add_test(NAME TestAlignedMem_Resize COMMAND test_aligned_mem "[resize]")
add_test(NAME TestAlignedMem_Integration COMMAND test_aligned_mem "[integration]")

##############################
# Transport Tests
##############################

add_executable(test_transports
  test_transports.cpp
  ${PROJECT_SOURCE_DIR}/src/helpers.cpp
)

target_include_directories(test_transports
  PRIVATE
  ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(test_transports
  PRIVATE
  composite::composite
  nlohmann_json::nlohmann_json
  spdlog::spdlog_header_only
  Catch2::Catch2WithMain
  dl
)

# Link NATS if enabled
if(COMPOSITE_USE_NATS)
  target_link_libraries(test_transports PRIVATE nats)
endif()

add_test(NAME TestTransports COMMAND test_transports)
add_test(NAME TestTransports_Enum COMMAND test_transports "[enum]")
add_test(NAME TestTransports_Parsing COMMAND test_transports "[parsing]")
add_test(NAME TestTransports_Creation COMMAND test_transports "[creation]")
add_test(NAME TestTransports_Attachment COMMAND test_transports "[attachment]")
add_test(NAME TestTransports_Integration COMMAND test_transports "[integration]")

##############################
# NATS Integration Tests
##############################

add_executable(test_nats_integration
  test_nats_integration.cpp
)

target_include_directories(test_nats_integration
  PRIVATE
  ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(test_nats_integration
  PRIVATE
  composite::composite
  spdlog::spdlog_header_only
  Catch2::Catch2WithMain
)

# Link NATS if enabled
if(COMPOSITE_USE_NATS)
  target_link_libraries(test_nats_integration PRIVATE nats)
endif()

# Note: These tests require a running NATS server at localhost:4222
# Start with: docker run -d -p 4222:4222 nats:latest
# The tests will gracefully skip if no server is available
add_test(NAME TestNATSIntegration COMMAND test_nats_integration "[nats][integration][.]")
add_test(NAME TestNATSIntegration_Transport COMMAND test_nats_integration "[nats][integration][transport][.]")
add_test(NAME TestNATSIntegration_Performance COMMAND test_nats_integration "[nats][integration][performance][.]")

add_test(NAME TestHttpIntegration COMMAND test_http_integration)
