/*
 * Copyright (C) 2025 Geon Technologies, LLC
 *
 * This file is part of composite.
 *
 * composite is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * composite is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see http://www.gnu.org/licenses/.
 */

#define CATCH_CONFIG_MAIN
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include "composite/component.hpp"
#include "composite/property_set.hpp"
#include "helpers.hpp"
#include "property_rest_api.hpp"

using namespace composite;
using namespace composite::properties;
using namespace composite::properties::rest;

// ============================================================================
// Test Data Structures
// ============================================================================

struct audio_config {
    int32_t sample_rate{48000};
    int16_t bit_depth{16};
    std::string codec{"pcm"};
};

struct network_connection {
    uint32_t id{0};
    std::optional<std::string> label;
    std::string host{"localhost"};
    uint16_t port{8080};
    int32_t timeout{5000};
};

// Nested struct for testing deep nesting with list-of-structs
struct scaling_entry {
    float h_scale{1.0f};
    float v_scale{1.0f};
    bool preserve{false};
};

struct decimate_options {
    std::string algorithm{"default"};
    std::string kernel{"default"};
    std::vector<scaling_entry> scaling;
};

struct discretize_options {
    std::string algorithm{"default"};
    std::string style{"default"};
};

struct ingest_overrides {
    bool preserve_raw{false};
    discretize_options discretize_opts;
    decimate_options decimate_opts;
};

struct test_config {
    // Scalar properties
    int32_t sample_rate{48000};
    int32_t buffer_size{1024};
    float gain{1.0f};
    double threshold{0.5};
    bool enabled{true};
    std::string name{"test"};

    // Optional properties
    std::optional<int32_t> timeout;
    std::optional<std::string> description;

    // List properties
    std::vector<std::string> channels;
    std::vector<float> thresholds;
    std::vector<int32_t> sample_rates;

    // Struct property
    audio_config audio;

    // Struct list property
    std::vector<network_connection> connections;

    // Deeply nested struct with struct-list inside
    ingest_overrides ingest_opts;
};

// ============================================================================
// Test Component
// ============================================================================

class test_component : public component {
public:
    explicit test_component(std::string_view id = "test_comp") : component(id) {
        // Register scalar properties
        add_property("sample_rate", &m_config.sample_rate).configurability(config_type::RUNTIME).units("Hz");
        add_property("buffer_size", &m_config.buffer_size).configurability(config_type::INITIALIZE).units("samples");
        add_property("gain", &m_config.gain).configurability(config_type::RUNTIME).units("linear");
        add_property("threshold", &m_config.threshold).configurability(config_type::RUNTIME).units("linear");
        add_property("enabled", &m_config.enabled).configurability(config_type::RUNTIME);
        add_property("name", &m_config.name).configurability(config_type::RUNTIME);

        // Register optional properties
        add_property("timeout", &m_config.timeout).configurability(config_type::RUNTIME).units("ms");
        add_property("description", &m_config.description).configurability(config_type::RUNTIME);

        // Register list properties
        add_list_property("channels", &m_config.channels).configurability(config_type::RUNTIME);
        add_list_property("thresholds", &m_config.thresholds).configurability(config_type::RUNTIME).units("linear");
        add_list_property("sample_rates", &m_config.sample_rates).configurability(config_type::RUNTIME).units("Hz");

        // Register struct property
        add_struct_property("audio", &m_config.audio, [](property_set& ps, audio_config* cfg) {
            ps.add_property("sample_rate", &cfg->sample_rate).configurability(config_type::RUNTIME).units("Hz");
            ps.add_property("bit_depth", &cfg->bit_depth).configurability(config_type::RUNTIME).units("bits");
            ps.add_property("codec", &cfg->codec).configurability(config_type::RUNTIME);
        }).configurability(config_type::RUNTIME);

        // Register struct list property
        add_struct_list_property("connections", &m_config.connections, [](property_set& ps, network_connection* conn) {
            ps.add_property("id", &conn->id).configurability(config_type::RUNTIME);
            ps.add_property("label", &conn->label).configurability(config_type::RUNTIME);
            ps.add_property("host", &conn->host).configurability(config_type::RUNTIME);
            ps.add_property("port", &conn->port).configurability(config_type::RUNTIME);
            ps.add_property("timeout", &conn->timeout).configurability(config_type::RUNTIME).units("ms");
        }).configurability(config_type::RUNTIME);

        // Register deeply nested struct with list-of-structs (matches user's scenario)
        add_struct_property("ingest_overrides", &m_config.ingest_opts, [](property_set& set, ::ingest_overrides* prop) {
            set.add_property("preserve_raw", &prop->preserve_raw).configurability(config_type::RUNTIME);
            set.add_struct_property("discretize_opts", &prop->discretize_opts, [](property_set& set, discretize_options* prop) {
                set.add_property("algorithm", &prop->algorithm).configurability(config_type::RUNTIME);
                set.add_property("style", &prop->style).configurability(config_type::RUNTIME);
            }).configurability(config_type::RUNTIME);
            set.add_struct_property("decimate_opts", &prop->decimate_opts, [](property_set& set, decimate_options* prop) {
                set.add_property("algorithm", &prop->algorithm).configurability(config_type::RUNTIME);
                set.add_property("kernel", &prop->kernel).configurability(config_type::RUNTIME);
                set.add_struct_list_property("scaling", &prop->scaling, [](property_set& set, scaling_entry* prop) {
                    set.add_property("h_scale", &prop->h_scale).configurability(config_type::RUNTIME);
                    set.add_property("v_scale", &prop->v_scale).configurability(config_type::RUNTIME);
                    set.add_property("preserve", &prop->preserve).configurability(config_type::RUNTIME);
                }).configurability(config_type::RUNTIME);
            }).configurability(config_type::RUNTIME);
        }).configurability(config_type::RUNTIME);
    }

    auto process() -> retval override {
        return retval::FINISH;
    }

    auto get_config() -> test_config& { return m_config; }
    auto get_config() const -> const test_config& { return m_config; }

private:
    test_config m_config;
};

// ============================================================================
// Helper Functions for Tests
// ============================================================================

auto parse_json_response(const httplib::Response& res) -> nlohmann::json {
    return nlohmann::json::parse(res.body);
}

