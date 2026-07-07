#include "component.hpp"

#include <composite/core/register.hpp>

passthrough_gain::passthrough_gain(std::string_view id) : composite::component(id) {
    using enum composite::properties::config_type;
    add_port(&m_in);
    add_port(&m_out);
    add_property("gain", m_gain, RUNTIME)
        .units("factor")
        .validate([](const float& g) { return g >= 0.0F; }, "gain must be >= 0");
}

auto passthrough_gain::process() -> composite::retval {
    using enum composite::retval;

    // try_get() is the canonical read: nullopt = empty ring (distinct from a genuine zero-length
    // packet, which get_data() cannot tell apart). Returning NOOP idles on the doorbell; when the
    // upstream closes and this ring drains, the base auto-FINISHes and forwards EOS — no manual
    // end-of-stream handling needed. (Override on_end_of_stream() only if you hold buffered state.)
    auto pkt = m_in.try_get();
    if (!pkt) { return NOOP; }
    auto& [buffer, ts, metadata] = *pkt;

    for (std::size_t i = 0; i < buffer.size(); ++i) { buffer[i] *= m_gain; }

    // Metadata (if any) rides atomically with the packet as the optional third argument.
    m_out.send_data(std::move(buffer), ts, metadata);
    return NORMAL;
}

// Emits the create(id, args) ABI + the composite_abi_version() handshake.
COMPOSITE_REGISTER_SIMPLE(passthrough_gain)
