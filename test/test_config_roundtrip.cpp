// config<T> round-trip parity: a component written with the new
// config<T> form must round-trip config + REST IDENTICALLY to the same component
// written with the per-property add_property form, and react equivalently. Proves
// the wire contract is unchanged and the new form is a drop-in. Own main(); explicit
// checks (NDEBUG-safe). Linked against composite::composite.
#include <composite/core/component.hpp>

#include <cstdint>
#include <cstdio>
#include <string>

using namespace composite;
using composite::properties::config_type;
using json = composite::properties::json;

namespace acme {
enum class window { hann, hamming, blackman_harris };
struct filter_cfg {
    double gain{1.0};
    std::uint32_t taps{8};
    window win{window::hann};
    COMPOSITE_FIELDS(filter_cfg, (gain, runtime, range(0.0, 10.0), unit("dB")),
                     (taps, runtime, range(1u, 1024u), power_of_two), (win, runtime));
};
} // namespace acme
COMPOSITE_ENUM(acme::window, hann, hamming, blackman_harris);

// OLD: the per-property add_property form.
class biquad_old : public component {
public:
    explicit biquad_old(std::string_view id) : component(id) {
        add_property("gain", m_gain, config_type::RUNTIME)
            .units("dB")
            .validate([](const double& g) { return g >= 0.0 && g <= 10.0; }, "gain out of range [0, 10]");
        add_property("taps", m_taps, config_type::RUNTIME)
            .validate([](const std::uint32_t& t) { return t != 0 && (t & (t - 1)) == 0; },
                      "taps must be a power of two");
        add_property("win", m_win, config_type::RUNTIME);
    }
    auto process() -> retval override { return retval::FINISH; }
    auto property_change_handler(const json& diff) -> void override {
        ++reacts;
        last = diff;
    }

    double m_gain{1.0};
    std::uint32_t m_taps{8};
    acme::window m_win{acme::window::hann};
    int reacts{0};
    json last;
    component::auto_stop m_auto_stop{*this};
};

// NEW: the config<T> form.
class biquad_new : public component {
public:
    explicit biquad_new(std::string_view id) : component(id) {
        add_config(m_cfg, config_type::RUNTIME);
        m_cfg.on_apply([this](const acme::filter_cfg& prev, const changes<acme::filter_cfg>& ch) {
            (void)prev;
            ++reacts;
            gain_ch = ch.changed(&acme::filter_cfg::gain);
            taps_ch = ch.changed(&acme::filter_cfg::taps);
        });
    }
    auto process() -> retval override { return retval::FINISH; }

    config<acme::filter_cfg> m_cfg{};
    int reacts{0};
    bool gain_ch{false};
    bool taps_ch{false};
    component::auto_stop m_auto_stop{*this};
};

static int g_fails = 0;
static void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_fails;
    }
}

int main() {
    biquad_old o{"old"};
    biquad_new n{"new"};

    // identical initial state (incl. the base component properties both share)
    check(o.property_state() == n.property_state(), "initial property_state identical");

    // same config-file load
    const json cfg = json::parse(R"({"gain": 2.5, "taps": 16, "win": "hamming"})");
    o.set_properties(cfg, config_type::INITIALIZE);
    n.set_properties(cfg, config_type::INITIALIZE);
    check(o.property_state() == n.property_state(), "state identical after config load");
    check(n.m_cfg->gain == 2.5 && n.m_cfg->taps == 16 && n.m_cfg->win == acme::window::hamming,
          "new form reads via cfg->field");

    // same single-field PATCH, equivalent reaction
    o.reacts = 0;
    n.reacts = 0;
    o.set_properties(json::parse(R"({"gain": 3.0})"), config_type::RUNTIME);
    n.set_properties(json::parse(R"({"gain": 3.0})"), config_type::RUNTIME);
    check(o.property_state() == n.property_state(), "state identical after single-field PATCH");
    check(o.reacts == 1 && n.reacts == 1, "both reacted exactly once");
    check(n.gain_ch && !n.taps_ch, "new reaction scoped to gain (changes<T>)");
    check(o.last.contains("gain") && !o.last.contains("taps"), "old handler diff scoped to gain");

    // single-property route parity (PUT /properties/taps {"value": 32})
    o.set_properties(json::parse(R"({"taps": 32})"), config_type::RUNTIME);
    n.set_properties(json::parse(R"({"taps": 32})"), config_type::RUNTIME);
    check(o.property_state()["taps"] == 32 && n.property_state()["taps"] == 32, "single-field set parity");
    check(n.taps_ch && !n.gain_ch, "new reaction scoped to taps");

    // validation parity: out-of-range gain rejected by both; state intact
    bool to = false;
    bool tn = false;
    try {
        o.set_properties(json::parse(R"({"gain": 99.0})"), config_type::RUNTIME);
    } catch (const std::exception&) {
        to = true;
    }
    try {
        n.set_properties(json::parse(R"({"gain": 99.0})"), config_type::RUNTIME);
    } catch (const std::exception&) {
        tn = true;
    }
    check(to && tn, "both reject out-of-range gain");
    check(o.property_state() == n.property_state(), "state identical after a rejected PATCH");

    // power_of_two parity
    to = tn = false;
    try {
        o.set_properties(json::parse(R"({"taps": 7})"), config_type::RUNTIME);
    } catch (const std::exception&) {
        to = true;
    }
    try {
        n.set_properties(json::parse(R"({"taps": 7})"), config_type::RUNTIME);
    } catch (const std::exception&) {
        tn = true;
    }
    check(to && tn, "both reject non-power-of-two taps");

    // both schemas expose the same property names (the new form is richer: it also
    // carries range/unit from the attributes — strictly more capable, wire-identical).
    // property_schema() is a single JSON Schema 2020-12 document; names are the keys of
    // "properties", so compare those key sets rather than array entries.
    const json so = o.property_schema();
    const json sn = n.property_schema();
    check(so.at("properties").size() == sn.at("properties").size(), "both describe the same number of properties");
    auto has_named = [](const json& sch, const char* nm) { return sch.at("properties").contains(nm); };
    check(has_named(sn, "gain") && has_named(sn, "taps") && has_named(sn, "win"),
          "new schema exposes gain/taps/win at top level");
    check(has_named(so, "gain") && has_named(sn, "gain"), "both forms expose gain by the same key");

    if (g_fails != 0) {
        std::fprintf(stderr, "%d round-trip check(s) FAILED\n", g_fails);
        return 1;
    }
    std::puts("CONFIG<T> ROUND-TRIP PARITY TESTS PASSED");
    return 0;
}