auto get_error_code(const nlohmann::json& response) -> std::string {
    if (!response.contains("error")) {
        return "";
    }
    return response["error"]["code"].get<std::string>();
}

// ============================================================================
// Property System Tests
// ============================================================================

TEST_CASE("Property System - Scalar Properties") {
    test_config cfg;
    property_set ps;

    ps.add_property("gain", &cfg.gain);
    ps.add_property("sample_rate", &cfg.sample_rate);
    ps.add_property("enabled", &cfg.enabled);
    ps.add_property("name", &cfg.name);

    SECTION("Get and set int32 property") {
        REQUIRE(ps.get_property<int32_t>("sample_rate") == 48000);
        REQUIRE(ps.set_property("sample_rate", int32_t{96000}) == error::OK);
        REQUIRE(cfg.sample_rate == 96000);
        REQUIRE(ps.get_property<int32_t>("sample_rate") == 96000);
    }

    SECTION("Get and set float property") {
        REQUIRE_THAT(ps.get_property<float>("gain"), Catch::Matchers::WithinAbs(1.0, 0.000001));
        REQUIRE(ps.set_property("gain", float{0.5f}) == error::OK);
        REQUIRE_THAT(cfg.gain, Catch::Matchers::WithinAbs(0.5, 0.000001));
    }

    SECTION("Get and set bool property") {
        REQUIRE(ps.get_property<bool>("enabled") == true);
        REQUIRE(ps.set_property("enabled", false) == error::OK);
        REQUIRE(cfg.enabled == false);
    }

    SECTION("Get and set string property") {
        REQUIRE(ps.get_property<std::string>("name") == "test");
        REQUIRE(ps.set_property("name", std::string{"new_name"}) == error::OK);
        REQUIRE(cfg.name == "new_name");
    }

    SECTION("Set properties using string values") {
        ps.set_properties({{"sample_rate", "44100"}});
        REQUIRE(cfg.sample_rate == 44100);

        ps.set_properties({{"gain", "0.75"}});
        REQUIRE_THAT(cfg.gain, Catch::Matchers::WithinAbs(0.75, 0.000001));

        ps.set_properties({{"enabled", "false"}});
        REQUIRE(cfg.enabled == false);

        ps.set_properties({{"name", "updated"}});
        REQUIRE(cfg.name == "updated");
    }

    SECTION("Reset property to default using null") {
        cfg.gain = 0.75f;
        ps.set_properties({{"gain", std::string{null_prop}}});
        REQUIRE_THAT(cfg.gain, Catch::Matchers::WithinAbs(0.0, 0.000001));
    }
}

TEST_CASE("Property System - Optional Properties") {
    test_config cfg;
    property_set ps;

    ps.add_property("timeout", &cfg.timeout);
    ps.add_property("description", &cfg.description);

    SECTION("Optional int32 property") {
        REQUIRE_FALSE(cfg.timeout.has_value());
        REQUIRE(ps.try_get_property<int32_t>("timeout") == std::nullopt);

        REQUIRE(ps.set_property("timeout", int32_t{1000}) == error::OK);
        REQUIRE(cfg.timeout.has_value());
        REQUIRE(cfg.timeout.value() == 1000);
        REQUIRE(ps.try_get_property<int32_t>("timeout") == 1000);

        ps.set_properties({{"timeout", std::string{null_prop}}});
        REQUIRE_FALSE(cfg.timeout.has_value());
    }

    SECTION("Optional string property") {
        REQUIRE_FALSE(cfg.description.has_value());

        ps.set_properties({{"description", "test description"}});
        REQUIRE(cfg.description.has_value());
        REQUIRE(cfg.description.value() == "test description");

        ps.set_properties({{"description", std::string{null_prop}}});
        REQUIRE_FALSE(cfg.description.has_value());
    }
}

TEST_CASE("Property System - List Properties") {
    test_config cfg;
    property_set ps;

    ps.add_list_property("channels", &cfg.channels);
    ps.add_list_property("thresholds", &cfg.thresholds);
    ps.add_list_property("sample_rates", &cfg.sample_rates);

    SECTION("Set entire list") {
        std::vector<std::string> channels = {"left", "right", "center"};
        REQUIRE(ps.set_list_property("channels", channels) == error::OK);
        REQUIRE(cfg.channels == channels);
        REQUIRE(ps.get_list_property<std::string>("channels") == channels);
    }

    SECTION("Set list using set_properties") {
        std::vector<std::string> str_values = {"1.0", "2.0", "3.0"};
        ps.set_properties({{"thresholds", str_values}});
        REQUIRE(cfg.thresholds.size() == 3);
        REQUIRE_THAT(cfg.thresholds[0], Catch::Matchers::WithinAbs(1.0, 0.000001));
        REQUIRE_THAT(cfg.thresholds[1], Catch::Matchers::WithinAbs(2.0, 0.000001));
        REQUIRE_THAT(cfg.thresholds[2], Catch::Matchers::WithinAbs(3.0, 0.000001));
    }

    SECTION("Get and set list item by index") {
        cfg.sample_rates = {8000, 16000, 32000};
        REQUIRE(ps.get_list_property_item<int32_t>("sample_rates", 0) == 8000);
        REQUIRE(ps.get_list_property_item<int32_t>("sample_rates", 1) == 16000);

        REQUIRE(ps.set_list_property_item("sample_rates", 1, int32_t{44100}) == error::OK);
        REQUIRE(cfg.sample_rates[1] == 44100);
    }

    SECTION("Set list item using set_properties with index notation") {
        cfg.sample_rates = {8000, 16000, 32000};
        ps.set_properties({{"sample_rates[1]", "48000"}});
        REQUIRE(cfg.sample_rates[1] == 48000);
    }

    SECTION("Append to list") {
        REQUIRE(ps.append_list_property_item("channels", std::string{"mono"}) == error::OK);
        REQUIRE(cfg.channels.size() == 1);
        REQUIRE(cfg.channels[0] == "mono");

        REQUIRE(ps.append_list_property_item("channels", std::string{"stereo"}) == error::OK);
        REQUIRE(cfg.channels.size() == 2);
        REQUIRE(cfg.channels[1] == "stereo");
    }

    SECTION("Append using set_properties with [] notation") {
        ps.set_properties({{"channels[]", "left"}});
        REQUIRE(cfg.channels.size() == 1);
        ps.set_properties({{"channels[]", "right"}});
        REQUIRE(cfg.channels.size() == 2);
        REQUIRE(cfg.channels[0] == "left");
        REQUIRE(cfg.channels[1] == "right");
    }

    SECTION("Erase list item") {
        cfg.channels = {"left", "right", "center"};
        REQUIRE(ps.erase_list_property_item<std::string>("channels", 1) == error::OK);
        REQUIRE(cfg.channels.size() == 2);
        REQUIRE(cfg.channels[0] == "left");
        REQUIRE(cfg.channels[1] == "center");
    }

    SECTION("Erase list item using set_properties with null") {
        cfg.channels = {"left", "right", "center"};
        ps.set_properties({{"channels[0]", std::string{null_prop}}});
        REQUIRE(cfg.channels.size() == 2);
        REQUIRE(cfg.channels[0] == "right");
        REQUIRE(cfg.channels[1] == "center");
    }
}

