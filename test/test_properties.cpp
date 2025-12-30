/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#define CATCH_CONFIG_MAIN
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "composite/core/component.hpp"
#include "composite/properties/property_set.hpp"
#include "composite/properties/serialization.hpp"

using namespace composite;
using namespace composite::properties;

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
    std::string host{"localhost"};
    uint16_t port{8080};
    int32_t timeout{5000};
};

// ============================================================================
// Property Traits Specializations
// ============================================================================

template<>
struct composite::properties::property_traits<audio_config> {
    static void register_fields(property_set& ps, audio_config& cfg) {
        ps.add("sample_rate", cfg.sample_rate, config_type::RUNTIME, "Hz");
        ps.add("bit_depth", cfg.bit_depth, config_type::RUNTIME, "bits");
        ps.add("codec", cfg.codec, config_type::RUNTIME);
    }
};

template<>
struct composite::properties::property_traits<network_connection> {
    static void register_fields(property_set& ps, network_connection& cfg) {
        ps.add("id", cfg.id, config_type::RUNTIME);
        ps.add("host", cfg.host, config_type::RUNTIME);
        ps.add("port", cfg.port, config_type::RUNTIME);
        ps.add("timeout", cfg.timeout, config_type::RUNTIME, "ms");
    }
};

// ============================================================================
// Test Config Structure
// ============================================================================

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
};

// ============================================================================
// Test Component
// ============================================================================

