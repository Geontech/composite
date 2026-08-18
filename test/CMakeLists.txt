#
# Copyright (C) 2024-2025 Geon Technologies, LLC
# SPDX-License-Identifier: LGPL-3.0-or-later
#

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.8.1
)
FetchContent_MakeAvailable(Catch2)

# Test-only API access (e.g., metrics::registry::clear)
add_compile_definitions(COMPOSITE_TESTING)

##############################
# Standalone tests (own main(), no Catch2)
##############################

# Linking composite::composite supplies the public include dirs AND the compiled
# registry singleton (registry.cpp); spdlog/Threads cover headers that need them.
# This is the CMake equivalent of the hand-rolled
#   g++ -std=c++20 -I include ... src/registry.cpp -pthread
# lines that previously lived ONLY in .gitlab-ci.yml — so `ctest` now actually
# runs the inverted-core concurrency suite. Before this, 13 of 22 test sources
# were orphaned from the build and a green ctest excluded the park / SPSC ring /
# component-concurrency / worker-self-write / slab_pool / histogram tests.
# Under the tsan / asan-ubsan presets these are instrumented automatically by
# the directory-scope sanitizer flags set in the top-level CMakeLists.
function(composite_standalone_test name)
  add_executable(${name} ${name}.cpp)
  target_include_directories(${name} PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src)
  target_link_libraries(${name} PRIVATE
    composite::composite
    composite_warnings
    nlohmann_json::nlohmann_json
    spdlog::spdlog_header_only
    Threads::Threads)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

foreach(t
    # property core (reflect / typed / property_set / config<T>)
    test_reflect test_reflect_strict test_fields_macro test_typed test_property_set
    test_config test_config_crux test_config_roundtrip test_config_loop_top
    # K1 regression: concurrent inline property writers on a STOPPED component (no worker)
    # must serialize their config<T> reaction drains under the park (TSan-clean)
    test_config_stopped_writers
    # K1-fix deadlock regression: a lifecycle-touching on_apply calling stop() during the inline
    # drain (EXITING state) must not spin forever on its own park in-flight guard
    test_reentrant_stop_on_apply
    # AF1/AF2 v0.5 freeze contract: a throwing on_apply / overflow callback must not unwind
    # across a worker, destructor, or graph-teardown boundary; depth() must not replace a
    # live ring; power-of-two rounding must saturate rather than spin
    test_failure_containment
    test_enabled_lifecycle
    # M6 lifecycle completion: on_finished / is_finished / wait_until_finished
    test_lifecycle_finish
    # M6 lifecycle EOS: end-of-stream marker propagation (at_end / send_eos / try_get)
    test_eos_propagation
    # P0 regression: a pipeline_component's pool must start via application::start()/reconcile
    test_pipeline_app_lifecycle
    # M6 lifecycle: source_component archetype + application::drain_stop
    test_source_component
    # M4 doorbell: producer wakes an idle consumer on the empty->non-empty edge
    test_doorbell
    # M4 reverse doorbell: consumer wakes a backpressured AWAIT_OUTPUT producer
    test_reverse_doorbell
    # connection-layer ownership: input deregisters from producer on destroy (UAF fix)
    test_connection_lifetime
    # inverted-core concurrency + buffers + metrics suite (previously orphaned)
    test_annotation test_application_registry test_buffer_ops
    test_component_concurrency test_connection_cow test_histogram
    test_metrics_lifecycle test_park_coordinator test_pipeline_component
    test_slab_pool test_spsc_ring test_worker_self_write
    # make_component deleter (stop-before-destroy while the leaf is intact) + final lifecycle
    # entry points + connect-time mutability diagnostics
    test_make_component
    # snapshot<T>: atomically published config for non-parked threads (pool/receiver)
    test_snapshot
    # overlap_ring: lock-free overlapped framing ring lifted from the comps framer_pool (M6)
    test_overlap_ring
    # EAL-free DPDK port_registry bookkeeping (compiles + TSan-tests without libdpdk)
    test_dpdk_registry)
  composite_standalone_test(${t})
endforeach()

# OTLP instrument grouping. Only meaningful when the OTel bridge is compiled in, so it is
# conditional — but it IS a ctest when enabled, because the bug it guards against (N components
# collapsing into one exported series) is invisible without asserting on the series count.
if(COMPOSITE_USE_OPENTELEMETRY)
  composite_standalone_test(test_telemetry_grouping)