TEST_CASE("Property System - Struct Properties") {
    test_config cfg;
    property_set ps;

    ps.add_struct_property("audio", &cfg.audio, [](property_set& p, audio_config* a) {
        p.add_property("sample_rate", &a->sample_rate);
        p.add_property("bit_depth", &a->bit_depth);
        p.add_property("codec", &a->codec);
    });

    SECTION("Get struct field using dot notation") {
        cfg.audio.sample_rate = 44100;
        cfg.audio.bit_depth = 24;
        cfg.audio.codec = "flac";

        REQUIRE(ps.get_property<int32_t>("audio.sample_rate") == 44100);
        REQUIRE(ps.get_property<int16_t>("audio.bit_depth") == 24);
        REQUIRE(ps.get_property<std::string>("audio.codec") == "flac");
    }

    SECTION("Set struct field using dot notation") {
        REQUIRE(ps.set_property("audio.sample_rate", int32_t{96000}) == error::OK);
        REQUIRE(cfg.audio.sample_rate == 96000);

        REQUIRE(ps.set_property("audio.codec", std::string{"mp3"}) == error::OK);
        REQUIRE(cfg.audio.codec == "mp3");
    }

    SECTION("Set struct fields using set_properties") {
        std::vector<std::pair<std::string, std::string>> fields = {
            {"sample_rate", "192000"},
            {"bit_depth", "32"},
            {"codec", "opus"}
        };
        ps.set_properties({{"audio", fields}});

        REQUIRE(cfg.audio.sample_rate == 192000);
        REQUIRE(cfg.audio.bit_depth == 32);
        REQUIRE(cfg.audio.codec == "opus");
    }

    SECTION("Reset struct using null") {
        cfg.audio = {96000, 24, "flac"};
        ps.set_properties({{"audio", std::string{null_prop}}});
        // Struct reset keeps default initialization values
        REQUIRE(cfg.audio.sample_rate == 48000); // Default value
        REQUIRE(cfg.audio.bit_depth == 16); // Default value
        REQUIRE(cfg.audio.codec == "pcm"); // Default value
    }
}

TEST_CASE("Property System - Struct List Properties") {
    test_config cfg;
    property_set ps;

    ps.add_struct_list_property("connections", &cfg.connections, [](property_set& p, network_connection* conn) {
        p.add_property("id", &conn->id);
        p.add_property("label", &conn->label);
        p.add_property("host", &conn->host);
        p.add_property("port", &conn->port);
        p.add_property("timeout", &conn->timeout);
    });

    SECTION("Update struct list item field using set_properties") {
        cfg.connections.push_back({1, "primary", "server1.com", 8080, 1000});

        std::vector<std::pair<std::string, std::string>> fields = {
            {"id", "2"},
            {"host", "server2.com"},
            {"port", "9090"}
        };
        ps.set_properties({{"connections[0]", fields}});

        REQUIRE(cfg.connections[0].id == 2);
        REQUIRE(cfg.connections[0].host == "server2.com");
        REQUIRE(cfg.connections[0].port == 9090);
    }

    SECTION("Append struct to list using set_properties") {
        std::vector<std::pair<std::string, std::string>> fields = {
            {"id", "1"},
            {"label", "primary"},
            {"host", "localhost"},
            {"port", "8080"},
            {"timeout", "5000"}
        };
        ps.set_properties({{"connections[]", fields}});

        REQUIRE(cfg.connections.size() == 1);
        REQUIRE(cfg.connections[0].id == 1);
        REQUIRE(cfg.connections[0].label == "primary");
        REQUIRE(cfg.connections[0].host == "localhost");
        REQUIRE(cfg.connections[0].port == 8080);
    }

    SECTION("Erase struct list item using null") {
        cfg.connections.push_back({1, "conn1", "host1", 8080, 1000});
        cfg.connections.push_back({2, "conn2", "host2", 9090, 2000});

        ps.set_properties({{"connections[0]", std::string{null_prop}}});
        REQUIRE(cfg.connections.size() == 1);
        REQUIRE(cfg.connections[0].id == 2);
    }
}

