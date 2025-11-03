#include <catch2/catch_test_macros.hpp>
#include "composite/core/component.hpp"
#include "composite/ports/input_port.hpp"
#include "composite/ports/output_port.hpp"
#include "composite/buffers/buffer.hpp"
#include <thread>

using namespace composite;

class SimpleTestSink : public component {
public:
    SimpleTestSink() : component("SimpleTestSink") {
        add_port(&m_input);
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
