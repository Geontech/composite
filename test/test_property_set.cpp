// New property_set registry: typed + keyed registration, atomic-batch apply
// (validate-all-then-commit-all), JSON encode/describe.
#include <composite/properties/property_set.hpp>
#include <cassert>
#include <cstdio>
#include <map>
#include <string>

namespace props = composite::properties;
using props::json;
using props::config_type;

enum class Window { hann, hamming };
COMPOSITE_ENUM(Window, hann, hamming);
struct ChannelConfig { double center_freq{}; double bandwidth{}; Window window{Window::hann}; };
COMPOSITE_STRUCT(ChannelConfig, center_freq, bandwidth, window);

int main() {
    props::property_set ps;
    double gain = 1.0; int buf = 0;
    std::map<std::string, ChannelConfig> channels;
    constexpr double budget = 25e6;

    ps.add("gain", gain, config_type::RUNTIME).validate([](const double& g){ return g > 0; });
    ps.add("buf", buf);  // INITIALIZE-only
    ps.add_keyed("channels", channels, config_type::RUNTIME)
      .validate_element([](const std::string&, const ChannelConfig& c){ return c.bandwidth > 0; })
      .validate_list([&](const std::map<std::string,ChannelConfig>& m){
          double t = 0; for (auto& [k,c] : m) { (void)k; t += c.bandwidth; } return t <= budget; });

    // INITIALIZE batch applies both scalars
    auto d = ps.apply(json::parse(R"({"gain": 2.5, "buf": 8})"), config_type::INITIALIZE);
    assert(gain == 2.5 && buf == 8 && d.contains("gain") && d.contains("buf"));

    // atomic batch: buf is INITIALIZE-only -> whole RUNTIME batch rejected, gain untouched
    bool threw = false;
    try { ps.apply(json::parse(R"({"gain": 9.0, "buf": 9})"), config_type::RUNTIME); }
    catch (const props::config_violation&) { threw = true; }
    assert(threw && gain == 2.5 && buf == 8);   // nothing committed

    // atomic batch: gain validator rejects -> channels in the same batch not committed
    threw = false;
    try { ps.apply(json::parse(R"({"gain": -1.0, "channels": {"L": {"bandwidth": 10e6}}})"),
                   config_type::RUNTIME); }
    catch (const props::validation_error&) { threw = true; }
    assert(threw && gain == 2.5 && channels.empty());

    // valid runtime batch: gain + two channels
    ps.apply(json::parse(R"({"gain": 5.0, "channels": {"L": {"bandwidth": 10e6}, "C": {"bandwidth": 10e6}}})"),
             config_type::RUNTIME);
    assert(gain == 5.0 && channels.size() == 2);

    // cross-channel invariant still enforced through the registry
    threw = false;
    try { ps.apply(json::parse(R"({"channels": {"X": {"bandwidth": 10e6}}})"), config_type::RUNTIME); }
    catch (const props::validation_error&) { threw = true; }
    assert(threw && channels.size() == 2);

    // unknown property: throws, or skipped with allow_unknown
    threw = false;
    try { ps.apply(json::parse(R"({"nope": 1})")); } catch (const props::unknown_property&) { threw = true; }
    assert(threw);
    ps.apply(json::parse(R"({"nope": 1, "gain": 6.0})"), config_type::RUNTIME, /*allow_unknown=*/true);
    assert(gain == 6.0);

    // encode + describe
    auto state = ps.encode();
    assert(state["gain"] == 6.0 && state["buf"] == 8 && state["channels"].size() == 2);
    auto schema = ps.describe();
    assert(schema.size() == 3 && schema[0]["name"] == "gain" && schema[0]["configurability"] == "runtime");
    // describe() is enriched: real types, default captured at registration,
    // integer range, and nested element schema with enum choices.
    assert(schema[0]["type"] == "number" && schema[0]["default"] == 1.0);   // gain default at add(), not 6.0
    assert(schema[1]["name"] == "buf" && schema[1]["type"] == "integer"
           && schema[1].contains("minimum") && schema[1].contains("maximum"));
    assert(schema[2]["name"] == "channels" && schema[2]["type"] == "keyed_collection");
    assert(schema[2]["element"]["type"] == "object"
           && schema[2]["element"]["fields"]["window"]["choices"]
                  == json::parse(R"(["hann","hamming"])"));

    // Duplicate property registration throws (instead of returning a
    // reference to a freed object).
    {
        props::property_set ps2;
        double x = 0;
        ps2.add("dup", x);
        bool dup_threw = false;
        try { ps2.add("dup", x); } catch (const std::logic_error&) { dup_threw = true; }
        assert(dup_threw);
    }

    // In an atomic batch, a listener fires only AFTER every value is
    // committed, so it observes the fully-applied batch — never a partial one.
    {
        props::property_set ps3;
        int a = 0, b = 0;
        int b_seen_when_a_notified = -1;
        ps3.add("a", a, config_type::RUNTIME).on_change([&](const json&){ b_seen_when_a_notified = b; });
        ps3.add("b", b, config_type::RUNTIME);
        ps3.apply(json::parse(R"({"a": 1, "b": 2})"), config_type::RUNTIME);
        assert(a == 1 && b == 2);
        assert(b_seen_when_a_notified == 2);  // a's listener saw b already committed (not 0)
    }

    // A keyed-collection commit is node-stable: a held pointer to a surviving
    // element stays valid (same address) across a batch that adds/modifies others.
    {
        props::property_set ps4;
        std::map<std::string, ChannelConfig> ch;
        ps4.add_keyed("ch", ch, config_type::RUNTIME)
           .validate_element([](const std::string&, const ChannelConfig& c){ return c.bandwidth > 0; });
        ps4.apply(json::parse(R"({"ch": {"L": {"bandwidth": 1e6}}})"), config_type::RUNTIME);
        const ChannelConfig* pL = &ch.at("L");
        ps4.apply(json::parse(R"({"ch": {"C": {"bandwidth": 2e6}, "L": {"bandwidth": 3e6}}})"),
                  config_type::RUNTIME);
        assert(ch.size() == 2);
        assert(&ch.at("L") == pL);            // survivor node address preserved
        assert(ch.at("L").bandwidth == 3e6);  // and updated in place
    }

    // Listeners fire in REGISTRATION order, not JSON key order. Register
    // y before x (so registration order differs from sorted key order); the
    // notify sequence must follow registration (y, x), regardless of patch order.
    {
        props::property_set ps5;
        int x = 0, y = 0;
        std::string order;
        ps5.add("y", y, config_type::RUNTIME).on_change([&](const json&){ order += "y"; });
        ps5.add("x", x, config_type::RUNTIME).on_change([&](const json&){ order += "x"; });
        ps5.apply(json::parse(R"({"x": 2, "y": 1})"), config_type::RUNTIME);
        assert(order == "yx");  // registration order, not sorted key order ("xy")
    }

    // A post-commit listener that throws is non-fatal: the batch is already
    // live, so apply() succeeds and routes the failure to the error sink (never a
    // 400-while-live).
    {
        props::property_set ps6;
        int v = 0;
        int warns = 0;
        std::string warned;
        ps6.set_listener_error_handler([&](const std::string& n, const char*){ ++warns; warned = n; });
        ps6.add("v", v, config_type::RUNTIME)
           .on_change([](const json&){ throw std::runtime_error("listener boom"); });
        bool threw = false;
        try { ps6.apply(json::parse(R"({"v": 7})"), config_type::RUNTIME); }
        catch (...) { threw = true; }
        assert(!threw && v == 7 && warns == 1 && warned == "v");
    }

    std::printf("PROPERTY_SET TESTS PASSED (atomic batch, keyed collection, encode/describe, "
                "dup-throws, batch-notify-after-commit, keyed node-stability, "
                "describe-enrichment, registration-order notify, listener-warn)\n");
    return 0;
}