TEST_CASE("Property System - Deeply Nested Struct with List-of-Structs") {
    test_component comp("nested_test");
    auto& cfg = comp.get_config();

    SECTION("Set nested scalar properties via component") {
        comp.set_properties({
            {"ingest_overrides.preserve_raw", "true"},
            {"ingest_overrides.discretize_opts.algorithm", "advanced"},
            {"ingest_overrides.discretize_opts.style", "smooth"},
            {"ingest_overrides.decimate_opts.algorithm", "lanczos"},
            {"ingest_overrides.decimate_opts.kernel", "cubic"}
        }, config_type::RUNTIME);

        REQUIRE(cfg.ingest_opts.preserve_raw == true);
        REQUIRE(cfg.ingest_opts.discretize_opts.algorithm == "advanced");
        REQUIRE(cfg.ingest_opts.discretize_opts.style == "smooth");
        REQUIRE(cfg.ingest_opts.decimate_opts.algorithm == "lanczos");
        REQUIRE(cfg.ingest_opts.decimate_opts.kernel == "cubic");
    }

    SECTION("Append struct to nested list using component") {
        std::vector<std::pair<std::string, std::string>> fields = {
            {"h_scale", "2.0"},
            {"v_scale", "1.5"},
            {"preserve", "true"}
        };
        comp.set_properties({{"ingest_overrides.decimate_opts.scaling[]", fields}}, config_type::RUNTIME);

        REQUIRE(cfg.ingest_opts.decimate_opts.scaling.size() == 1);
        REQUIRE(cfg.ingest_opts.decimate_opts.scaling[0].h_scale == 2.0f);
        REQUIRE(cfg.ingest_opts.decimate_opts.scaling[0].v_scale == 1.5f);
        REQUIRE(cfg.ingest_opts.decimate_opts.scaling[0].preserve == true);
    }

    SECTION("Append multiple structs to nested list") {
        std::vector<std::pair<std::string, std::string>> entry1 = {
            {"h_scale", "2.0"},
            {"v_scale", "2.0"},
            {"preserve", "false"}
        };
        std::vector<std::pair<std::string, std::string>> entry2 = {
            {"h_scale", "4.0"},
            {"v_scale", "4.0"},
            {"preserve", "true"}
        };

        comp.set_properties({{"ingest_overrides.decimate_opts.scaling[]", entry1}}, config_type::RUNTIME);
        comp.set_properties({{"ingest_overrides.decimate_opts.scaling[]", entry2}}, config_type::RUNTIME);

        REQUIRE(cfg.ingest_opts.decimate_opts.scaling.size() == 2);
        REQUIRE(cfg.ingest_opts.decimate_opts.scaling[0].h_scale == 2.0f);
        REQUIRE(cfg.ingest_opts.decimate_opts.scaling[1].h_scale == 4.0f);
        REQUIRE(cfg.ingest_opts.decimate_opts.scaling[1].preserve == true);
    }

    SECTION("Update nested list item field") {
        // First add an entry
        std::vector<std::pair<std::string, std::string>> entry = {
            {"h_scale", "1.0"},
            {"v_scale", "1.0"},
            {"preserve", "false"}
        };
        comp.set_properties({{"ingest_overrides.decimate_opts.scaling[]", entry}}, config_type::RUNTIME);

        // Then update a specific field in the entry
        comp.set_properties({{"ingest_overrides.decimate_opts.scaling[0].h_scale", "3.5"}}, config_type::RUNTIME);

        REQUIRE(cfg.ingest_opts.decimate_opts.scaling[0].h_scale == 3.5f);
        REQUIRE(cfg.ingest_opts.decimate_opts.scaling[0].v_scale == 1.0f); // Unchanged
    }

    SECTION("Replace entire nested list using PATCH-style update") {
        // Add initial entry
        std::vector<std::pair<std::string, std::string>> entry_initial = {
            {"h_scale", "1.0"},
            {"v_scale", "1.0"},
            {"preserve", "false"}
        };
        comp.set_properties({{"ingest_overrides.decimate_opts.scaling[]", entry_initial}}, config_type::RUNTIME);

        REQUIRE(cfg.ingest_opts.decimate_opts.scaling.size() == 1);

        // Clear and replace with new entries
        comp.set_properties({{"ingest_overrides.decimate_opts.scaling", std::string{null_prop}}}, config_type::RUNTIME);
        REQUIRE(cfg.ingest_opts.decimate_opts.scaling.size() == 0);

        std::vector<std::pair<std::string, std::string>> entry_new1 = {
            {"h_scale", "2.0"},
            {"v_scale", "2.0"},
            {"preserve", "true"}
        };
        std::vector<std::pair<std::string, std::string>> entry_new2 = {
            {"h_scale", "4.0"},
            {"v_scale", "4.0"},
            {"preserve", "false"}
        };
        comp.set_properties({
            {"ingest_overrides.decimate_opts.scaling[]", entry_new1},
            {"ingest_overrides.decimate_opts.scaling[]", entry_new2}
        }, config_type::RUNTIME);

        REQUIRE(cfg.ingest_opts.decimate_opts.scaling.size() == 2);
        REQUIRE(cfg.ingest_opts.decimate_opts.scaling[0].h_scale == 2.0f);
        REQUIRE(cfg.ingest_opts.decimate_opts.scaling[1].h_scale == 4.0f);
    }

    SECTION("Delete nested list item") {
        // Add two entries
        std::vector<std::pair<std::string, std::string>> del_entry1 = {
            {"h_scale", "1.0"},
            {"v_scale", "1.0"},
            {"preserve", "false"}
        };
        std::vector<std::pair<std::string, std::string>> del_entry2 = {
            {"h_scale", "2.0"},
            {"v_scale", "2.0"},
            {"preserve", "true"}
        };
        comp.set_properties({
            {"ingest_overrides.decimate_opts.scaling[]", del_entry1},
            {"ingest_overrides.decimate_opts.scaling[]", del_entry2}
        }, config_type::RUNTIME);

        REQUIRE(cfg.ingest_opts.decimate_opts.scaling.size() == 2);

        // Delete first entry using null
        comp.set_properties({{"ingest_overrides.decimate_opts.scaling[0]", std::string{null_prop}}}, config_type::RUNTIME);

        REQUIRE(cfg.ingest_opts.decimate_opts.scaling.size() == 1);
        REQUIRE(cfg.ingest_opts.decimate_opts.scaling[0].h_scale == 2.0f); // Second entry is now first
    }
}

TEST_CASE("Property System - Configurability") {
    test_component comp("test");
    auto& cfg = comp.get_config();

    SECTION("Runtime configurable property can be updated") {
        REQUIRE_NOTHROW(comp.set_properties({{"sample_rate", "96000"}}, config_type::RUNTIME));
        REQUIRE(cfg.sample_rate == 96000);
    }

    SECTION("Initialize-only property cannot be updated at runtime") {
        REQUIRE_THROWS_AS(comp.set_properties({{"buffer_size", "2048"}}, config_type::RUNTIME), configurability_error);
        REQUIRE(cfg.buffer_size == 1024); // Unchanged
    }

    SECTION("Initialize-only property can be set during initialization") {
        REQUIRE_NOTHROW(comp.set_properties({{"buffer_size", "2048"}}, config_type::INITIALIZE));
        REQUIRE(cfg.buffer_size == 2048);
    }
}

