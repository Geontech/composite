/*
 * Copyright (C) 2024-2026 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Component-level property tests on the new typed JSON model (reflect/typed/
 * property_set): registration, atomic set_properties(json), get_property,
 * validation (no mutation on reject), INITIALIZE-vs-RUNTIME, keyed collections
 * with cross-element invariants, and property_state()/property_schema().
 */

#include "composite/core/component.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <map>
#include <string>

using namespace composite;
using composite::properties::config_type;
using json = composite::properties::json;

namespace {
enum class Win { hann, hamming };
struct Chan {
    double cf{};
    double bw{};
    Win win{Win::hann};
};
struct Net {
    std::string host{"localhost"};
    std::uint16_t port{8080};
};
} // namespace

COMPOSITE_ENUM(Win, hann, hamming);
COMPOSITE_STRUCT(Chan, cf, bw, win);
COMPOSITE_STRUCT(Net, host, port);

namespace {
class cfg_component : public component {
public:
    explicit cfg_component(std::string_view id) : component(id) {
        add_property("gain", m_gain, config_type::RUNTIME).units("dB").validate([](const double& g) {
            return g > 0.0;
        });
        add_property("buf_size", m_buf, config_type::INITIALIZE);
        add_property("net", m_net, config_type::RUNTIME);
        add_keyed("channels", m_channels, config_type::RUNTIME)
            .validate_element([](const std::string&, const Chan& c) { return c.bw > 0.0; })
            .validate_list([this](const std::map<std::string, Chan>& m) {
                double total = 0;
                for (const auto& [k, c] : m) {
                    (void)k;
                    total += c.bw;
                }
                return total <= m_budget;
            })
            .on_change([this](const json& d) { m_last_diff = d; });
    }
    auto process() -> retval override { return retval::FINISH; }
    auto property_change_handler(const json& /*diff*/) -> void override { ++m_handler_calls; }

    double m_gain{1.0};
    std::int32_t m_buf{1024};
    Net m_net;
    std::map<std::string, Chan> m_channels;
    double m_budget{25e6};
    json m_last_diff;
    int m_handler_calls{0};
    component::auto_stop m_auto_stop{*this}; // MUST be last
};
} // namespace

TEST_CASE("scalar property: set / get / validate / no-op", "[properties]") {
    cfg_component c{"c"};
    c.set_properties(json::parse(R"({"gain": 2.5})"), config_type::RUNTIME);
    REQUIRE(c.get_property<double>("gain") == 2.5);
    REQUIRE(c.m_handler_calls == 1);

    REQUIRE_THROWS_AS(c.set_properties(json::parse(R"({"gain": -1.0})"), config_type::RUNTIME),
                      properties::validation_error);
    REQUIRE(c.get_property<double>("gain") == 2.5); // unchanged on reject
}

TEST_CASE("INITIALIZE property rejects a runtime apply", "[properties]") {
    cfg_component c{"c"};
    REQUIRE_THROWS_AS(c.set_properties(json::parse(R"({"buf_size": 2048})"), config_type::RUNTIME),
                      properties::config_violation);
    c.set_properties(json::parse(R"({"buf_size": 2048})"), config_type::INITIALIZE);
    REQUIRE(c.get_property<std::int32_t>("buf_size") == 2048);
}

TEST_CASE("multi-property batch is atomic (one reject commits nothing)", "[properties]") {
    cfg_component c{"c"};
    REQUIRE_THROWS(c.set_properties(json::parse(R"({"gain": 9.0, "buf_size": 9})"), config_type::RUNTIME));
    REQUIRE(c.get_property<double>("gain") == 1.0); // gain not committed
}

TEST_CASE("nested struct: RFC-7396 partial merge", "[properties]") {
    cfg_component c{"c"};
    c.set_properties(json::parse(R"({"net": {"port": 9000}})"), config_type::RUNTIME);
    REQUIRE(c.m_net.port == 9000);
    REQUIRE(c.m_net.host == "localhost"); // untouched field preserved
}

TEST_CASE("keyed collection: add / modify / remove + cross-element invariant", "[properties][keyed]") {
    cfg_component c{"c"};
    c.set_properties(json::parse(R"({"channels": {"L": {"bw": 10e6}, "C": {"bw": 10e6}}})"), config_type::RUNTIME);
    REQUIRE(c.m_channels.size() == 2);
    REQUIRE(c.m_last_diff.contains("L"));

    c.set_properties(json::parse(R"({"channels": {"L": {"cf": 1.5e9}}})"), config_type::RUNTIME);
    REQUIRE(c.m_channels["L"].cf == 1.5e9);
    REQUIRE(c.m_channels["L"].bw == 10e6);

    REQUIRE_THROWS_AS(c.set_properties(json::parse(R"({"channels": {"X": {"bw": 10e6}}})"), config_type::RUNTIME),
                      properties::validation_error);
    REQUIRE(c.m_channels.size() == 2);

    REQUIRE_THROWS(c.set_properties(json::parse(R"({"channels": {"B": {"bw": -1.0}}})"), config_type::RUNTIME));
    REQUIRE(c.m_channels.size() == 2);

    auto l_cf = c.m_channels["L"].cf;
    c.set_properties(json::parse(R"({"channels": {"C": null}})"), config_type::RUNTIME);
    REQUIRE(c.m_channels.size() == 1);
    REQUIRE(c.m_channels.count("C") == 0);
    REQUIRE(c.m_channels["L"].cf == l_cf); // survivor undisturbed
}

TEST_CASE("property_state and property_schema", "[properties]") {
    cfg_component c{"c"};
    c.set_properties(json::parse(R"({"gain": 3.0})"), config_type::RUNTIME);
    auto state = c.property_state();
    REQUIRE(state["gain"] == 3.0);
    REQUIRE(state.contains("channels"));
    REQUIRE(state.contains("net"));

    // property_schema() is ONE JSON Schema 2020-12 document: names are keys under
    // "properties", composite metadata rides as x-composite-* vendor extensions.
    auto schema = c.property_schema();
    REQUIRE(schema.is_object());
    REQUIRE(schema["$schema"] == "https://json-schema.org/draft/2020-12/schema");
    REQUIRE(schema["type"] == "object");
    REQUIRE(schema["additionalProperties"] == false);
    // No "required": every property has a default and PATCH is a merge, so a partial
    // document must validate (see property_set::schema()).
    REQUIRE(!schema.contains("required"));

    const auto& props = schema.at("properties");
    REQUIRE(props.contains("gain"));
    REQUIRE(props["gain"]["x-composite-configurability"] == "runtime");
    REQUIRE(props["gain"].contains("default"));
    REQUIRE(!props["gain"].contains("name")); // the name IS the key now

    // the `enabled` lifecycle virtual is advertised alongside the value properties
    REQUIRE(props.contains("enabled"));
    REQUIRE(props["enabled"]["type"] == "boolean");
    REQUIRE(props["enabled"]["x-composite-configurability"] == "runtime");

    // struct properties expose their members as a nested 2020-12 "properties" map,
    // never the internal "fields" vocabulary.
    REQUIRE(props.contains("net"));
    REQUIRE(props["net"].contains("properties"));
    REQUIRE(!props["net"].contains("fields"));
}