endif()

# Data-path benchmark (Perf-0): built so it cannot rot, but NOT registered as a
# ctest (it is a timing harness, not pass/fail). Run from a scheduled,
# non-gating CI job; the table output makes before/after diffs obvious.
add_executable(bench_datapath bench_datapath.cpp)
target_include_directories(bench_datapath PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(bench_datapath PRIVATE
  composite::composite composite_warnings spdlog::spdlog_header_only Threads::Threads)

# Property tests on the new typed JSON model.
add_executable(test_properties test_properties.cpp)
target_include_directories(test_properties PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(test_properties PRIVATE
  composite::composite composite_warnings nlohmann_json::nlohmann_json spdlog::spdlog_header_only Catch2::Catch2WithMain ${CMAKE_DL_LIBS})
add_test(NAME TestProperties COMMAND test_properties)

# REST integration test drives the production make_server (server.cpp) over the
# new JSON property routes.
add_executable(test_http_integration
  test_http_integration.cpp
  ${PROJECT_SOURCE_DIR}/src/server.cpp
  ${PROJECT_SOURCE_DIR}/src/helpers.cpp)
target_include_directories(test_http_integration PRIVATE
  ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/src)
# Match the production server's pool size so the SSE-stream-cap test (8 held
# streams + a rejected 9th) has spare workers to serve the 9th request.
target_compile_definitions(test_http_integration PRIVATE CPPHTTPLIB_THREAD_POOL_COUNT=16)
target_link_libraries(test_http_integration PRIVATE
  composite::composite composite_warnings nlohmann_json::nlohmann_json spdlog::spdlog_header_only
  httplib::httplib Catch2::Catch2WithMain ${CMAKE_DL_LIBS})
add_test(NAME TestHttpIntegration COMMAND test_http_integration)

##############################
# Ports Tests
##############################

add_executable(test_ports
  test_ports.cpp
)

target_link_libraries(test_ports
  PRIVATE
  composite::composite
  composite_warnings
  spdlog::spdlog_header_only
  Catch2::Catch2WithMain
)

add_test(NAME TestPorts COMMAND test_ports)
add_test(NAME TestPorts_Buffer COMMAND test_ports "[buffer]")
add_test(NAME TestPorts_Connection COMMAND test_ports "[connection]")
add_test(NAME TestPorts_Fanout COMMAND test_ports "[fanout]")
add_test(NAME TestPorts_Integration COMMAND test_ports "[integration]")
# add_test(NAME TestPorts_Benchmark COMMAND test_ports "[.benchmark]" "--benchmark-samples" "5")

##############################
# Lifecycle Tests
##############################

add_executable(test_lifecycle_simple
  test_lifecycle_simple.cpp
)

target_link_libraries(test_lifecycle_simple
  PRIVATE
  composite::composite
  composite_warnings
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
  composite_warnings
  spdlog::spdlog_header_only
  Catch2::Catch2WithMain
)

add_test(NAME TestAlignedMem COMMAND test_aligned_mem)
add_test(NAME TestAlignedMem_Basic COMMAND test_aligned_mem "[basic]")
add_test(NAME TestAlignedMem_Resize COMMAND test_aligned_mem "[resize]")
add_test(NAME TestAlignedMem_Integration COMMAND test_aligned_mem "[integration]")

##############################
# Metrics Tests
##############################

add_executable(test_metrics
  test_metrics.cpp
)

target_link_libraries(test_metrics
  PRIVATE
  composite::composite
  composite_warnings
  spdlog::spdlog_header_only
  Catch2::Catch2WithMain
)

add_test(NAME TestMetrics COMMAND test_metrics)
add_test(NAME TestMetrics_Counter COMMAND test_metrics "[counter]")
add_test(NAME TestMetrics_UpDownCounter COMMAND test_metrics "[updown_counter]")
add_test(NAME TestMetrics_Gauge COMMAND test_metrics "[gauge]")
add_test(NAME TestMetrics_Histogram COMMAND test_metrics "[histogram]")
add_test(NAME TestMetrics_Registry COMMAND test_metrics "[registry]")
add_test(NAME TestMetrics_Threading COMMAND test_metrics "[threading]")