// ============================================================================
// REST API Handler Tests - Phase 1: Basic Property Operations
// ============================================================================

TEST_CASE("REST API - Get Properties Collection") {
    auto comp = test_component("test_comp");

    SECTION("Get all properties") {
        httplib::Request req;
        auto res = property_handlers::get_properties_collection(&comp, req, "/test/path");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::OK_200);
        REQUIRE(json_res.contains("properties"));
        REQUIRE(json_res.contains("count"));
        REQUIRE(json_res["count"].get<size_t>() > 0);

        // Check that properties have correct structure
        auto& props = json_res["properties"];
        REQUIRE(props.contains("sample_rate"));
        REQUIRE(props["sample_rate"].contains("name"));
        REQUIRE(props["sample_rate"].contains("type"));
        REQUIRE(props["sample_rate"].contains("value"));
        REQUIRE(props["sample_rate"].contains("configurability"));
        REQUIRE(props["sample_rate"].contains("href"));
    }

    SECTION("Filter properties by type") {
        httplib::Request req;
        req.params.insert({"type", "int32"});
        auto res = property_handlers::get_properties_collection(&comp, req, "/test/path");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::OK_200);
        // All returned properties should be int32
        for (const auto& [name, prop] : json_res["properties"].items()) {
            REQUIRE(prop["type"].get<std::string>() == "int32");
        }
    }

    SECTION("Filter properties by configurability") {
        httplib::Request req;
        req.params.insert({"configurability", "runtime"});
        auto res = property_handlers::get_properties_collection(&comp, req, "/test/path");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::OK_200);
        // All returned properties should be runtime configurable
        for (const auto& [name, prop] : json_res["properties"].items()) {
            REQUIRE(prop["configurability"].get<std::string>() == "runtime");
        }
    }

    SECTION("Get properties with schema information") {
        httplib::Request req;
        req.params.insert({"schema", "true"});
        auto res = property_handlers::get_properties_collection(&comp, req, "/test/path");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::OK_200);
        REQUIRE(json_res.contains("properties"));

        // Properties should include schema fields
        auto& props = json_res["properties"];
        REQUIRE(props.contains("sample_rate"));
        auto& sample_rate = props["sample_rate"];

        // Schema includes name, type, units, configurability, and href
        REQUIRE(sample_rate.contains("name"));
        REQUIRE(sample_rate.contains("type"));
        REQUIRE(sample_rate.contains("units"));
        REQUIRE(sample_rate.contains("configurability"));
        REQUIRE(sample_rate.contains("href"));

        // Verify schema values
        REQUIRE(sample_rate["name"].get<std::string>() == "sample_rate");
        REQUIRE(sample_rate["type"].get<std::string>() == "int32");
        REQUIRE(sample_rate["configurability"].get<std::string>() == "runtime");
    }
}

TEST_CASE("REST API - Get Single Property") {
    auto comp = test_component("test_comp");

    SECTION("Get existing property") {
        auto res = property_handlers::get_property(&comp, "sample_rate", "/test/path");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::OK_200);
        REQUIRE(json_res["name"].get<std::string>() == "sample_rate");
        REQUIRE(json_res["type"].get<std::string>() == "int32");
        REQUIRE(json_res.contains("value"));
        REQUIRE(json_res.contains("configurability"));
    }

    SECTION("Get non-existent property") {
        auto res = property_handlers::get_property(&comp, "does_not_exist", "/test/path");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::NotFound_404);
        REQUIRE(get_error_code(json_res) == "PROPERTY_NOT_FOUND");
        REQUIRE(json_res["error"]["details"]["property"].get<std::string>() == "does_not_exist");
    }
}

TEST_CASE("REST API - Update Property (PUT)") {
    auto comp = test_component("test_comp");
    auto& cfg = comp.get_config();

    SECTION("Update runtime-configurable property") {
        auto request_body = nlohmann::json{{"value", 96000}};
        auto res = property_handlers::put_property(&comp, "sample_rate", request_body, "/test/path");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::OK_200);
        // Property values are serialized as strings
        REQUIRE(std::stoi(json_res["value"].get<std::string>()) == 96000);
        REQUIRE(cfg.sample_rate == 96000);
    }

    SECTION("Update initialize-only property fails") {
        auto request_body = nlohmann::json{{"value", 2048}};
        auto res = property_handlers::put_property(&comp, "buffer_size", request_body, "/test/path");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::Forbidden_403);
        REQUIRE(get_error_code(json_res) == "NOT_RUNTIME_CONFIGURABLE");
        REQUIRE(cfg.buffer_size == 1024); // Unchanged
    }

    SECTION("Update with missing value field") {
        auto request_body = nlohmann::json{{"something_else", 42}};
        auto res = property_handlers::put_property(&comp, "sample_rate", request_body, "/test/path");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::BadRequest_400);
        REQUIRE(get_error_code(json_res) == "MISSING_FIELD");
    }

    SECTION("Update boolean property") {
        auto request_body = nlohmann::json{{"value", false}};
        auto res = property_handlers::put_property(&comp, "enabled", request_body, "/test/path");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::OK_200);
        // Verify the API returns the correct value
        REQUIRE(json_res.contains("value"));
        REQUIRE(json_res["value"].get<std::string>() == "false");
    }

    SECTION("Update string property") {
        auto request_body = nlohmann::json{{"value", "new_name"}};
        auto res = property_handlers::put_property(&comp, "name", request_body, "/test/path");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::OK_200);
        REQUIRE(cfg.name == "new_name");
    }

    SECTION("Update float property") {
        auto request_body = nlohmann::json{{"value", 0.75}};
        auto res = property_handlers::put_property(&comp, "gain", request_body, "/test/path");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::OK_200);
        REQUIRE_THAT(cfg.gain, Catch::Matchers::WithinAbs(0.75, 0.01));
    }
}

TEST_CASE("REST API - Update Property (PATCH)") {
    auto comp = test_component("test_comp");
    auto& cfg = comp.get_config();

    SECTION("PATCH behaves same as PUT for simple properties") {
        auto request_body = nlohmann::json{{"value", 44100}};
        auto res = property_handlers::patch_property(&comp, "sample_rate", request_body, "/test/path");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::OK_200);
        // Property values are serialized as strings
        REQUIRE(std::stoi(json_res["value"].get<std::string>()) == 44100);
        REQUIRE(cfg.sample_rate == 44100);
    }
}

