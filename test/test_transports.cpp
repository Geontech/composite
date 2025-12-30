/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "composite/buffers/buffer.hpp"
#include "composite/core/component.hpp"
#include "composite/ports/input_port.hpp"
#include "composite/ports/output_port.hpp"
#include "composite/transports/transport.hpp"
#include "helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

using namespace composite;
using namespace Catch::Matchers;

// =============================================================================
// Transport Type Enum Tests
// =============================================================================

TEST_CASE("transport_type enum to_string conversions", "[transport][enum]") {
    SECTION("nats type converts correctly") {
        REQUIRE(to_string(transport_type::nats) == "nats");
    }

    SECTION("to_string is constexpr") {
        constexpr auto nats_str = to_string(transport_type::nats);
        REQUIRE(nats_str == "nats");
    }
}

TEST_CASE("transport_type enum from_string conversions", "[transport][enum]") {
    SECTION("valid nats string converts correctly") {
        auto result = from_string("nats");
        REQUIRE(result.has_value());
        REQUIRE(*result == transport_type::nats);
    }

    SECTION("invalid string returns nullopt") {
        auto result = from_string("invalid");
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("empty string returns nullopt") {
        auto result = from_string("");
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("case sensitive - uppercase fails") {
        auto result = from_string("NATS");
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("future transport types return nullopt") {
        REQUIRE_FALSE(from_string("zeromq").has_value());
        REQUIRE_FALSE(from_string("udp").has_value());
        REQUIRE_FALSE(from_string("tcp").has_value());
        REQUIRE_FALSE(from_string("websocket").has_value());
    }
}

TEST_CASE("transport_type enum round-trip conversions", "[transport][enum]") {
    SECTION("enum -> string -> enum preserves value") {
        auto original = transport_type::nats;
        auto str = to_string(original);
        auto result = from_string(str);
        REQUIRE(result.has_value());
        REQUIRE(*result == original);
    }
}

// =============================================================================
// Transport Parsing Tests
// =============================================================================

TEST_CASE("parse_transports with valid single transport", "[transport][parsing]") {
    auto json = nlohmann::json::parse(R"([
        {
            "id": "nats_main",
            "type": "nats",
            "url": "nats://localhost:4222",
            "subject": "sensors.temp"
        }
    ])");

    auto [registry, error] = parse_transports(json);

    REQUIRE(error.empty());
    REQUIRE(registry.size() == 1);
    REQUIRE(registry.contains("nats_main"));

    auto& def = registry["nats_main"];
    REQUIRE(def.id == "nats_main");
    REQUIRE(def.type == transport_type::nats);
    REQUIRE(def.config["url"] == "nats://localhost:4222");
    REQUIRE(def.config["subject"] == "sensors.temp");
}

TEST_CASE("parse_transports with multiple transports", "[transport][parsing]") {
    auto json = nlohmann::json::parse(R"([
        {
            "id": "nats_primary",
            "type": "nats",
            "url": "nats://primary:4222",
            "subject": "data.primary"
        },
        {
            "id": "nats_backup",
            "type": "nats",
            "url": "nats://backup:4222",
            "subject": "data.backup"
        }
    ])");

    auto [registry, error] = parse_transports(json);

    REQUIRE(error.empty());
    REQUIRE(registry.size() == 2);
    REQUIRE(registry.contains("nats_primary"));
    REQUIRE(registry.contains("nats_backup"));
}

TEST_CASE("parse_transports validation errors", "[transport][parsing][error]") {
    SECTION("not an array fails") {
        auto json = nlohmann::json::parse(R"({"not": "array"})");
        auto [registry, error] = parse_transports(json);
        REQUIRE_FALSE(error.empty());
        REQUIRE_THAT(error, ContainsSubstring("must be an array"));
    }

    SECTION("missing id field fails") {
        auto json = nlohmann::json::parse(R"([
            {
                "type": "nats",
                "url": "nats://localhost:4222"
            }
        ])");
        auto [registry, error] = parse_transports(json);
        REQUIRE_FALSE(error.empty());
        REQUIRE_THAT(error, ContainsSubstring("missing 'id' field"));
    }

    SECTION("missing type field fails") {
        auto json = nlohmann::json::parse(R"([
            {
                "id": "test",
                "url": "nats://localhost:4222"
            }
        ])");
        auto [registry, error] = parse_transports(json);
        REQUIRE_FALSE(error.empty());
        REQUIRE_THAT(error, ContainsSubstring("missing 'type' field"));
    }

    SECTION("duplicate id fails") {
        auto json = nlohmann::json::parse(R"([
            {
                "id": "duplicate",
                "type": "nats",
                "url": "nats://server1:4222",
                "subject": "test1"
            },
            {
                "id": "duplicate",
                "type": "nats",
                "url": "nats://server2:4222",
                "subject": "test2"
            }
        ])");
        auto [registry, error] = parse_transports(json);
        REQUIRE_FALSE(error.empty());
        REQUIRE_THAT(error, ContainsSubstring("duplicate transport id"));
        REQUIRE_THAT(error, ContainsSubstring("duplicate"));
    }

    SECTION("unknown transport type fails") {
        auto json = nlohmann::json::parse(R"([
            {
                "id": "test",
                "type": "invalid_type",
                "url": "invalid://localhost:9999"
            }
        ])");
        auto [registry, error] = parse_transports(json);
        REQUIRE_FALSE(error.empty());
        REQUIRE_THAT(error, ContainsSubstring("unknown transport type"));
        REQUIRE_THAT(error, ContainsSubstring("invalid_type"));
    }
}