class test_component : public component {
public:
    explicit test_component(std::string_view id = "test_comp") : component(id) {
        // Register scalar properties
        add_property("sample_rate", m_config.sample_rate, config_type::RUNTIME, "Hz");
        add_property("buffer_size", m_config.buffer_size, config_type::INITIALIZE, "samples");
        add_property("gain", m_config.gain, config_type::RUNTIME, "linear");
        add_property("threshold", m_config.threshold, config_type::RUNTIME, "linear");
        add_property("enabled", m_config.enabled, config_type::RUNTIME);
        add_property("name", m_config.name, config_type::RUNTIME);

        // Register optional properties
        add_property("timeout", m_config.timeout, config_type::RUNTIME, "ms");
        add_property("description", m_config.description, config_type::RUNTIME);

        // Register list properties
        add_property("channels", m_config.channels, config_type::RUNTIME);
        add_property("thresholds", m_config.thresholds, config_type::RUNTIME, "linear");
        add_property("sample_rates", m_config.sample_rates, config_type::RUNTIME, "Hz");

        // Register struct property
        add_property("audio", m_config.audio, config_type::RUNTIME);

        // Register struct list property
        add_property("connections", m_config.connections, config_type::RUNTIME);
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
// Property System Tests - Scalar Properties
// ============================================================================

TEST_CASE("Property System - Scalar Properties") {
    test_config cfg;
    property_set ps;

    ps.add("gain", cfg.gain);
    ps.add("sample_rate", cfg.sample_rate);
    ps.add("enabled", cfg.enabled);
    ps.add("name", cfg.name);

    SECTION("Get and set int32 property") {
        REQUIRE(ps.get<int32_t>("sample_rate") == 48000);
        ps.set("sample_rate", "96000");
        REQUIRE(cfg.sample_rate == 96000);
        REQUIRE(ps.get<int32_t>("sample_rate") == 96000);
    }

    SECTION("Get and set float property") {
        REQUIRE_THAT(ps.get<float>("gain"), Catch::Matchers::WithinAbs(1.0, 0.000001));
        ps.set("gain", "0.5");
        REQUIRE_THAT(cfg.gain, Catch::Matchers::WithinAbs(0.5, 0.000001));
    }

    SECTION("Get and set bool property") {
        REQUIRE(ps.get<bool>("enabled") == true);
        ps.set("enabled", "false");
        REQUIRE(cfg.enabled == false);
    }

    SECTION("Get and set string property") {
        REQUIRE(ps.get<std::string>("name") == "test");
        ps.set("name", "new_name");
        REQUIRE(cfg.name == "new_name");
    }

    SECTION("Reset property to default using null") {
        cfg.gain = 0.75f;
        ps.set("gain", std::string{null_value});
        REQUIRE_THAT(cfg.gain, Catch::Matchers::WithinAbs(0.0, 0.000001));
    }
}

TEST_CASE("Property System - Optional Properties") {
    test_config cfg;
    property_set ps;

    ps.add("timeout", cfg.timeout);
    ps.add("description", cfg.description);

    SECTION("Optional int32 property") {
        REQUIRE_FALSE(cfg.timeout.has_value());

        ps.set("timeout", "1000");
        REQUIRE(cfg.timeout.has_value());
        REQUIRE(cfg.timeout.value() == 1000);
        REQUIRE(ps.get_optional<int32_t>("timeout") == 1000);

        ps.set("timeout", std::string{null_value});
        REQUIRE_FALSE(cfg.timeout.has_value());
    }

    SECTION("Optional string property") {
        REQUIRE_FALSE(cfg.description.has_value());

        ps.set("description", "test description");
        REQUIRE(cfg.description.has_value());
        REQUIRE(cfg.description.value() == "test description");

        ps.set("description", std::string{null_value});
        REQUIRE_FALSE(cfg.description.has_value());
    }
}

TEST_CASE("Property System - List Properties") {
    test_config cfg;
    property_set ps;

    ps.add("channels", cfg.channels);
    ps.add("thresholds", cfg.thresholds);
    ps.add("sample_rates", cfg.sample_rates);

    SECTION("Append to list using set with append notation") {
        ps.set("channels[]", "left");
        REQUIRE(cfg.channels.size() == 1);
        ps.set("channels[]", "right");
        REQUIRE(cfg.channels.size() == 2);
        REQUIRE(cfg.channels[0] == "left");
        REQUIRE(cfg.channels[1] == "right");
    }

    SECTION("Get and set list item by index") {
        cfg.sample_rates = {8000, 16000, 32000};
        REQUIRE(ps.list_size("sample_rates") == 3);

        ps.set("sample_rates[1]", "48000");
        REQUIRE(cfg.sample_rates[1] == 48000);
    }

    SECTION("Erase list item using null") {
        cfg.channels = {"left", "right", "center"};
        ps.set("channels[0]", std::string{null_value});
        REQUIRE(cfg.channels.size() == 2);
        REQUIRE(cfg.channels[0] == "right");
        REQUIRE(cfg.channels[1] == "center");
    }

    SECTION("Append and erase operations") {
        auto idx = ps.append("channels", "mono");
        REQUIRE(idx == 0);
        REQUIRE(cfg.channels.size() == 1);
        REQUIRE(cfg.channels[0] == "mono");

        ps.append("channels", "stereo");
        REQUIRE(cfg.channels.size() == 2);

        ps.erase("channels", 0);
        REQUIRE(cfg.channels.size() == 1);
        REQUIRE(cfg.channels[0] == "stereo");
    }
}

TEST_CASE("Property System - Struct Properties") {
    test_config cfg;
    property_set ps;

    ps.add("audio", cfg.audio);

    SECTION("Get struct field using dot notation") {
        cfg.audio.sample_rate = 44100;
        cfg.audio.bit_depth = 24;
        cfg.audio.codec = "flac";

        // Access through nested property_set
        auto* prop = ps.find("audio");
        REQUIRE(prop != nullptr);
        REQUIRE(prop->is_struct());

        auto* nested = prop->nested();
        REQUIRE(nested != nullptr);
        REQUIRE(nested->get<int32_t>("sample_rate") == 44100);
        REQUIRE(nested->get<int16_t>("bit_depth") == 24);
        REQUIRE(nested->get<std::string>("codec") == "flac");
    }

    SECTION("Set struct field using dot notation") {
        ps.set("audio.sample_rate", "96000");
        REQUIRE(cfg.audio.sample_rate == 96000);

        ps.set("audio.codec", "mp3");
        REQUIRE(cfg.audio.codec == "mp3");
    }

    SECTION("Reset struct using null") {
        cfg.audio = {96000, 24, "flac"};
        ps.set("audio", std::string{null_value});
        // Struct reset restores default values
        REQUIRE(cfg.audio.sample_rate == 48000);
        REQUIRE(cfg.audio.bit_depth == 16);
        REQUIRE(cfg.audio.codec == "pcm");
    }
}

TEST_CASE("Property System - Struct List Properties") {
    test_config cfg;
    property_set ps;

    ps.add("connections", cfg.connections);

    SECTION("Append struct to list") {
        ps.set("connections[]", "");  // Append empty struct
        REQUIRE(cfg.connections.size() == 1);

        // Set fields on the new element
        ps.set("connections[0].host", "server.com");
        ps.set("connections[0].port", "9090");
        REQUIRE(cfg.connections[0].host == "server.com");
        REQUIRE(cfg.connections[0].port == 9090);
    }

    SECTION("Update struct list item field") {
        cfg.connections.push_back({1, "host1", 8080, 1000});

        ps.set("connections[0].host", "newhost.com");
        REQUIRE(cfg.connections[0].host == "newhost.com");
        REQUIRE(cfg.connections[0].port == 8080); // Unchanged
    }

    SECTION("Delete struct list item using null") {
        cfg.connections.push_back({1, "host1", 8080, 1000});
        cfg.connections.push_back({2, "host2", 9090, 2000});

        ps.set("connections[0]", std::string{null_value});
        REQUIRE(cfg.connections.size() == 1);
        REQUIRE(cfg.connections[0].id == 2);
    }
}

TEST_CASE("Property System - Configurability via Component") {
    test_component comp("test");
    auto& cfg = comp.get_config();

    SECTION("Runtime configurable property can be updated") {
        REQUIRE_NOTHROW(comp.set_properties({{"sample_rate", "96000"}}, config_type::RUNTIME));
        REQUIRE(cfg.sample_rate == 96000);
    }

    SECTION("Initialize-only property cannot be updated at runtime") {
        REQUIRE_THROWS_AS(
            comp.set_properties({{"buffer_size", "2048"}}, config_type::RUNTIME),
            config_error
        );
        REQUIRE(cfg.buffer_size == 1024); // Unchanged
    }

    SECTION("Initialize-only property can be set during initialization") {
        REQUIRE_NOTHROW(comp.set_properties({{"buffer_size", "2048"}}, config_type::INITIALIZE));
        REQUIRE(cfg.buffer_size == 2048);
    }
}

TEST_CASE("Property System - Nested Struct via Component") {
    test_component comp("nested_test");
    auto& cfg = comp.get_config();

    SECTION("Set nested scalar properties") {
        comp.set_properties({
            {"audio.sample_rate", "96000"},
            {"audio.bit_depth", "24"},
            {"audio.codec", "flac"}
        }, config_type::RUNTIME);

        REQUIRE(cfg.audio.sample_rate == 96000);
        REQUIRE(cfg.audio.bit_depth == 24);
        REQUIRE(cfg.audio.codec == "flac");
    }

    SECTION("Append and modify struct list") {
        // Append empty struct
        comp.set_properties({{"connections[]", ""}}, config_type::RUNTIME);
        REQUIRE(cfg.connections.size() == 1);

        // Set fields
        comp.set_properties({
            {"connections[0].host", "myserver.com"},
            {"connections[0].port", "9000"}
        }, config_type::RUNTIME);

        REQUIRE(cfg.connections[0].host == "myserver.com");
        REQUIRE(cfg.connections[0].port == 9000);
    }
}

// ============================================================================
// Property Serialization Tests
// ============================================================================

TEST_CASE("Property Serialization") {
    test_component comp("serialize_test");
    auto& cfg = comp.get_config();

    // Set up some values
    cfg.sample_rate = 96000;
    cfg.channels = {"left", "right", "center"};
    cfg.audio = {44100, 24, "flac"};
    cfg.connections.push_back({1, "host1", 8080, 1000});

    SECTION("Serialize all properties") {
        auto json = property_serializer::to_json(comp.property_set());

        REQUIRE(json.is_object());
        REQUIRE(json.contains("sample_rate"));
        REQUIRE(json.contains("channels"));
        REQUIRE(json.contains("audio"));
        REQUIRE(json.contains("connections"));
    }

    SECTION("Serialize single scalar property") {
        auto* prop = comp.property_set().find("sample_rate");
        REQUIRE(prop != nullptr);

        auto json = property_serializer::to_json(*prop, "sample_rate");
        REQUIRE(json["name"].get<std::string>() == "sample_rate");
        REQUIRE(json["type"].get<std::string>() == "int32");
        REQUIRE(json.contains("value"));
    }

    SECTION("Serialize list property") {
        auto* prop = comp.property_set().find("channels");
        REQUIRE(prop != nullptr);

        auto json = property_serializer::to_json(*prop, "channels");
        REQUIRE(json["name"].get<std::string>() == "channels");
        REQUIRE(json["type"].get<std::string>() == "[]string");
        REQUIRE(json["value"].is_array());
        REQUIRE(json["value"].size() == 3);
    }

    SECTION("Serialize struct property") {
        auto* prop = comp.property_set().find("audio");
        REQUIRE(prop != nullptr);

        auto json = property_serializer::to_json(*prop, "audio");
        REQUIRE(json["name"].get<std::string>() == "audio");
        REQUIRE(json["type"].get<std::string>() == "struct");
        REQUIRE(json["value"].is_object());
        REQUIRE(json["value"].contains("sample_rate"));
        REQUIRE(json["value"].contains("bit_depth"));
        REQUIRE(json["value"].contains("codec"));
    }

    SECTION("Serialize struct list property") {
        auto* prop = comp.property_set().find("connections");
        REQUIRE(prop != nullptr);

        auto json = property_serializer::to_json(*prop, "connections");
        REQUIRE(json["name"].get<std::string>() == "connections");
        REQUIRE(json["type"].get<std::string>() == "[]struct");
        REQUIRE(json["value"].is_array());
        REQUIRE(json["value"].size() == 1);
        REQUIRE(json["value"][0].contains("host"));
        REQUIRE(json["value"][0].contains("port"));
    }
}

// ============================================================================
// Error Handling Tests
// ============================================================================

TEST_CASE("Property System - Error Handling") {
    test_config cfg;
    property_set ps;

    ps.add("sample_rate", cfg.sample_rate);
    ps.add("name", cfg.name);

    SECTION("Key error for non-existent property") {
        REQUIRE_THROWS_AS(ps.get<int32_t>("nonexistent"), key_error);
        REQUIRE_THROWS_AS(ps.set("nonexistent", "value"), key_error);
    }

    SECTION("Value error for invalid conversion") {
        REQUIRE_THROWS_AS(ps.set("sample_rate", "not_a_number"), value_error);
    }

    SECTION("Index error for out-of-bounds list access") {
        std::vector<int32_t> values = {1, 2, 3};
        ps.add("values", values);

        REQUIRE_THROWS_AS(ps.set("values[10]", "42"), index_error);
        REQUIRE_THROWS_AS(ps.erase("values", 10), index_error);
    }
}

TEST_CASE("Property System - Change Listeners") {
    test_config cfg;
    property_set ps;

    ps.add("sample_rate", cfg.sample_rate);

    bool listener_called = false;
    int old_value = 0;

    SECTION("Listener called on change") {
        ps.add_change_listener("sample_rate", [&]() -> bool {
            listener_called = true;
            old_value = cfg.sample_rate;
            return true;
        });

        ps.set("sample_rate", "96000");
        REQUIRE(listener_called);
        // Note: listener is called after value is set
        REQUIRE(cfg.sample_rate == 96000);
    }

    SECTION("Listener can reject change") {
        ps.add_change_listener("sample_rate", [&]() -> bool {
            listener_called = true;
            return false;  // Reject the change
        });

        REQUIRE_THROWS_AS(ps.set("sample_rate", "96000"), listener_rejected);
        REQUIRE(listener_called);
        // Verify value is unchanged after rejection
        REQUIRE(cfg.sample_rate == 48000);
    }

    SECTION("Struct parent listener called on field update") {
        ps.add("audio", cfg.audio);

        int call_count = 0;
        ps.add_change_listener("audio", [&]() -> bool {
            call_count++;
            return true;
        });

        ps.set("audio.sample_rate", "96000");
        REQUIRE(call_count == 1);
        REQUIRE(cfg.audio.sample_rate == 96000);
    }

    SECTION("Struct parent listener can reject field update") {
        cfg.audio.sample_rate = 48000;
        ps.add("audio", cfg.audio);

        ps.add_change_listener("audio", [&]() -> bool {
            return false;
        });

        REQUIRE_THROWS_AS(ps.set("audio.sample_rate", "96000"), listener_rejected);
        REQUIRE(cfg.audio.sample_rate == 48000);
    }
}

TEST_CASE("Property System - Change Listener Rollback on Rejection") {
    test_config cfg;
    property_set ps;

    SECTION("Scalar property - value unchanged when listener rejects") {
        cfg.sample_rate = 48000;
        ps.add("sample_rate", cfg.sample_rate);

        bool listener_called = false;
        ps.add_change_listener("sample_rate", [&]() -> bool {
            listener_called = true;
            return false;  // Reject
        });

        REQUIRE_THROWS_AS(ps.set("sample_rate", "96000"), listener_rejected);
        REQUIRE(listener_called);
        REQUIRE(cfg.sample_rate == 48000);  // Value rolled back
    }

    SECTION("Optional property - value unchanged when listener rejects") {
        cfg.timeout = 1000;
        ps.add("timeout", cfg.timeout);

        bool listener_called = false;
        ps.add_change_listener("timeout", [&]() -> bool {
            listener_called = true;
            return false;  // Reject
        });

        REQUIRE_THROWS_AS(ps.set("timeout", "2000"), listener_rejected);
        REQUIRE(listener_called);
        REQUIRE(cfg.timeout == 1000);  // Value rolled back
    }

    SECTION("Optional property - nullopt unchanged when listener rejects") {
        cfg.timeout = std::nullopt;
        ps.add("timeout", cfg.timeout);

        ps.add_change_listener("timeout", [&]() -> bool {
            return false;  // Reject
        });

        REQUIRE_THROWS_AS(ps.set("timeout", "1000"), listener_rejected);
        REQUIRE_FALSE(cfg.timeout.has_value());  // Still nullopt
    }

    SECTION("List element - value unchanged when listener rejects") {
        cfg.channels = {"left", "right", "center"};
        ps.add("channels", cfg.channels);

        bool listener_called = false;
        ps.add_change_listener("channels", [&](std::size_t idx) -> bool {
            listener_called = true;
            return false;  // Reject
        });

        REQUIRE_THROWS_AS(ps.set("channels[1]", "surround"), listener_rejected);
        REQUIRE(listener_called);
        REQUIRE(cfg.channels[1] == "right");  // Value rolled back
    }

    SECTION("List deletion - value unchanged when listener rejects") {
        cfg.channels = {"left", "right", "center"};
        ps.add("channels", cfg.channels);

        ps.add_change_listener("channels", [&](std::size_t) -> bool {
            return false;  // Reject
        });

        REQUIRE_THROWS_AS(
            ps.set("channels[1]", std::string{null_value}),
            listener_rejected
        );
        REQUIRE(cfg.channels.size() == 3);
        REQUIRE(cfg.channels[1] == "right");
    }

    SECTION("Struct list listener called on field update") {
        cfg.connections.push_back({1, "host1", 8080, 1000});
        ps.add("connections", cfg.connections);

        int call_count = 0;
        std::size_t last_index = 0;
        ps.add_change_listener("connections", [&](std::size_t idx) -> bool {
            call_count++;
            last_index = idx;
            return true;
        });

        ps.set("connections[0].host", "newhost");
        REQUIRE(call_count == 1);
        REQUIRE(last_index == 0);
        REQUIRE(cfg.connections[0].host == "newhost");
    }

    SECTION("Struct list listener can reject field update") {
        cfg.connections.push_back({1, "host1", 8080, 1000});
        ps.add("connections", cfg.connections);

        ps.add_change_listener("connections", [&](std::size_t) -> bool {
            return false;
        });

        REQUIRE_THROWS_AS(ps.set("connections[0].host", "newhost"), listener_rejected);
        REQUIRE(cfg.connections[0].host == "host1");
    }

    SECTION("Struct list deletion - value unchanged when listener rejects") {
        cfg.connections.push_back({1, "host1", 8080, 1000});
        cfg.connections.push_back({2, "host2", 9090, 2000});
        ps.add("connections", cfg.connections);

        ps.add_change_listener("connections", [&](std::size_t) -> bool {
            return false;  // Reject
        });

        REQUIRE_THROWS_AS(
            ps.set("connections[0]", std::string{null_value}),
            listener_rejected
        );
        REQUIRE(cfg.connections.size() == 2);
        REQUIRE(cfg.connections[0].host == "host1");
    }

    SECTION("Boolean property - value unchanged when listener rejects") {
        cfg.enabled = true;
        ps.add("enabled", cfg.enabled);

        ps.add_change_listener("enabled", [&]() -> bool {
            return false;  // Reject
        });

        REQUIRE_THROWS_AS(ps.set("enabled", "false"), listener_rejected);
        REQUIRE(cfg.enabled == true);  // Value rolled back
    }

    SECTION("Float property - value unchanged when listener rejects") {
        cfg.gain = 1.0f;
        ps.add("gain", cfg.gain);

        ps.add_change_listener("gain", [&]() -> bool {
            return false;  // Reject
        });

        REQUIRE_THROWS_AS(ps.set("gain", "0.5"), listener_rejected);
        REQUIRE_THAT(cfg.gain, Catch::Matchers::WithinAbs(1.0, 0.0001));  // Value rolled back
    }

    SECTION("String property - value unchanged when listener rejects") {
        cfg.name = "original";
        ps.add("name", cfg.name);

        ps.add_change_listener("name", [&]() -> bool {
            return false;  // Reject
        });

        REQUIRE_THROWS_AS(ps.set("name", "modified"), listener_rejected);
        REQUIRE(cfg.name == "original");  // Value rolled back
    }
}

TEST_CASE("Property System - Batch Operations") {
    test_component comp("batch_test");
    auto& cfg = comp.get_config();

    SECTION("Set multiple properties at once") {
        comp.set_properties({
            {"sample_rate", "96000"},
            {"gain", "0.5"},
            {"name", "updated"}
        }, config_type::RUNTIME);

        REQUIRE(cfg.sample_rate == 96000);
        REQUIRE_THAT(cfg.gain, Catch::Matchers::WithinAbs(0.5, 0.01));
        REQUIRE(cfg.name == "updated");
    }

    SECTION("Allow unknown keys when specified") {
        REQUIRE_NOTHROW(
            comp.set_properties({{"unknown_key", "value"}}, config_type::RUNTIME, true)
        );
    }

    SECTION("Reject unknown keys by default") {
        REQUIRE_THROWS_AS(
            comp.set_properties({{"unknown_key", "value"}}, config_type::RUNTIME),
            key_error
        );
    }
}

// ============================================================================
// Batch Rollback Tests
// ============================================================================

TEST_CASE("Property System - Batch Rollback on Failure") {
    test_component comp("rollback_test");
    auto& cfg = comp.get_config();

    // Set initial values
    cfg.sample_rate = 48000;
    cfg.gain = 1.0f;
    cfg.name = "original";
    cfg.threshold = 0.5;

    SECTION("Rollback scalar properties on value_error") {
        // First property succeeds, second fails due to invalid conversion
        REQUIRE_THROWS_AS(
            comp.set_properties({
                {"sample_rate", "96000"},  // Valid
                {"gain", "not_a_float"}    // Invalid - causes rollback
            }, config_type::RUNTIME),
            value_error
        );

        // sample_rate should be rolled back to original
        REQUIRE(cfg.sample_rate == 48000);
        REQUIRE_THAT(cfg.gain, Catch::Matchers::WithinAbs(1.0, 0.001));
    }

    SECTION("Rollback scalar properties on key_error") {
        REQUIRE_THROWS_AS(
            comp.set_properties({
                {"sample_rate", "96000"},
                {"gain", "0.5"},
                {"nonexistent", "value"}  // Invalid key - causes rollback
            }, config_type::RUNTIME),
            key_error
        );

        // All properties should be rolled back
        REQUIRE(cfg.sample_rate == 48000);
        REQUIRE_THAT(cfg.gain, Catch::Matchers::WithinAbs(1.0, 0.001));
    }

    SECTION("Rollback on config_error (not runtime configurable)") {
        REQUIRE_THROWS_AS(
            comp.set_properties({
                {"sample_rate", "96000"},
                {"buffer_size", "2048"}  // Not runtime configurable
            }, config_type::RUNTIME),
            config_error
        );

        // sample_rate should be rolled back
        REQUIRE(cfg.sample_rate == 48000);
        REQUIRE(cfg.buffer_size == 1024);  // Unchanged
    }

    SECTION("Rollback multiple properties in correct order") {
        // Verify that many properties are all rolled back
        REQUIRE_THROWS_AS(
            comp.set_properties({
                {"sample_rate", "96000"},
                {"gain", "0.75"},
                {"threshold", "0.9"},
                {"name", "updated"},
                {"gain", "invalid"}  // Invalid - causes rollback
            }, config_type::RUNTIME),
            value_error
        );

        // All should be back to original
        REQUIRE(cfg.sample_rate == 48000);
        REQUIRE_THAT(cfg.gain, Catch::Matchers::WithinAbs(1.0, 0.001));
        REQUIRE_THAT(cfg.threshold, Catch::Matchers::WithinAbs(0.5, 0.001));
        REQUIRE(cfg.name == "original");
    }
}

TEST_CASE("Property System - Batch Rollback with Lists") {
    test_component comp("rollback_list_test");
    auto& cfg = comp.get_config();

    // Initialize with some list items
    cfg.channels = {"left", "right"};
    cfg.sample_rates = {44100, 48000};

    SECTION("Rollback list item modifications") {
        REQUIRE_THROWS_AS(
            comp.set_properties({
                {"channels[0]", "front_left"},
                {"sample_rates[0]", "invalid"}  // Invalid - causes rollback
            }, config_type::RUNTIME),
            value_error
        );

        // channels[0] should be rolled back
        REQUIRE(cfg.channels[0] == "left");
        REQUIRE(cfg.sample_rates[0] == 44100);
    }

    SECTION("Rollback list appends") {
        REQUIRE_THROWS_AS(
            comp.set_properties({
                {"channels[]", "center"},      // Append
                {"sample_rates[]", "invalid"}  // Invalid append - causes rollback
            }, config_type::RUNTIME),
            value_error
        );

        // The appended "center" should be removed
        REQUIRE(cfg.channels.size() == 2);
        REQUIRE(cfg.channels[0] == "left");
        REQUIRE(cfg.channels[1] == "right");
    }

    SECTION("Single-item deletion still works") {
        // Single deletions should work without rollback errors
        REQUIRE_NOTHROW(
            comp.set_properties({
                {"channels[0]", std::string{null_value}}
            }, config_type::RUNTIME)
        );

        REQUIRE(cfg.channels.size() == 1);
        REQUIRE(cfg.channels[0] == "right");
    }

    SECTION("Multi-item batch with deletion is rejected") {
        REQUIRE_THROWS_AS(
            comp.set_properties({
                {"sample_rate", "96000"},
                {"channels[0]", std::string{null_value}}  // Deletion in batch
            }, config_type::RUNTIME),
            value_error
        );

        // Nothing should have changed
        REQUIRE(cfg.sample_rate == 48000);
        REQUIRE(cfg.channels.size() == 2);
    }
}

TEST_CASE("Property System - Batch Rollback with Structs") {
    test_component comp("rollback_struct_test");
    auto& cfg = comp.get_config();

    // Initialize struct
    cfg.audio.sample_rate = 44100;
    cfg.audio.bit_depth = 16;
    cfg.audio.codec = "pcm";

    SECTION("Rollback nested struct field changes") {
        REQUIRE_THROWS_AS(
            comp.set_properties({
                {"audio.sample_rate", "96000"},
                {"audio.codec", "flac"},
                {"audio.bit_depth", "invalid"}  // Invalid - causes rollback
            }, config_type::RUNTIME),
            value_error
        );

        // All struct fields should be rolled back
        REQUIRE(cfg.audio.sample_rate == 44100);
        REQUIRE(cfg.audio.codec == "pcm");
        REQUIRE(cfg.audio.bit_depth == 16);
    }

    SECTION("Rollback mixed scalar and struct properties") {
        cfg.sample_rate = 48000;

        REQUIRE_THROWS_AS(
            comp.set_properties({
                {"sample_rate", "96000"},
                {"audio.sample_rate", "192000"},
                {"nonexistent", "value"}  // Invalid key - causes rollback
            }, config_type::RUNTIME),
            key_error
        );

        REQUIRE(cfg.sample_rate == 48000);
        REQUIRE(cfg.audio.sample_rate == 44100);
    }
}

TEST_CASE("Property System - Batch Rollback with Struct Lists") {
    test_component comp("rollback_struct_list_test");
    auto& cfg = comp.get_config();

    // Initialize struct list
    cfg.connections.push_back({1, "host1", 8080, 1000});
    cfg.connections.push_back({2, "host2", 9090, 2000});

    SECTION("Rollback struct list field modifications") {
        REQUIRE_THROWS_AS(
            comp.set_properties({
                {"connections[0].host", "newhost1"},
                {"connections[1].port", "invalid"}  // Invalid - causes rollback
            }, config_type::RUNTIME),
            value_error
        );

        // First connection's host should be rolled back
        REQUIRE(cfg.connections[0].host == "host1");
        REQUIRE(cfg.connections[1].port == 9090);
    }

    SECTION("Rollback struct list appends") {
        REQUIRE_THROWS_AS(
            comp.set_properties({
                {"connections[]", ""},             // Append new struct
                {"connections[2].host", "host3"},  // Set field on new struct
                {"nonexistent", "value"}           // Invalid - causes rollback
            }, config_type::RUNTIME),
            key_error
        );

        // Appended struct should be removed
        REQUIRE(cfg.connections.size() == 2);
    }
}

TEST_CASE("Property System - Batch Rollback with Change Listeners") {
    test_config cfg;
    property_set ps;

    cfg.sample_rate = 48000;
    cfg.gain = 1.0f;
    cfg.name = "original";

    ps.add("sample_rate", cfg.sample_rate);
    ps.add("gain", cfg.gain);
    ps.add("name", cfg.name);

    int listener_call_count = 0;

    SECTION("Rollback when listener rejects later property") {
        // Add listener that rejects changes to 'name'
        ps.add_change_listener("name", [&]() -> bool {
            listener_call_count++;
            return false;  // Reject
        });

        std::vector<std::pair<std::string, std::string>> batch = {
            {"sample_rate", "96000"},
            {"gain", "0.5"},
            {"name", "new_name"}  // Will be rejected by listener
        };

        REQUIRE_THROWS_AS(
            ps.set_batch(batch, config_type::INITIALIZE),
            listener_rejected
        );

        // All properties should be rolled back
        REQUIRE(cfg.sample_rate == 48000);
        REQUIRE_THAT(cfg.gain, Catch::Matchers::WithinAbs(1.0, 0.001));
        REQUIRE(cfg.name == "original");
        REQUIRE(listener_call_count == 1);
    }

    SECTION("Listeners not called during rollback") {
        int sample_rate_listener_calls = 0;

        ps.add_change_listener("sample_rate", [&]() -> bool {
            sample_rate_listener_calls++;
            return true;
        });

        // Add listener that rejects 'name'
        ps.add_change_listener("name", [&]() -> bool {
            return false;
        });

        std::vector<std::pair<std::string, std::string>> batch = {
            {"sample_rate", "96000"},  // Listener called once
            {"name", "new_name"}       // Rejected - triggers rollback
        };

        REQUIRE_THROWS_AS(
            ps.set_batch(batch, config_type::INITIALIZE),
            listener_rejected
        );

        // sample_rate listener called once during set, NOT during rollback
        REQUIRE(sample_rate_listener_calls == 1);
        REQUIRE(cfg.sample_rate == 48000);  // Rolled back
    }
}

TEST_CASE("Property System - Successful Batch Operations") {
    test_component comp("successful_batch_test");
    auto& cfg = comp.get_config();

    cfg.sample_rate = 48000;
    cfg.gain = 1.0f;
    cfg.channels = {"left", "right"};
    cfg.audio.sample_rate = 44100;

    SECTION("All properties updated when batch succeeds") {
        REQUIRE_NOTHROW(
            comp.set_properties({
                {"sample_rate", "96000"},
                {"gain", "0.5"},
                {"channels[]", "center"},
                {"audio.sample_rate", "192000"}
            }, config_type::RUNTIME)
        );

        REQUIRE(cfg.sample_rate == 96000);
        REQUIRE_THAT(cfg.gain, Catch::Matchers::WithinAbs(0.5, 0.001));
        REQUIRE(cfg.channels.size() == 3);
        REQUIRE(cfg.channels[2] == "center");
        REQUIRE(cfg.audio.sample_rate == 192000);
    }
}

TEST_CASE("Property System - Contextual Change Listeners") {
    test_config cfg;
    property_set ps;

    SECTION("Scalar property receives MODIFY context") {
        cfg.sample_rate = 48000;
        ps.add("sample_rate", cfg.sample_rate);

        change_type received_ctx{};
        ps.add_change_listener("sample_rate", [&](change_type ctx) -> bool {
            received_ctx = ctx;
            return true;
        });

        ps.set("sample_rate", "96000");
        REQUIRE(received_ctx == change_type::MODIFY);
    }

    SECTION("Scalar property receives RESET context") {
        cfg.sample_rate = 48000;
        ps.add("sample_rate", cfg.sample_rate);

        change_type received_ctx{};
        ps.add_change_listener("sample_rate", [&](change_type ctx) -> bool {
            received_ctx = ctx;
            return true;
        });

        ps.set("sample_rate", std::string{null_value});
        REQUIRE(received_ctx == change_type::RESET);
    }

    SECTION("Optional property receives SET context when setting from nullopt") {
        cfg.timeout = std::nullopt;
        ps.add("timeout", cfg.timeout);

        change_type received_ctx{};
        ps.add_change_listener("timeout", [&](change_type ctx) -> bool {
            received_ctx = ctx;
            return true;
        });

        ps.set("timeout", "1000");
        REQUIRE(received_ctx == change_type::SET);
    }

    SECTION("Optional property receives MODIFY context when changing value") {
        cfg.timeout = 1000;
        ps.add("timeout", cfg.timeout);

        change_type received_ctx{};
        ps.add_change_listener("timeout", [&](change_type ctx) -> bool {
            received_ctx = ctx;
            return true;
        });

        ps.set("timeout", "2000");
        REQUIRE(received_ctx == change_type::MODIFY);
    }

    SECTION("Optional property receives RESET context when setting to nullopt") {
        cfg.timeout = 1000;
        ps.add("timeout", cfg.timeout);

        change_type received_ctx{};
        ps.add_change_listener("timeout", [&](change_type ctx) -> bool {
            received_ctx = ctx;
            return true;
        });

        ps.set("timeout", std::string{null_value});
        REQUIRE(received_ctx == change_type::RESET);
    }

    SECTION("List receives SET context on append") {
        cfg.channels = {"left"};
        ps.add("channels", cfg.channels);

        std::size_t received_idx{};
        change_type received_ctx{};
        ps.add_change_listener("channels", [&](std::size_t idx, change_type ctx) -> bool {
            received_idx = idx;
            received_ctx = ctx;
            return true;
        });

        ps.set("channels[]", "right");
        REQUIRE(received_idx == 1);
        REQUIRE(received_ctx == change_type::SET);
    }

    SECTION("List receives MODIFY context on element update") {
        cfg.channels = {"left", "right"};
        ps.add("channels", cfg.channels);

        std::size_t received_idx{};
        change_type received_ctx{};
        ps.add_change_listener("channels", [&](std::size_t idx, change_type ctx) -> bool {
            received_idx = idx;
            received_ctx = ctx;
            return true;
        });

        ps.set("channels[0]", "surround");
        REQUIRE(received_idx == 0);
        REQUIRE(received_ctx == change_type::MODIFY);
    }

    SECTION("List receives RESET context on element deletion") {
        cfg.channels = {"left", "right"};
        ps.add("channels", cfg.channels);

        std::size_t received_idx{};
        change_type received_ctx{};
        ps.add_change_listener("channels", [&](std::size_t idx, change_type ctx) -> bool {
            received_idx = idx;
            received_ctx = ctx;
            return true;
        });

        ps.set("channels[0]", std::string{null_value});
        REQUIRE(received_idx == 0);
        REQUIRE(received_ctx == change_type::RESET);
    }

    SECTION("Struct receives MODIFY context on field update") {
        cfg.audio.sample_rate = 48000;
        ps.add("audio", cfg.audio);

        change_type received_ctx{};
        ps.add_change_listener("audio", [&](change_type ctx) -> bool {
            received_ctx = ctx;
            return true;
        });

        ps.set("audio.sample_rate", "96000");
        REQUIRE(received_ctx == change_type::MODIFY);
    }

    SECTION("Struct receives RESET context on reset") {
        cfg.audio.sample_rate = 96000;
        ps.add("audio", cfg.audio);

        change_type received_ctx{};
        ps.add_change_listener("audio", [&](change_type ctx) -> bool {
            received_ctx = ctx;
            return true;
        });

        ps.set("audio", std::string{null_value});
        REQUIRE(received_ctx == change_type::RESET);
    }

    SECTION("Contextual listener can reject based on change type") {
        cfg.timeout = std::nullopt;
        ps.add("timeout", cfg.timeout);

        // Only allow SET operations, reject MODIFY
        ps.add_change_listener("timeout", [&](change_type ctx) -> bool {
            return ctx == change_type::SET;
        });

        // SET should succeed
        REQUIRE_NOTHROW(ps.set("timeout", "1000"));
        REQUIRE(cfg.timeout == 1000);

        // MODIFY should be rejected
        REQUIRE_THROWS_AS(ps.set("timeout", "2000"), listener_rejected);
        REQUIRE(cfg.timeout == 1000);  // Value unchanged
    }

    SECTION("Fallback to simple listener when no contextual listener") {
        cfg.sample_rate = 48000;
        ps.add("sample_rate", cfg.sample_rate);

        bool simple_called = false;
        ps.add_change_listener("sample_rate", [&]() -> bool {
            simple_called = true;
            return true;
        });

        ps.set("sample_rate", "96000");
        REQUIRE(simple_called);
    }

    SECTION("Struct list receives SET context on append") {
        ps.add("connections", cfg.connections);

        std::size_t received_idx{};
        change_type received_ctx{};
        ps.add_change_listener("connections", [&](std::size_t idx, change_type ctx) -> bool {
            received_idx = idx;
            received_ctx = ctx;
            return true;
        });

        ps.set("connections[]", "");
        REQUIRE(received_idx == 0);
        REQUIRE(received_ctx == change_type::SET);
    }

    SECTION("Struct list receives MODIFY context on field update") {
        cfg.connections.push_back({1, "host1", 8080, 1000});
        ps.add("connections", cfg.connections);

        std::size_t received_idx{};
        change_type received_ctx{};
        ps.add_change_listener("connections", [&](std::size_t idx, change_type ctx) -> bool {
            received_idx = idx;
            received_ctx = ctx;
            return true;
        });

        ps.set("connections[0].host", "newhost");
        REQUIRE(received_idx == 0);
        REQUIRE(received_ctx == change_type::MODIFY);
    }
}