TEST_CASE("REST API - Delete Property (Reset to Default)") {
    auto comp = test_component("test_comp");
    auto& cfg = comp.get_config();

    SECTION("Reset runtime-configurable property") {
        // First set to non-default value
        cfg.sample_rate = 96000;

        // Then reset
        auto res = property_handlers::delete_property(&comp, "sample_rate", "/test/path");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::OK_200);
        REQUIRE(json_res["success"].get<bool>() == true);
        REQUIRE(cfg.sample_rate == 0); // Reset to default (0)
    }

    SECTION("Reset initialize-only property fails") {
        auto res = property_handlers::delete_property(&comp, "buffer_size", "/test/path");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::Forbidden_403);
        REQUIRE(get_error_code(json_res) == "NOT_RUNTIME_CONFIGURABLE");
    }

    SECTION("Reset optional property clears value") {
        cfg.timeout = 1000;
        REQUIRE(cfg.timeout.has_value());

        auto res = property_handlers::delete_property(&comp, "timeout", "/test/path");
        REQUIRE(res.status == httplib::OK_200);
        REQUIRE_FALSE(cfg.timeout.has_value());
    }
}

// ============================================================================
// REST API Handler Tests - Phase 2: List Operations
// ============================================================================

TEST_CASE("REST API - Get List Items") {
    auto comp = test_component("test_comp");
    auto& cfg = comp.get_config();
    cfg.channels = {"left", "right", "center"};

    SECTION("Get all list items") {
        auto res = property_handlers::get_list_items(&comp, "channels", "/test/path", "/test/base");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::OK_200);
        REQUIRE(json_res["name"].get<std::string>() == "channels");
        REQUIRE(json_res["type"].get<std::string>() == "[]string");
        REQUIRE(json_res["count"].get<size_t>() == 3);
        REQUIRE(json_res["items"].size() == 3);

        REQUIRE(json_res["items"][0]["index"].get<size_t>() == 0);
        REQUIRE(json_res["items"][0]["value"].get<std::string>() == "left");
        REQUIRE(json_res["items"][1]["value"].get<std::string>() == "right");
        REQUIRE(json_res["items"][2]["value"].get<std::string>() == "center");
    }

    SECTION("Get items from non-list property fails") {
        auto res = property_handlers::get_list_items(&comp, "sample_rate", "/test/path", "/test/base");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::MethodNotAllowed_405);
        REQUIRE(get_error_code(json_res) == "UNSUPPORTED_OPERATION");
    }
}

TEST_CASE("REST API - Get Single List Item") {
    auto comp = test_component("test_comp");
    auto& cfg = comp.get_config();
    cfg.sample_rates = {8000, 16000, 32000, 48000};

    SECTION("Get valid list item") {
        auto res = property_handlers::get_list_item(&comp, "sample_rates", 2, "/test/path");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::OK_200);
        REQUIRE(json_res["index"].get<size_t>() == 2);
        REQUIRE(std::stoi(json_res["value"].get<std::string>()) == 32000);
    }

    SECTION("Get out-of-bounds list item fails") {
        auto res = property_handlers::get_list_item(&comp, "sample_rates", 999, "/test/path");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::BadRequest_400);
        REQUIRE(get_error_code(json_res) == "INVALID_INDEX");
        REQUIRE(json_res["error"]["details"]["index"].get<size_t>() == 999);
        REQUIRE(json_res["error"]["details"]["list_size"].get<size_t>() == 4);
    }
}

TEST_CASE("REST API - Post List Item (Append)") {
    auto comp = test_component("test_comp");
    auto& cfg = comp.get_config();
    cfg.channels = {"left", "right"};

    SECTION("Append to list") {
        auto request_body = nlohmann::json{{"value", "center"}};
        auto res = property_handlers::post_list_item(&comp, "channels", request_body, "/test/path", "/test/base");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::Created_201);
        REQUIRE(json_res["success"].get<bool>() == true);
        REQUIRE(json_res["index"].get<size_t>() == 2);
        REQUIRE(json_res["value"].get<std::string>() == "center");
        REQUIRE(cfg.channels.size() == 3);
        REQUIRE(cfg.channels[2] == "center");
    }

    SECTION("Append to empty list") {
        cfg.channels.clear();
        auto request_body = nlohmann::json{{"value", "mono"}};
        auto res = property_handlers::post_list_item(&comp, "channels", request_body, "/test/path", "/test/base");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::Created_201);
        REQUIRE(json_res["index"].get<size_t>() == 0);
        REQUIRE(cfg.channels.size() == 1);
        REQUIRE(cfg.channels[0] == "mono");
    }
}

TEST_CASE("REST API - Put List Item (Update)") {
    auto comp = test_component("test_comp");
    auto& cfg = comp.get_config();
    cfg.thresholds = {0.1f, 0.5f, 0.9f};

    SECTION("Update list item") {
        auto request_body = nlohmann::json{{"value", 0.75}};
        auto res = property_handlers::put_list_item(&comp, "thresholds", 1, request_body, "/test/path");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::OK_200);
        REQUIRE(json_res["index"].get<size_t>() == 1);
        REQUIRE_THAT(cfg.thresholds[1], Catch::Matchers::WithinAbs(0.75, 0.01));
    }

    SECTION("Update out-of-bounds index fails") {
        auto request_body = nlohmann::json{{"value", 0.5}};
        auto res = property_handlers::put_list_item(&comp, "thresholds", 10, request_body, "/test/path");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::BadRequest_400);
        REQUIRE(get_error_code(json_res) == "INVALID_INDEX");
    }
}

TEST_CASE("REST API - Delete List Item") {
    auto comp = test_component("test_comp");
    auto& cfg = comp.get_config();
    cfg.channels = {"left", "right", "center", "subwoofer"};

    SECTION("Delete list item") {
        auto res = property_handlers::delete_list_item(&comp, "channels", 2, "/test/path");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::OK_200);
        REQUIRE(json_res["success"].get<bool>() == true);
        REQUIRE(json_res["previous_value"].get<std::string>() == "center");
        REQUIRE(cfg.channels.size() == 3);
        REQUIRE(cfg.channels[0] == "left");
        REQUIRE(cfg.channels[1] == "right");
        REQUIRE(cfg.channels[2] == "subwoofer");
    }

    SECTION("Delete out-of-bounds index fails") {
        auto res = property_handlers::delete_list_item(&comp, "channels", 999, "/test/path");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::BadRequest_400);
        REQUIRE(get_error_code(json_res) == "INVALID_INDEX");
    }
}

