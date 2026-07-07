// Metrics identity (label-only) + lifetime (RAII teardown) on the new model.
#include <composite/core/component.hpp>
#include <composite/metrics/registry.hpp>
#include <cassert>
#include <cstdio>
#include <memory>
#include <spdlog/spdlog.h>

using namespace composite;
namespace mx = composite::metrics;

class metric_comp : public component {
public:
    explicit metric_comp(std::string_view id) : component(id) {
        m_frames = &create_counter("frames_processed", "frames", "1");  // protected helper
    }
    auto process() -> retval override { return retval::FINISH; }
    void bump() { m_frames->inc(); }
    mx::counter<uint64_t>* m_frames{};
    component::auto_stop m_auto_stop{*this};  // MUST be last
};

int main() {
    spdlog::set_level(spdlog::level::off);
    auto& reg = mx::registry::instance();
    const auto base = reg.metric_count();
    {
        auto a = std::make_shared<metric_comp>("alpha");
        auto b = std::make_shared<metric_comp>("beta");

        // Label-only identity: one family NAME, two series distinguished by label
        // (the old design folded a sanitized id + hash into the name).
        auto by_name = reg.snapshot_by_prefix("frames_processed");
        assert(by_name.size() == 2);
        for (const auto& m : by_name) { assert(m.name == "frames_processed"); }

        // The same instance is sliceable by the component label (prefix and label agree).
        bool found_alpha = false;
        for (const auto& m : reg.snapshot_by_label("component_id", "alpha")) {
            if (m.name == "frames_processed") { found_alpha = true; }
        }
        assert(found_alpha);
        assert(reg.metric_count() > base);
    }
    // Both destroyed -> their metrics deregistered -> count returns to baseline.
    assert(reg.metric_count() == base);

    // Reload with the same id must not throw duplicate_metric_error (the old leak did).
    { auto a = std::make_shared<metric_comp>("alpha"); a->bump(); }
    assert(reg.metric_count() == base);

    std::printf("METRICS IDENTITY+LIFETIME OK (label-only series, teardown deregisters, reload-safe)\n");
    return 0;
}