TEST_CASE("parse_transports with empty array", "[transport][parsing]") {
    auto json = nlohmann::json::parse("[]");
    auto [registry, error] = parse_transports(json);
    REQUIRE(error.empty());
    REQUIRE(registry.empty());
}

// =============================================================================
// Transport Creation Tests
// =============================================================================

TEST_CASE("create_transport validation", "[transport][creation][error]") {
    SECTION("unknown transport type fails") {
        transport_definition def;
        def.id = "test";
        def.type = static_cast<transport_type>(999); // Invalid enum value
        def.config = nlohmann::json::object();

        auto [transport, error] = create_transport(def);
        REQUIRE(transport == nullptr);
        REQUIRE_FALSE(error.empty());
    }

#ifdef COMPOSITE_USE_NATS
    SECTION("NATS transport missing url field fails") {
        transport_definition def;
        def.id = "nats_test";
        def.type = transport_type::nats;
        def.config = nlohmann::json::parse(R"({
            "id": "nats_test",
            "type": "nats",
            "subject": "test.subject"
        })");

        auto [transport, error] = create_transport(def);
        REQUIRE(transport == nullptr);
        REQUIRE_THAT(error, ContainsSubstring("missing 'url' field"));
    }

    SECTION("NATS transport missing subject field fails") {
        transport_definition def;
        def.id = "nats_test";
        def.type = transport_type::nats;
        def.config = nlohmann::json::parse(R"({
            "id": "nats_test",
            "type": "nats",
            "url": "nats://localhost:4222"
        })");

        auto [transport, error] = create_transport(def);
        REQUIRE(transport == nullptr);
        REQUIRE_THAT(error, ContainsSubstring("missing 'subject' field"));
    }
#else
    SECTION("NATS transport without NATS support fails") {
        transport_definition def;
        def.id = "nats_test";
        def.type = transport_type::nats;
        def.config = nlohmann::json::parse(R"({
            "id": "nats_test",
            "type": "nats",
            "url": "nats://localhost:4222",
            "subject": "test.subject"
        })");

        auto [transport, error] = create_transport(def);
        REQUIRE(transport == nullptr);
        REQUIRE_THAT(error, ContainsSubstring("NATS support not enabled"));
    }
#endif
}

// =============================================================================
// Mock Component for Transport Attachment Tests
// =============================================================================

class mock_component : public component {
public:
    output_port<immutable_buffer<int>> output{"output"};
    input_port<immutable_buffer<int>> input{"input"};

    mock_component() : component("mock") {
        add_port(output);
        add_port(input);
    }

    auto process() -> retval override {
        return retval::NOOP;
    }
};

// =============================================================================
// Transport Attachment Tests
// =============================================================================