// ============================================================================
// REST API Handler Tests - Phase 3: Struct Operations
// ============================================================================

TEST_CASE("REST API - Get Struct Fields") {
    auto comp = test_component("test_comp");
    auto& cfg = comp.get_config();
    cfg.audio = {96000, 24, "flac"};

    SECTION("Get all struct fields") {
        auto res = property_handlers::get_struct_fields(&comp, "audio", "/test/path", "/test/base");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::OK_200);
        REQUIRE(json_res["name"].get<std::string>() == "audio");
        REQUIRE(json_res["type"].get<std::string>() == "struct");
        REQUIRE(json_res.contains("fields"));
        REQUIRE(json_res["count"].get<size_t>() == 3);

        auto& fields = json_res["fields"];
        REQUIRE(fields.contains("sample_rate"));
        REQUIRE(fields.contains("bit_depth"));
        REQUIRE(fields.contains("codec"));
    }

    SECTION("Get fields from non-struct property fails") {
        auto res = property_handlers::get_struct_fields(&comp, "sample_rate", "/test/path", "/test/base");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::MethodNotAllowed_405);
        REQUIRE(get_error_code(json_res) == "UNSUPPORTED_OPERATION");
    }
}

TEST_CASE("REST API - Get Single Struct Field") {
    auto comp = test_component("test_comp");
    auto& cfg = comp.get_config();
    cfg.audio = {48000, 16, "pcm"};

    SECTION("Get existing struct field") {
        auto res = property_handlers::get_struct_field(&comp, "audio", "codec", "/test/path");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::OK_200);
        REQUIRE(json_res["name"].get<std::string>() == "codec");
        REQUIRE(json_res["value"].get<std::string>() == "pcm");
    }

    SECTION("Get non-existent struct field") {
        auto res = property_handlers::get_struct_field(&comp, "audio", "does_not_exist", "/test/path");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::NotFound_404);
        REQUIRE(get_error_code(json_res) == "PROPERTY_NOT_FOUND");
    }
}

TEST_CASE("REST API - Patch Struct Field") {
    auto comp = test_component("test_comp");
    auto& cfg = comp.get_config();
    cfg.audio = {48000, 16, "pcm"};

    SECTION("Update struct field") {
        auto request_body = nlohmann::json{{"value", "mp3"}};
        auto res = property_handlers::patch_struct_field(&comp, "audio", "codec", request_body, "/test/path");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::OK_200);
        REQUIRE(json_res["value"].get<std::string>() == "mp3");
        REQUIRE(cfg.audio.codec == "mp3");
    }

    SECTION("Update integer struct field") {
        auto request_body = nlohmann::json{{"value", 96000}};
        auto res = property_handlers::patch_struct_field(&comp, "audio", "sample_rate", request_body, "/test/path");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::OK_200);
        REQUIRE(cfg.audio.sample_rate == 96000);
    }
}

TEST_CASE("REST API - Struct List Operations") {
    auto comp = test_component("test_comp");
    auto& cfg = comp.get_config();
    cfg.connections = {
        {1, "conn1", "host1", 8080, 1000},
        {2, "conn2", "host2", 9090, 2000}
    };

    SECTION("Get struct list item fields") {
        auto res = property_handlers::get_struct_list_item_fields(&comp, "connections", 0, "/test/path", "/test/base");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::OK_200);
        REQUIRE(json_res["index"].get<size_t>() == 0);
        REQUIRE(json_res.contains("fields"));
        REQUIRE(json_res["fields"].contains("host"));
        REQUIRE(json_res["fields"].contains("port"));
    }

    SECTION("Update struct list item field") {
        auto request_body = nlohmann::json{{"value", "newhost.com"}};
        auto res = property_handlers::patch_struct_list_item_field(&comp, "connections", 1, "host", request_body, "/test/path");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::OK_200);
        // Verify the API returns the updated value
        REQUIRE(json_res.contains("value"));
        REQUIRE(json_res["value"].get<std::string>() == "newhost.com");
    }
}

// ============================================================================
// REST API Handler Tests - Phase 4: Validation & Discovery
// ============================================================================

TEST_CASE("REST API - Get Property Schema") {
    auto comp = test_component("test_comp");

    SECTION("Get schema for property") {
        auto res = property_handlers::get_property_schema(&comp, "sample_rate", "/test/path");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::OK_200);
        REQUIRE(json_res["name"].get<std::string>() == "sample_rate");
        REQUIRE(json_res["type"].get<std::string>() == "int32");
        REQUIRE(json_res["configurability"].get<std::string>() == "runtime");
        REQUIRE(json_res.contains("units"));
    }

    SECTION("Get schema for non-existent property") {
        auto res = property_handlers::get_property_schema(&comp, "does_not_exist", "/test/path");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::NotFound_404);
        REQUIRE(get_error_code(json_res) == "PROPERTY_NOT_FOUND");
    }
}

TEST_CASE("REST API - Validate Property Value") {
    auto comp = test_component("test_comp");

    SECTION("Validate valid value") {
        auto request_body = nlohmann::json{{"value", 96000}};
        auto res = property_handlers::validate_property_value(&comp, "sample_rate", request_body, "/test/path");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::OK_200);
        REQUIRE(json_res["valid"].get<bool>() == true);
    }

    SECTION("Validate with missing value field") {
        auto request_body = nlohmann::json{{"something_else", 42}};
        auto res = property_handlers::validate_property_value(&comp, "sample_rate", request_body, "/test/path");
        auto json_res = parse_json_response(res);

        REQUIRE(res.status == httplib::BadRequest_400);
        REQUIRE(get_error_code(json_res) == "MISSING_FIELD");
    }
}

// ============================================================================
// Helper Function Tests
// ============================================================================

