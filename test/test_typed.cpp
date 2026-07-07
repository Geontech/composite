// typed_property + keyed_collection: candidate->validate->swap->diff->notify,
// no rollback; channelizer keyed collection with cross-element invariant.
#include <cassert>
#include <composite/properties/typed.hpp>
#include <cstdio>
#include <map>
#include <string>

namespace props = composite::properties;
using props::config_type;
using props::json;

enum class Window { hann, hamming, blackman_harris };
COMPOSITE_ENUM(Window, hann, hamming, blackman_harris);
struct ChannelConfig {
    double center_freq{};
    double bandwidth{};
    std::uint32_t decimation{1};
    Window window{Window::hann};
    bool enabled{true};
};
COMPOSITE_STRUCT(ChannelConfig, center_freq, bandwidth, decimation, window, enabled);

int main() {
    // ---- scalar: validate-before-commit, no-op detection, reject leaves intact ----
    double gain = 1.0;
    json last;
    int fires = 0;
    props::typed_property<double> p_gain{"gain", &gain, config_type::RUNTIME};
    p_gain.validate([](const double& g) { return g > 0.0; }).on_change([&](const json& d) {
        last = d;
        ++fires;
    });
    assert(p_gain.apply(json(2.5), config_type::RUNTIME) == true);
    assert(gain == 2.5 && last == 2.5 && fires == 1);
    assert(p_gain.apply(json(2.5)) == false && fires == 1); // no change -> no notify
    bool threw = false;
    try {
        p_gain.apply(json(-1.0));
    } catch (const props::validation_error&) {
        threw = true;
    }
    assert(threw && gain == 2.5); // rejected: state intact

    // ---- INITIALIZE-only rejects a RUNTIME apply ----
    int buf = 0;
    props::typed_property<int> p_buf{"buf_size", &buf}; // INITIALIZE
    threw = false;
    try {
        p_buf.apply(json(8), config_type::RUNTIME);
    } catch (const props::config_violation&) {
        threw = true;
    }
    assert(threw && buf == 0);
    assert(p_buf.apply(json(8), config_type::INITIALIZE) == true && buf == 8);

    // ---- reflected struct: partial 7396 merge, diff carries only changed field ----
    ChannelConfig cfg{1.0e9, 10e6, 4, Window::hann, true};
    json struct_diff;
    props::typed_property<ChannelConfig> p_cfg{"chan", &cfg, config_type::RUNTIME};
    p_cfg.on_change([&](const json& d) { struct_diff = d; });
    assert(p_cfg.apply(json::parse(R"({"center_freq": 2.0e9})"), config_type::RUNTIME) == true);
    assert(cfg.center_freq == 2.0e9 && cfg.bandwidth == 10e6 && cfg.window == Window::hann);
    assert(struct_diff.size() == 1 && struct_diff.contains("center_freq"));

    // ---- keyed_collection: the channelizer ----
    std::map<std::string, ChannelConfig> channels;
    json coll_diff;
    constexpr double budget = 25e6;
    props::keyed_collection<ChannelConfig> chans{"channels", &channels, config_type::RUNTIME};
    chans.validate_element([](const std::string&, const ChannelConfig& c) { return c.bandwidth > 0.0; })
        .validate_list([&](const std::map<std::string, ChannelConfig>& m) {
            double tot = 0;
            for (auto& [k, c] : m) {
                (void)k;
                tot += c.bandwidth;
            }
            return tot <= budget;
        })
        .on_change([&](const json& d) { coll_diff = d; });

    // upsert two channels (total 20e6 <= 25e6 budget)
    assert(chans.apply(json::parse(R"({"Lband": {"center_freq": 1e9, "bandwidth": 10e6},
                                       "Cband": {"center_freq": 4e9, "bandwidth": 10e6}})"),
                       config_type::RUNTIME) == true);
    assert(chans.size() == 2 && coll_diff.contains("Lband") && coll_diff.contains("Cband"));

    // modify one channel -> diff carries only that key's sub-patch
    assert(chans.apply(json::parse(R"({"Lband": {"center_freq": 1.5e9}})"), config_type::RUNTIME) == true);
    assert(channels["Lband"].center_freq == 1.5e9 && channels["Lband"].bandwidth == 10e6);
    assert(coll_diff.size() == 1 && coll_diff["Lband"].contains("center_freq"));

    // cross-channel invariant: adding 10e6 would make total 30e6 > budget -> reject, unchanged
    threw = false;
    try {
        chans.apply(json::parse(R"({"Xband": {"center_freq": 6e9, "bandwidth": 10e6}})"), config_type::RUNTIME);
    } catch (const props::validation_error&) {
        threw = true;
    }
    assert(threw && chans.size() == 2 && channels.count("Xband") == 0);

    // per-element invariant: bad bandwidth rejected, map intact
    threw = false;
    try {
        chans.apply(json::parse(R"({"Bad": {"bandwidth": -1.0}})"), config_type::RUNTIME);
    } catch (const props::validation_error&) {
        threw = true;
    }
    assert(threw && chans.size() == 2);

    // remove a channel -> others undisturbed (stable identity)
    auto lband_before = channels["Lband"];
    assert(chans.apply(json::parse(R"({"Cband": null})"), config_type::RUNTIME) == true);
    assert(chans.size() == 1 && channels.count("Cband") == 0);
    assert(composite::reflect::equal(channels["Lband"], lband_before)); // Lband untouched
    assert(coll_diff.size() == 1 && coll_diff["Cband"].is_null());

    // ---- P0b: validate(fn, "reason") surfaces the reason in the rejection ----
    {
        int rate = 10;
        props::typed_property<int> p_rate{"rate", &rate, config_type::RUNTIME};
        p_rate.validate([](const int& r) { return r >= 0; }, "rate must be non-negative");
        bool caught = false;
        try {
            p_rate.apply(json(-5), config_type::RUNTIME);
        } catch (const props::validation_error& e) {
            caught = true;
            assert(e.reason == "rate must be non-negative");
            assert(std::string(e.what()).find("rate must be non-negative") != std::string::npos);
        }
        assert(caught && rate == 10);
    }

    // ---- P0b: RFC-7396 null resets to the REGISTERED default, not T{} ----
    // (a member initialized to a non-zero default must come back to that value,
    //  not 0 — the noop_thread_delay-resets-to-100%-CPU bug.)
    {
        int delay = 100; // registration-time default
        props::typed_property<int> p_delay{"delay", &delay, config_type::RUNTIME};
        assert(p_delay.apply(json(50), config_type::RUNTIME) == true && delay == 50);
        assert(p_delay.apply(json(nullptr), config_type::RUNTIME) == true);
        assert(delay == 100); // back to the registered default, NOT 0

        ChannelConfig cfg_d{2e9, 5e6, 8, Window::hamming, true}; // non-default initializers
        props::typed_property<ChannelConfig> p_cfg_d{"chan_d", &cfg_d, config_type::RUNTIME};
        assert(p_cfg_d.apply(json::parse(R"({"center_freq": 9e9})"), config_type::RUNTIME) == true);
        assert(p_cfg_d.apply(json(nullptr), config_type::RUNTIME) == true);
        assert(cfg_d.center_freq == 2e9 && cfg_d.decimation == 8 && cfg_d.window == Window::hamming);
    }

    std::printf("TYPED TESTS PASSED (scalar+struct commit path, keyed collection w/ cross-element invariant)\n");
    return 0;
}