TEST_CASE("attach_component_transports validation", "[transport][attachment][error]") {
    auto comp = std::make_shared<mock_component>();
    transport_registry registry;

    SECTION("transports field not an object fails") {
        auto json = nlohmann::json::parse(R"(["not", "an", "object"])");
        auto error = attach_component_transports(comp, json, registry);
        REQUIRE_FALSE(error.empty());
        REQUIRE_THAT(error, ContainsSubstring("must be an object"));
    }

    SECTION("transport IDs not an array fails") {
        auto json = nlohmann::json::parse(R"({
            "output": "not_an_array"
        })");
        auto error = attach_component_transports(comp, json, registry);
        REQUIRE_FALSE(error.empty());
        REQUIRE_THAT(error, ContainsSubstring("must be an array"));
    }

    SECTION("transport ID not a string fails") {
        auto json = nlohmann::json::parse(R"({
            "output": [123]
        })");
        auto error = attach_component_transports(comp, json, registry);
        REQUIRE_FALSE(error.empty());
        REQUIRE_THAT(error, ContainsSubstring("must be a string"));
    }

    SECTION("unknown transport ID fails") {
        auto json = nlohmann::json::parse(R"({
            "output": ["unknown_transport"]
        })");
        auto error = attach_component_transports(comp, json, registry);
        REQUIRE_FALSE(error.empty());
        REQUIRE_THAT(error, ContainsSubstring("unknown transport"));
        REQUIRE_THAT(error, ContainsSubstring("unknown_transport"));
    }

    SECTION("unknown port name fails") {
        auto json = nlohmann::json::parse(R"({
            "nonexistent_port": ["some_transport"]
        })");
        auto error = attach_component_transports(comp, json, registry);
        REQUIRE_FALSE(error.empty());
        REQUIRE_THAT(error, ContainsSubstring("no port named"));
        REQUIRE_THAT(error, ContainsSubstring("nonexistent_port"));
    }

    SECTION("input port transport not yet supported") {
        auto json = nlohmann::json::parse(R"({
            "input": ["some_transport"]
        })");

        // Add a dummy transport to registry so port lookup happens
        transport_definition def;
        def.id = "some_transport";
        def.type = transport_type::nats;
        def.config = nlohmann::json::object();
        registry["some_transport"] = def;

        auto error = attach_component_transports(comp, json, registry);
        REQUIRE_FALSE(error.empty());
        REQUIRE_THAT(error, ContainsSubstring("input port"));
        REQUIRE_THAT(error, ContainsSubstring("not yet supported"));
    }
}

TEST_CASE("attach_component_transports with empty config", "[transport][attachment]") {
    auto comp = std::make_shared<mock_component>();
    transport_registry registry;
    auto json = nlohmann::json::parse("{}");

    auto error = attach_component_transports(comp, json, registry);
    REQUIRE(error.empty());
}

// =============================================================================
// Integration Tests
// =============================================================================

TEST_CASE("transport system integration - parse and validate", "[transport][integration]") {
    // Parse transport definitions
    auto transports_json = nlohmann::json::parse(R"([
        {
            "id": "nats_main",
            "type": "nats",
            "url": "nats://localhost:4222",
            "subject": "test.data"
        },
        {
            "id": "nats_backup",
            "type": "nats",
            "url": "nats://backup:4222",
            "subject": "test.data.backup"
        }
    ])");

    auto [registry, parse_error] = parse_transports(transports_json);
    REQUIRE(parse_error.empty());
    REQUIRE(registry.size() == 2);

    // Create mock component
    auto comp = std::make_shared<mock_component>();

    // Validate attachment configuration (without actually attaching since we'd need real NATS)
    auto attach_json = nlohmann::json::parse(R"({
        "output": ["nats_main", "nats_backup"]
    })");

    // This will fail at creation time if NATS is not available, but validation should pass
    auto attach_error = attach_component_transports(comp, attach_json, registry);

#ifdef COMPOSITE_USE_NATS
    // If NATS is enabled, we might fail on connection (which is expected in test environment)
    // or succeed if NATS server is running
    INFO("NATS support is enabled, attachment attempted");
#else
    // Without NATS support, we should get an error about NATS not being enabled
    REQUIRE_FALSE(attach_error.empty());
    REQUIRE_THAT(attach_error, ContainsSubstring("NATS support not enabled"));
#endif
}

TEST_CASE("transport definition preserves all config fields", "[transport][integration]") {
    auto json = nlohmann::json::parse(R"([
        {
            "id": "nats_test",
            "type": "nats",
            "url": "nats://example.com:4222",
            "subject": "custom.subject",
            "custom_field1": "value1",
            "custom_field2": 42,
            "custom_obj": {
                "nested": true
            }
        }
    ])");

    auto [registry, error] = parse_transports(json);
    REQUIRE(error.empty());
    REQUIRE(registry.size() == 1);

    auto& def = registry["nats_test"];
    REQUIRE(def.config["custom_field1"] == "value1");
    REQUIRE(def.config["custom_field2"] == 42);
    REQUIRE(def.config["custom_obj"]["nested"] == true);
}
