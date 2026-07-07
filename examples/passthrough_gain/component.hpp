// passthrough_gain — the minimal example component from the README's "Implementing a Component"
// section. It reads float buffers from an input port, multiplies each sample by a runtime-tunable
// gain, and forwards the (moved) buffer — carrying any per-packet metadata — to its output port.
#pragma once

#include <composite/core/component.hpp>

class passthrough_gain : public composite::component {
public:
    explicit passthrough_gain(std::string_view id);
    ~passthrough_gain() override = default;
    auto process() -> composite::retval override;

private:
    composite::input_port<composite::mutable_buffer<float>>  m_in{"data_in"};
    composite::output_port<composite::mutable_buffer<float>> m_out{"data_out"};
    float m_gain{1.0F};

    // MUST be the last data member: its destructor stops the worker first, while the
    // members process() touches are still alive (otherwise: destruction-order use-after-free).
    composite::component::auto_stop m_auto_stop{*this};
};
