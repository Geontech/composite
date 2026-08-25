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

# KEEP assert() LIVE IN EVERY BUILD TYPE. 12 of these test files carry 123 assert() calls, and
# CMake's Release/RelWithDebInfo flags define NDEBUG -- so in any job that does not explicitly ask
# for Debug, all of them compiled to nothing and the tests ran without checking anything.
# build:options-matrix and both sanitizer jobs configure without CMAKE_BUILD_TYPE, which defaults
# to RelWithDebInfo: those runs were catching crashes and sanitizer reports (their main purpose)
# while validating no behaviour at all.
#
# -UNDEBUG is applied after CMAKE_CXX_FLAGS_<CONFIG>, so it wins. It also makes the
# unused-but-set-variable warnings honest: a `bool threw` that only feeds an assert() genuinely IS
# unused once the assert is gone, which is how this surfaced -- as -Werror failures the first time
# the tree was built in the environment it ships in.
add_compile_options(-UNDEBUG)

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
    # pool workers and the component worker must appear under readable names in /proc
    test_thread_name
    # make_component deleter (stop-before-destroy while the leaf is intact) + final lifecycle
    # entry points + connect-time mutability diagnostics
    test_make_component
    # snapshot<T>: atomically published config for non-parked threads (pool/receiver)
    test_snapshot
    # overlap_ring: lock-free overlapped framing ring lifted from the comps framer_pool (M6)
    test_overlap_ring
    # EAL-free DPDK port_registry bookkeeping (compiles + TSan-tests without libdpdk)
    test_dpdk_registry
    # Deterministic coverage for observer/enumeration synchronization: reentrant metric removal
    # during initial enumeration, and an observer retiring itself. Neither schedule is reached by
    # ordinary runs — a regression is a UAF or a hang, so this is worth pinning explicitly.
    test_registry_observer_sync
    # 0.5.1 regression: pattern flags in a component id must render verbatim, not be compiled
    # into the spdlog pattern (ids arrive from config files and POST /app/components)
    test_logger
    # EAL-arg logical->physical translation (--lcores rejection, guarded -l parsing);
    # cpu_affinity.cpp lives in libcomposite as of 0.5.1, so plain linking suffices
    test_cpu_affinity)
  composite_standalone_test(${t})
endforeach()

# OTLP instrument grouping. Only meaningful when the OTel bridge is compiled in, so it is
# conditional — but it IS a ctest when enabled, because the bug it guards against (N components
# collapsing into one exported series) is invisible without asserting on the series count.
if(COMPOSITE_USE_OPENTELEMETRY)
  composite_standalone_test(test_telemetry_grouping)
endif()

# Benchmarks (Baseline-0): built so they cannot rot, but NOT registered as ctests (timing
# harnesses, not pass/fail). The scheduled, non-gating `bench:baseline` CI job runs them with
# --json and uploads the artifacts; committed baselines live in benchmarks/baselines/. The
# table output makes before/after diffs obvious; the JSON carries raw samples + environment.
#
# UNDO the directory-wide -UNDEBUG for the benchmarks. That flag exists so the TESTS' assert()s
# stay live in every build type — but it also re-armed assertions inside the framework code the
# benchmarks measure, so a "Release" benchmark quietly timed a build with assertions live (the
# first committed baselines said so themselves: "assertions": "live"). Target options come
# after directory options on the compile line, so re-defining NDEBUG here wins; the generator
# expression keeps Debug builds honest (a Debug bench build still reports assertions live).
# The artifact's meta block records assertions/optimized, and bench:baseline verifies both.
set(COMPOSITE_BENCH_NDEBUG "$<$<NOT:$<CONFIG:Debug>>:-DNDEBUG>")

add_executable(bench_datapath bench_datapath.cpp)
target_include_directories(bench_datapath PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_options(bench_datapath PRIVATE ${COMPOSITE_BENCH_NDEBUG})
target_compile_definitions(bench_datapath PRIVATE BENCH_CMAKE_CONFIG="$<CONFIG>")
target_link_libraries(bench_datapath PRIVATE
  composite::composite composite_warnings spdlog::spdlog_header_only Threads::Threads)

# Registry profile: PERF-5's evidence (registration/snapshot/filter/teardown scaling) and the
# sizing input for the Prometheus exposition path.
add_executable(bench_registry bench_registry.cpp)
target_include_directories(bench_registry PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_options(bench_registry PRIVATE ${COMPOSITE_BENCH_NDEBUG})
target_compile_definitions(bench_registry PRIVATE BENCH_CMAKE_CONFIG="$<CONFIG>")
target_link_libraries(bench_registry PRIVATE
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
# The TLS cases need the same macro server.cpp is compiled with, plus OpenSSL for generating the
# self-signed material they hand-shake against. Without this the TLS half of the REST surface —
# which the published images now ship — has no test coverage at all.
if(COMPOSITE_USE_OPENSSL)
  target_compile_definitions(test_http_integration PRIVATE COMPOSITE_USE_OPENSSL=1)
  target_link_libraries(test_http_integration PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
target_link_libraries(test_http_integration PRIVATE
  composite::composite composite_warnings nlohmann_json::nlohmann_json spdlog::spdlog_header_only
  httplib::httplib Catch2::Catch2WithMain ${CMAKE_DL_LIBS})
add_test(NAME TestHttpIntegration COMMAND test_http_integration)

# Minimal loadable module (the suite's only dlopen'd component), for the POST /app/components
# creation-parity regression: initialize() and the applied log level are observable as
# properties, pinning REST-created == config-loaded behavior.
add_library(test_parity_module MODULE test_parity_module.cpp)
target_link_libraries(test_parity_module PRIVATE
  composite::composite composite_warnings spdlog::spdlog_header_only)
target_compile_definitions(test_http_integration PRIVATE
  TEST_PARITY_MODULE_PATH="$<TARGET_FILE:test_parity_module>")
add_dependencies(test_http_integration test_parity_module)

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
