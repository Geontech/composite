#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "composite/core/component.hpp"
#include "composite/metrics/metrics.hpp"
#include "composite/ports/input_port.hpp"
#include "composite/ports/output_port.hpp"
#include "composite/buffers/buffer.hpp"
#include <set>
#include <thread>

using namespace composite;

class SimpleTestSink : public component {
public:
    SimpleTestSink() : component("SimpleTestSink") {
        add_port(m_input);
        m_input.depth(100);
    }

    auto process() -> retval override {
        return retval::NOOP;
    }

    auto get_input_depth() const -> std::size_t {
        return m_input.depth();
    }

private:
    input_port<immutable_buffer<float>> m_input{"data_in"};
};

TEST_CASE("Component enabled property pauses input ports", "[lifecycle]") {
    auto sink = std::make_shared<SimpleTestSink>();
    
    // Verify initial state
    REQUIRE(sink->get_property<bool>("enabled") == true);
    REQUIRE(sink->get_input_depth() == 100);
    
    // Start component
    sink->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Disable component via property
    sink->set_properties({{"enabled", "false"}});
    sink->apply_lifecycle_changes();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Verify component stopped and input port paused
    REQUIRE(sink->get_property<bool>("enabled") == false);
    REQUIRE(sink->get_input_depth() == 0);  // Should be paused
    
    // Cleanup
    sink->stop();
}

// ============================================================================
// Component Lifecycle Metrics Tests
// ============================================================================

class MetricsTestComponent : public component {
public:
    explicit MetricsTestComponent(std::string_view id) : component(id) {}

    auto process() -> retval override {
        ++m_process_count;
        if (m_should_noop) {
            return retval::NOOP;
        }
        // Do a tiny bit of work
        volatile int x = 0;
        for (int i = 0; i < 100; ++i) { x += i; }
        return retval::NORMAL;
    }

    auto set_noop(bool noop) -> void { m_should_noop = noop; }
    auto process_count() const -> int { return m_process_count; }

private:
    std::atomic<int> m_process_count{0};
    bool m_should_noop{false};
};

TEST_CASE("Component lifecycle metrics are registered", "[lifecycle][metrics]") {
    auto comp = std::make_shared<MetricsTestComponent>("metrics_test_comp");

    // Verify metrics are registered with correct labels
    auto snapshots = metrics::registry::instance().snapshot_by_label("component_id", "metrics_test_comp");

    // Should have 4 metrics: process_calls, noop_count, process_time, state
    REQUIRE(snapshots.size() == 4);

    // Verify metric names
    std::set<std::string> metric_names;
    for (const auto& snap : snapshots) {
        metric_names.insert(snap.name);
    }

    REQUIRE(metric_names.contains("composite.component.process_calls"));
    REQUIRE(metric_names.contains("composite.component.noop_count"));
    REQUIRE(metric_names.contains("composite.component.process_time"));
    REQUIRE(metric_names.contains("composite.component.state"));

    // Destroy component before cleaning up metrics (component destructor accesses metrics)
    comp.reset();
    metrics::registry::instance().remove_by_label("component_id", "metrics_test_comp");
}

TEST_CASE("Component state metric updates on start/stop", "[lifecycle][metrics]") {
    auto comp = std::make_shared<MetricsTestComponent>("state_test_comp");

    // Get the state metric
    auto get_state = [&]() -> double {
        auto snapshots = metrics::registry::instance().snapshot_by_label("component_id", "state_test_comp");
        for (const auto& snap : snapshots) {
            if (snap.name == "composite.component.state") {
                return std::get<double>(snap.value);
            }
        }
        return -1.0;
    };

    // Initial state should be 0 (stopped)
    REQUIRE(get_state() == Catch::Approx(0.0));

    // Start component
    comp->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // State should be 1 (running)
    REQUIRE(get_state() == Catch::Approx(1.0));

    // Stop component
    comp->stop();

    // State should be 0 (stopped)
    REQUIRE(get_state() == Catch::Approx(0.0));

    // Destroy component before cleaning up metrics (component destructor accesses metrics)
    comp.reset();
    metrics::registry::instance().remove_by_label("component_id", "state_test_comp");
}

TEST_CASE("Component process metrics are recorded", "[lifecycle][metrics]") {
    auto comp = std::make_shared<MetricsTestComponent>("process_test_comp");

    // Helper to get metric values
    auto get_counter = [&](const std::string& name) -> uint64_t {
        auto snapshots = metrics::registry::instance().snapshot_by_label("component_id", "process_test_comp");
        for (const auto& snap : snapshots) {
            if (snap.name == name) {
                return std::get<uint64_t>(snap.value);
            }
        }
        return 0;
    };

    auto get_histogram_count = [&]() -> uint64_t {
        auto snapshots = metrics::registry::instance().snapshot_by_label("component_id", "process_test_comp");
        for (const auto& snap : snapshots) {
            if (snap.name == "composite.component.process_time") {
                return std::get<metrics::histogram_snapshot>(snap.value).count;
            }
        }
        return 0;
    };

    // Initial values should be 0
    REQUIRE(get_counter("composite.component.process_calls") == 0);
    REQUIRE(get_counter("composite.component.noop_count") == 0);
    REQUIRE(get_histogram_count() == 0);

    // Start component and let it run
    comp->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    comp->stop();

    // Should have some process calls
    auto process_calls = get_counter("composite.component.process_calls");
    REQUIRE(process_calls > 0);

    // Histogram should have same count as process calls
    REQUIRE(get_histogram_count() == process_calls);

    // noop_count should be 0 since we returned NORMAL
    REQUIRE(get_counter("composite.component.noop_count") == 0);

    // Destroy component before cleaning up metrics (component destructor accesses metrics)
    comp.reset();
    metrics::registry::instance().remove_by_label("component_id", "process_test_comp");
}

TEST_CASE("Component NOOP count is tracked", "[lifecycle][metrics]") {
    auto comp = std::make_shared<MetricsTestComponent>("noop_test_comp");
    comp->set_noop(true);  // Make process() return NOOP

    auto get_noop_count = [&]() -> uint64_t {
        auto snapshots = metrics::registry::instance().snapshot_by_label("component_id", "noop_test_comp");
        for (const auto& snap : snapshots) {
            if (snap.name == "composite.component.noop_count") {
                return std::get<uint64_t>(snap.value);
            }
        }
        return 0;
    };

    // Start component and let it run (NOOPs have a delay so won't be many)
    comp->start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    comp->stop();

    // Should have some NOOP counts
    REQUIRE(get_noop_count() > 0);

    // Destroy component before cleaning up metrics (component destructor accesses metrics)
    comp.reset();
    metrics::registry::instance().remove_by_label("component_id", "noop_test_comp");
}