TEST_CASE("Helper Function - build_props_lists") {
    using namespace nlohmann::literals;

    auto json_config = R"({
        "test_key": "test_value",
        "test_null_key": null,
        "list_key[0]": "list_item_value",
        "test_list_key": ["1", "2", "3"],
        "test_struct_key": {
            "struct_key_1": "struct_value_1",
            "struct_key_2": "struct_value_2"
        },
        "test_struct_list_key[3]": {
            "struct_key_1": "struct_value_1",
            "struct_key_2": "struct_value_2"
        }
    })"_json;

    const auto& [props, list_props, struct_props] = build_props_lists(json_config);

    SECTION("Scalar properties parsed correctly") {
        REQUIRE(props.size() == 3);

        auto find_prop = [&](const std::string& key) -> std::optional<std::string> {
            for (const auto& [k, v] : props) {
                if (k == key) return v;
            }
            return std::nullopt;
        };

        REQUIRE(find_prop("test_key") == "test_value");
        REQUIRE(find_prop("test_null_key") == std::string{null_prop});
        REQUIRE(find_prop("list_key[0]") == "list_item_value");
    }

    SECTION("List properties parsed correctly") {
        REQUIRE(list_props.size() == 1);
        REQUIRE(list_props[0].first == "test_list_key");
        REQUIRE(list_props[0].second == std::vector<std::string>{"1", "2", "3"});
    }

    SECTION("Struct properties parsed correctly") {
        REQUIRE(struct_props.size() == 2);

        auto vec_contains = [](const auto& vec, const std::string& key) -> std::optional<std::size_t> {
            auto i = std::size_t{};
            for (const auto& [k, _] : vec) {
                if (k == key) {
                    return i;
                }
                ++i;
            }
            return std::nullopt;
        };

        auto prop_idx = vec_contains(struct_props, "test_struct_key");
        REQUIRE(prop_idx.has_value());
        REQUIRE(struct_props[*prop_idx].first == "test_struct_key");
        REQUIRE(struct_props[*prop_idx].second.size() == 2);
        auto internal_prop_idx = vec_contains(struct_props[*prop_idx].second, "struct_key_1");
        REQUIRE(internal_prop_idx.has_value());
        REQUIRE(struct_props[*prop_idx].second[*internal_prop_idx].second == "struct_value_1");
        prop_idx = vec_contains(struct_props, "test_struct_list_key[3]");
        REQUIRE(prop_idx.has_value());
        REQUIRE(struct_props[*prop_idx].first == "test_struct_list_key[3]");
        REQUIRE(struct_props[*prop_idx].second.size() == 2);
        internal_prop_idx = vec_contains(struct_props[*prop_idx].second, "struct_key_1");
        REQUIRE(internal_prop_idx.has_value());
        REQUIRE(struct_props[*prop_idx].second[*internal_prop_idx].second == "struct_value_1");
    }

    SECTION("build_props_lists handles nested structs") {
        using namespace nlohmann::literals;
        auto json_config = R"({
            "signal_overrides": {
                "center_frequency": "10e9",
                "sample_rate": "10e6",
                "data_format": {
                    "is_complex": "true"
                }
            }
        })"_json;
        const auto& [props, list_props, struct_props] = build_props_lists(json_config);
        REQUIRE(props.empty());
        REQUIRE(list_props.empty());
        REQUIRE(struct_props.size() == 1);
        REQUIRE(struct_props.front().first == "signal_overrides");
        REQUIRE(struct_props.front().second.size() == 3);
        auto vec_contains = [](const auto& vec, const std::string& key) -> std::optional<std::size_t> {
            auto i = std::size_t{};
            for (const auto& [k, _] : vec) {
                if (k == key) {
                    return i;
                }
                ++i;
            }
            return std::nullopt;
        };
        auto idx = vec_contains(struct_props.front().second, "center_frequency");
        REQUIRE(idx.has_value());
        REQUIRE(struct_props.front().second[*idx].second == "10e9");
        idx = vec_contains(struct_props.front().second, "sample_rate");
        REQUIRE(idx.has_value());
        REQUIRE(struct_props.front().second[*idx].second == "10e6");
        idx = vec_contains(struct_props.front().second, "data_format.is_complex");
        REQUIRE(idx.has_value());
        REQUIRE(struct_props.front().second[*idx].second == "true");
    }
}

// ============================================================================
// Error Response Tests
// ============================================================================

TEST_CASE("Error Response Format") {
    SECTION("Error codes map to correct HTTP status codes") {
        REQUIRE(error_response::http_status(error_code::PROPERTY_NOT_FOUND) == httplib::NotFound_404);
        REQUIRE(error_response::http_status(error_code::COMPONENT_NOT_FOUND) == httplib::NotFound_404);
        REQUIRE(error_response::http_status(error_code::NOT_RUNTIME_CONFIGURABLE) == httplib::Forbidden_403);
        REQUIRE(error_response::http_status(error_code::INVALID_TYPE) == httplib::BadRequest_400);
        REQUIRE(error_response::http_status(error_code::INVALID_INDEX) == httplib::BadRequest_400);
        REQUIRE(error_response::http_status(error_code::UNSUPPORTED_OPERATION) == httplib::MethodNotAllowed_405);
    }

    SECTION("Error codes convert to correct strings") {
        REQUIRE(error_response::error_code_to_string(error_code::PROPERTY_NOT_FOUND) == "PROPERTY_NOT_FOUND");
        REQUIRE(error_response::error_code_to_string(error_code::INVALID_TYPE) == "INVALID_TYPE");
        REQUIRE(error_response::error_code_to_string(error_code::NOT_RUNTIME_CONFIGURABLE) == "NOT_RUNTIME_CONFIGURABLE");
    }

    SECTION("Error response includes all required fields") {
        auto res = rest_helpers::error_response_obj(
            error_code::PROPERTY_NOT_FOUND,
            "Test error message",
            "/test/path",
            nlohmann::json{{"test_detail", "test_value"}}
        );
        auto json_res = parse_json_response(res);

        REQUIRE(json_res.contains("error"));
        REQUIRE(json_res["error"].contains("code"));
        REQUIRE(json_res["error"].contains("message"));
        REQUIRE(json_res["error"].contains("details"));
        REQUIRE(json_res["error"].contains("path"));
        REQUIRE(res.has_header("Access-Control-Allow-Origin"));
    }
}
