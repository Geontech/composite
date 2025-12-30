/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "composite/core/application.hpp"
#include "composite/core/component.hpp"
#include "property_handlers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <thread>
#include <chrono>

using namespace composite;
using namespace std::chrono_literals;

using composite::properties::config_type;

namespace {

// ============================================================================
// Test Configuration Structures with property_traits
// ============================================================================

struct network_config {
    std::string host{"localhost"};
    uint16_t port{8080};
};

} // anonymous namespace

// property_traits specialization for network_config
template<>
struct composite::properties::property_traits<network_config> {
    static void register_fields(property_set& ps, network_config& cfg) {
        ps.add("host", cfg.host, config_type::RUNTIME);
        ps.add("port", cfg.port, config_type::RUNTIME);
    }
};

namespace {

struct test_config {
    int32_t sample_rate{48000};
    int32_t buffer_size{1024};
    bool enabled{true};
    std::string name{"test_component"};
    std::vector<std::string> channels{"left", "right"};
    network_config network;
    std::vector<network_config> connections{
        {"host1", 9001},
        {"host2", 9002}
    };
};

} // anonymous namespace

// property_traits specialization for test_config
template<>
struct composite::properties::property_traits<test_config> {
    static void register_fields(composite::properties::property_set& ps, test_config& cfg) {
        ps.add("sample_rate", cfg.sample_rate, config_type::RUNTIME, "Hz");
        ps.add("buffer_size", cfg.buffer_size, config_type::INITIALIZE, "samples");
        ps.add("enabled", cfg.enabled, config_type::RUNTIME);
        ps.add("name", cfg.name, config_type::RUNTIME);
        ps.add("channels", cfg.channels, config_type::RUNTIME);
        ps.add("network", cfg.network, config_type::RUNTIME);
        ps.add("connections", cfg.connections, config_type::RUNTIME);
    }
};

namespace {

// Test component with various property types
class test_component : public component {
public:
    test_component(std::string_view id) : component(id) {
        // Register all properties using the property_traits system
        add_property("sample_rate", m_config.sample_rate, config_type::RUNTIME, "Hz");
        add_property("buffer_size", m_config.buffer_size, config_type::INITIALIZE, "samples");
        add_property("enabled", m_config.enabled, config_type::RUNTIME);
        add_property("name", m_config.name, config_type::RUNTIME);
        add_property("channels", m_config.channels, config_type::RUNTIME);
        add_property("network", m_config.network, config_type::RUNTIME);
        add_property("connections", m_config.connections, config_type::RUNTIME);
    }

    auto process() -> retval override {
        return retval::FINISH;
    }

    auto get_config() -> test_config& {
        return m_config;
    }

private:
    test_config m_config;
};

// Server fixture that starts an HTTP server with a test component
class test_server {
public:
    test_server() : m_port(18080), m_server(std::make_unique<httplib::Server>()) {
        // Create test application with one component
        auto comp = std::make_shared<test_component>("test_comp");
        m_app.add_component(comp);

        // Register property REST API endpoints
        setup_property_endpoints();

        // Start server in background thread
        m_server_thread = std::thread([this]() {
            m_server->listen("localhost", m_port);
        });

        // Wait for server to be ready
        std::this_thread::sleep_for(100ms);
    }

    ~test_server() {
        m_server->stop();
        if (m_server_thread.joinable()) {
            m_server_thread.join();
        }
    }

    auto port() const -> int { return m_port; }
    auto base_url() const -> std::string {
        return std::format("http://localhost:{}", m_port);
    }

    // Return paths only (not full URLs) for use with httplib::Client
    auto component_path(std::string_view comp_id) const -> std::string {
        return std::format("/app/components/{}", comp_id);
    }

    auto property_path(std::string_view comp_id, std::string_view prop_name) const -> std::string {
        return std::format("/app/components/{}/properties/{}", comp_id, prop_name);
    }

private:
    void setup_property_endpoints() {
        const std::string APP = "app";
        const std::string COMPONENTS = "components";

        // GET /app/components/:id/properties
        auto endpoint = std::format("/{}/{}/:id/properties", APP, COMPONENTS);
        m_server->Get(endpoint, [this](const httplib::Request& req, httplib::Response& res) {
            auto comp_id = req.path_params.at("id");
            auto comp = m_app.get_component(comp_id);
            if (!comp) {
                property_handlers::error(res, "Component not found", 404);
                return;
            }
            property_handlers::get_properties(comp.get(), res);
        });

        // GET /app/components/:id/properties/:name
        endpoint = std::format("/{}/{}/:id/properties/:name", APP, COMPONENTS);
        m_server->Get(endpoint, [this](const httplib::Request& req, httplib::Response& res) {
            auto comp_id = req.path_params.at("id");
            auto prop_name = req.path_params.at("name");
            auto comp = m_app.get_component(comp_id);
            if (!comp) {
                property_handlers::error(res, "Component not found", 404);
                return;
            }
            property_handlers::get_property(comp.get(), prop_name, res);
        });

        // PUT /app/components/:id/properties/:name
        endpoint = std::format("/{}/{}/:id/properties/:name", APP, COMPONENTS);
        m_server->Put(endpoint, [this](const httplib::Request& req, httplib::Response& res) {
            auto comp_id = req.path_params.at("id");
            auto prop_name = req.path_params.at("name");
            auto comp = m_app.get_component(comp_id);
            if (!comp) {
                property_handlers::error(res, "Component not found", 404);
                return;
            }

            auto request_body = nlohmann::json::parse(req.body, nullptr, false);
            if (request_body.is_discarded()) {
                property_handlers::error(res, "Invalid JSON", 400);
                return;
            }

            property_handlers::put_property(comp.get(), prop_name, request_body, res);
        });

        // DELETE /app/components/:id/properties/:name
        endpoint = std::format("/{}/{}/:id/properties/:name", APP, COMPONENTS);
        m_server->Delete(endpoint, [this](const httplib::Request& req, httplib::Response& res) {
            auto comp_id = req.path_params.at("id");
            auto prop_name = req.path_params.at("name");
            auto comp = m_app.get_component(comp_id);
            if (!comp) {
                property_handlers::error(res, "Component not found", 404);
                return;
            }

            property_handlers::delete_property(comp.get(), prop_name, res);
        });

        // List operations
        // GET /app/components/:id/properties/:name/items
        endpoint = std::format("/{}/{}/:id/properties/:name/items", APP, COMPONENTS);
        m_server->Get(endpoint, [this, APP, COMPONENTS](const httplib::Request& req, httplib::Response& res) {
            auto comp_id = req.path_params.at("id");
            auto prop_name = req.path_params.at("name");
            auto comp = m_app.get_component(comp_id);
            if (!comp) {
                property_handlers::error(res, "Component not found", 404);
                return;
            }

            auto base_href = std::format("/{}/{}/{}/properties/{}/items", APP, COMPONENTS, comp_id, prop_name);
            property_handlers::get_list_items(comp.get(), prop_name, base_href, res);
        });

        // GET /app/components/:id/properties/:name/items/:index
        endpoint = std::format("/{}/{}/:id/properties/:name/items/:index", APP, COMPONENTS);
        m_server->Get(endpoint, [this](const httplib::Request& req, httplib::Response& res) {
            auto comp_id = req.path_params.at("id");
            auto prop_name = req.path_params.at("name");
            auto index_str = req.path_params.at("index");
            auto comp = m_app.get_component(comp_id);
            if (!comp) {
                property_handlers::error(res, "Component not found", 404);
                return;
            }

            size_t index = std::stoull(index_str);
            property_handlers::get_list_item(comp.get(), prop_name, index, res);
        });

        // POST /app/components/:id/properties/:name/items
        m_server->Post(std::format("/{}/{}/:id/properties/:name/items", APP, COMPONENTS),
            [this](const httplib::Request& req, httplib::Response& res) {
            auto comp_id = req.path_params.at("id");
            auto prop_name = req.path_params.at("name");
            auto comp = m_app.get_component(comp_id);
            if (!comp) {
                property_handlers::error(res, "Component not found", 404);
                return;
            }

            auto request_body = nlohmann::json::parse(req.body, nullptr, false);
            if (request_body.is_discarded()) {
                property_handlers::error(res, "Invalid JSON", 400);
                return;
            }

            property_handlers::post_list_item(comp.get(), prop_name, request_body, res);
        });

        // PUT /app/components/:id/properties/:name/items/:index
        m_server->Put(std::format("/{}/{}/:id/properties/:name/items/:index", APP, COMPONENTS),
            [this](const httplib::Request& req, httplib::Response& res) {
            auto comp_id = req.path_params.at("id");
            auto prop_name = req.path_params.at("name");
            auto index_str = req.path_params.at("index");
            auto comp = m_app.get_component(comp_id);
            if (!comp) {
                property_handlers::error(res, "Component not found", 404);
                return;
            }

            auto request_body = nlohmann::json::parse(req.body, nullptr, false);
            if (request_body.is_discarded()) {
                property_handlers::error(res, "Invalid JSON", 400);
                return;
            }

            size_t index = std::stoull(index_str);
            property_handlers::put_list_item(comp.get(), prop_name, index, request_body, res);
        });

        // DELETE /app/components/:id/properties/:name/items/:index
        m_server->Delete(std::format("/{}/{}/:id/properties/:name/items/:index", APP, COMPONENTS),
            [this](const httplib::Request& req, httplib::Response& res) {
            auto comp_id = req.path_params.at("id");
            auto prop_name = req.path_params.at("name");
            auto index_str = req.path_params.at("index");
            auto comp = m_app.get_component(comp_id);
            if (!comp) {
                property_handlers::error(res, "Component not found", 404);
                return;
            }

            size_t index = std::stoull(index_str);
            property_handlers::delete_list_item(comp.get(), prop_name, index, res);
        });

        // Struct operations
        // GET /app/components/:id/properties/:name/fields
        m_server->Get(std::format("/{}/{}/:id/properties/:name/fields", APP, COMPONENTS),
            [this, APP, COMPONENTS](const httplib::Request& req, httplib::Response& res) {
            auto comp_id = req.path_params.at("id");
            auto prop_name = req.path_params.at("name");
            auto comp = m_app.get_component(comp_id);
            if (!comp) {
                property_handlers::error(res, "Component not found", 404);
                return;
            }

            auto base_href = std::format("/{}/{}/{}/properties/{}/fields", APP, COMPONENTS, comp_id, prop_name);
            property_handlers::get_struct_fields(comp.get(), prop_name, base_href, res);
        });

        // GET /app/components/:id/properties/:name/fields/:field
        m_server->Get(std::format("/{}/{}/:id/properties/:name/fields/:field", APP, COMPONENTS),
            [this](const httplib::Request& req, httplib::Response& res) {
            auto comp_id = req.path_params.at("id");
            auto prop_name = req.path_params.at("name");
            auto field_name = req.path_params.at("field");
            auto comp = m_app.get_component(comp_id);
            if (!comp) {
                property_handlers::error(res, "Component not found", 404);
                return;
            }

            property_handlers::get_struct_field(comp.get(), prop_name, field_name, res);
        });

        // PATCH /app/components/:id/properties/:name/fields/:field
        m_server->Patch(std::format("/{}/{}/:id/properties/:name/fields/:field", APP, COMPONENTS),
            [this](const httplib::Request& req, httplib::Response& res) {
            auto comp_id = req.path_params.at("id");
            auto prop_name = req.path_params.at("name");
            auto field_name = req.path_params.at("field");
            auto comp = m_app.get_component(comp_id);
            if (!comp) {
                property_handlers::error(res, "Component not found", 404);
                return;
            }

            auto request_body = nlohmann::json::parse(req.body, nullptr, false);
            if (request_body.is_discarded()) {
                property_handlers::error(res, "Invalid JSON", 400);
                return;
            }

            property_handlers::patch_struct_field(comp.get(), prop_name, field_name, request_body, res);
        });

        // Struct list operations
        // GET /app/components/:id/properties/:name/items/:index/fields
        m_server->Get(std::format("/{}/{}/:id/properties/:name/items/:index/fields", APP, COMPONENTS),
            [this, APP, COMPONENTS](const httplib::Request& req, httplib::Response& res) {
            auto comp_id = req.path_params.at("id");
            auto prop_name = req.path_params.at("name");
            auto index_str = req.path_params.at("index");
            auto comp = m_app.get_component(comp_id);
            if (!comp) {
                property_handlers::error(res, "Component not found", 404);
                return;
            }

            size_t index = std::stoull(index_str);
            auto base_href = std::format("/{}/{}/{}/properties/{}/items/{}/fields", APP, COMPONENTS, comp_id, prop_name, index);
            property_handlers::get_struct_list_item_fields(comp.get(), prop_name, index, base_href, res);
        });

        // PATCH /app/components/:id/properties/:name/items/:index/fields/:field
        m_server->Patch(std::format("/{}/{}/:id/properties/:name/items/:index/fields/:field", APP, COMPONENTS),
            [this](const httplib::Request& req, httplib::Response& res) {
            auto comp_id = req.path_params.at("id");
            auto prop_name = req.path_params.at("name");
            auto index_str = req.path_params.at("index");
            auto field_name = req.path_params.at("field");
            auto comp = m_app.get_component(comp_id);
            if (!comp) {
                property_handlers::error(res, "Component not found", 404);
                return;
            }

            auto request_body = nlohmann::json::parse(req.body, nullptr, false);
            if (request_body.is_discarded()) {
                property_handlers::error(res, "Invalid JSON", 400);
                return;
            }

            size_t index = std::stoull(index_str);
            property_handlers::patch_struct_list_item_field(comp.get(), prop_name, index, field_name, request_body, res);
        });
    }

    int m_port;
    application m_app{"test_app"};
    std::unique_ptr<httplib::Server> m_server;
    std::thread m_server_thread;
};

} // anonymous namespace

TEST_CASE("HTTP Integration - Server Health Check") {
    test_server server;
    httplib::Client client(server.base_url());

    SECTION("Verify server is running") {
        // Try a simple root path that should 404 but prove server is responding
        auto result = client.Get("/");
        REQUIRE(result); // Connection successful
        INFO("Server is responding on " << server.base_url());
    }
}

TEST_CASE("HTTP Integration - Property GET") {
    test_server server;
    httplib::Client client(server.base_url());

    SECTION("Get single property via HTTP") {
        auto path = server.property_path("test_comp", "sample_rate");
        INFO("Requesting: " << path);

        auto result = client.Get(path);

        REQUIRE(result);
        if (result->status != httplib::OK_200) {
            INFO("Response status: " << result->status);
            INFO("Response body: " << result->body);
        }
        REQUIRE(result->status == httplib::OK_200);
        REQUIRE(result->get_header_value("Content-Type") == "application/json");

        auto json = nlohmann::json::parse(result->body);
        REQUIRE(json["name"].get<std::string>() == "sample_rate");
        REQUIRE(json["type"].get<std::string>() == "int32");
    }

    SECTION("Get non-existent property returns 404") {
        auto result = client.Get(server.property_path("test_comp", "does_not_exist"));

        REQUIRE(result);
        REQUIRE(result->status == httplib::NotFound_404);
    }
}

TEST_CASE("HTTP Integration - Property PUT") {
    test_server server;
    httplib::Client client(server.base_url());

    SECTION("Update property via HTTP PUT") {
        auto body = nlohmann::json{{"value", 96000}}.dump();
        auto result = client.Put(
            server.property_path("test_comp", "sample_rate"),
            body,
            "application/json"
        );

        REQUIRE(result);
        REQUIRE(result->status == httplib::OK_200);

        // Verify change persisted with GET
        auto get_result = client.Get(server.property_path("test_comp", "sample_rate"));
        REQUIRE(get_result);
        auto get_json = nlohmann::json::parse(get_result->body);
        REQUIRE(get_json.contains("value"));
    }

    SECTION("Cannot update non-runtime-configurable property") {
        auto body = nlohmann::json{{"value", 2048}}.dump();
        auto result = client.Put(
            server.property_path("test_comp", "buffer_size"),
            body,
            "application/json"
        );

        REQUIRE(result);
        REQUIRE(result->status == httplib::Forbidden_403);
    }
}

TEST_CASE("HTTP Integration - Property DELETE") {
    test_server server;
    httplib::Client client(server.base_url());

    SECTION("Reset property via HTTP DELETE") {
        // First change the value
        auto put_body = nlohmann::json{{"value", 96000}}.dump();
        client.Put(
            server.property_path("test_comp", "sample_rate"),
            put_body,
            "application/json"
        );

        // Then delete (reset)
        auto result = client.Delete(server.property_path("test_comp", "sample_rate"));

        REQUIRE(result);
        REQUIRE(result->status == httplib::OK_200);
    }
}

TEST_CASE("HTTP Integration - Error Handling") {
    test_server server;
    httplib::Client client(server.base_url());

    SECTION("Invalid JSON returns 400") {
        auto result = client.Put(
            server.property_path("test_comp", "sample_rate"),
            "{invalid json}",
            "application/json"
        );

        REQUIRE(result);
        REQUIRE(result->status == httplib::BadRequest_400);
    }

    SECTION("Non-existent component returns 404") {
        auto result = client.Get("/app/components/fake_comp/properties/sample_rate");

        REQUIRE(result);
        REQUIRE(result->status == httplib::NotFound_404);
    }
}

TEST_CASE("HTTP Integration - List Operations") {
    test_server server;
    httplib::Client client(server.base_url());

    SECTION("Get list items") {
        auto path = "/app/components/test_comp/properties/channels/items";
        auto result = client.Get(path);

        REQUIRE(result);
        REQUIRE(result->status == httplib::OK_200);

        auto json = nlohmann::json::parse(result->body);
        REQUIRE(json.contains("items"));
        REQUIRE(json.contains("count"));
        REQUIRE(json["count"].get<size_t>() == 2);
        REQUIRE(json["items"].is_array());
    }

    SECTION("Get single list item") {
        auto path = "/app/components/test_comp/properties/channels/items/0";
        auto result = client.Get(path);

        REQUIRE(result);
        REQUIRE(result->status == httplib::OK_200);

        auto json = nlohmann::json::parse(result->body);
        REQUIRE(json.contains("index"));
        REQUIRE(json.contains("value"));
        REQUIRE(json["index"].get<size_t>() == 0);
    }

    SECTION("Append to list via POST") {
        auto path = "/app/components/test_comp/properties/channels/items";
        auto body = nlohmann::json{{"value", "center"}}.dump();
        auto result = client.Post(path, body, "application/json");

        REQUIRE(result);
        REQUIRE(result->status == httplib::Created_201);
    }

    SECTION("Update list item via PUT") {
        auto path = "/app/components/test_comp/properties/channels/items/0";
        auto body = nlohmann::json{{"value", "front_left"}}.dump();
        auto result = client.Put(path, body, "application/json");

        REQUIRE(result);
        REQUIRE(result->status == httplib::OK_200);
    }

    SECTION("Delete list item") {
        auto path = "/app/components/test_comp/properties/channels/items/1";
        auto result = client.Delete(path);

        REQUIRE(result);
        REQUIRE(result->status == httplib::OK_200);
    }
}

TEST_CASE("HTTP Integration - Struct Operations") {
    test_server server;
    httplib::Client client(server.base_url());

    SECTION("Get struct fields") {
        auto path = "/app/components/test_comp/properties/network/fields";
        auto result = client.Get(path);

        REQUIRE(result);
        REQUIRE(result->status == httplib::OK_200);

        auto json = nlohmann::json::parse(result->body);
        REQUIRE(json.contains("fields"));
        REQUIRE(json["fields"].contains("host"));
        REQUIRE(json["fields"].contains("port"));
    }

    SECTION("Get single struct field") {
        auto path = "/app/components/test_comp/properties/network/fields/host";
        auto result = client.Get(path);

        REQUIRE(result);
        REQUIRE(result->status == httplib::OK_200);

        auto json = nlohmann::json::parse(result->body);
        REQUIRE(json.contains("value"));
    }

    SECTION("Update struct field via PATCH") {
        auto path = "/app/components/test_comp/properties/network/fields/host";
        auto body = nlohmann::json{{"value", "192.168.1.1"}}.dump();
        auto result = client.Patch(path, body, "application/json");

        REQUIRE(result);
        REQUIRE(result->status == httplib::OK_200);
    }
}

TEST_CASE("HTTP Integration - Struct List Operations") {
    test_server server;
    httplib::Client client(server.base_url());

    SECTION("Get struct list item fields") {
        auto path = "/app/components/test_comp/properties/connections/items/0/fields";
        auto result = client.Get(path);

        REQUIRE(result);
        REQUIRE(result->status == httplib::OK_200);

        auto json = nlohmann::json::parse(result->body);
        REQUIRE(json.contains("fields"));
        REQUIRE(json["fields"].contains("host"));
        REQUIRE(json["fields"].contains("port"));
    }

    SECTION("Update struct list item field") {
        auto path = "/app/components/test_comp/properties/connections/items/0/fields/host";
        auto body = nlohmann::json{{"value", "10.0.0.1"}}.dump();
        auto result = client.Patch(path, body, "application/json");

        REQUIRE(result);
        REQUIRE(result->status == httplib::OK_200);
    }
}

// Test components with ports for connection tests
class source_component : public component {
public:
    source_component(std::string_view id) : component(id) {
        add_port(output);
    }
    auto process() -> retval override { return retval::FINISH; }
    output_port<mutable_buffer<float>> output{"data_out"};
};

class sink_component : public component {
public:
    sink_component(std::string_view id) : component(id) {
        add_port(input);
    }
    auto process() -> retval override { return retval::FINISH; }
    input_port<mutable_buffer<float>> input{"data_in"};
};

// Test server with port components
class port_test_server {
public:
    port_test_server() : m_port(18081), m_app("port_test_app"), m_server(std::make_unique<httplib::Server>()) {
        // Create test application with components that have ports
        auto source = std::make_shared<source_component>("source");
        auto sink = std::make_shared<sink_component>("sink");
        m_app.add_component(source);
        m_app.add_component(sink);

        // Setup REST API endpoints
        setup_port_endpoints();

        // Start server in background thread
        m_server_thread = std::thread([this]() {
            m_server->listen("localhost", m_port);
        });

        // Wait for server to be ready
        std::this_thread::sleep_for(100ms);
    }

    ~port_test_server() {
        m_server->stop();
        if (m_server_thread.joinable()) {
            m_server_thread.join();
        }
    }

    auto port() const -> int { return m_port; }
    auto base_url() const -> std::string {
        return std::format("http://localhost:{}", m_port);
    }

    auto get_application() -> composite::application& { return m_app; }

private:
    void setup_port_endpoints() {
        // GET /app/components/:id/ports
        m_server->Get("/app/components/:id/ports", [this](const httplib::Request& req, httplib::Response& res) {
            auto comp_id = req.path_params.at("id");
            auto content = nlohmann::json();

            if (auto comp = m_app.get_component(comp_id); comp != nullptr) {
                auto ports_json = nlohmann::json::array();

                for (const auto& [port_name, port_ptr] : comp->ports()) {
                    if (port_ptr == nullptr) { continue; }

                    auto port_obj = nlohmann::json::object();
                    port_obj["name"] = port_name;

                    if (auto* output_port = dynamic_cast<output_port_base*>(port_ptr)) {
                        port_obj["type"] = "output";
                        port_obj["is_connected"] = output_port->is_connected();
                        port_obj["connection_count"] = output_port->connection_count();
                        port_obj["connected_ports"] = output_port->connected_ports();
                    } else if (dynamic_cast<input_port_base*>(port_ptr)) {
                        port_obj["type"] = "input";
                    }

                    ports_json.push_back(port_obj);
                }

                content["component_id"] = comp_id;
                content["ports"] = ports_json;
                res.set_content(content.dump(2), "application/json");
                res.status = httplib::OK_200;
            } else {
                content["error"] = std::format("component not found: {}", comp_id);
                res.set_content(content.dump(), "application/json");
                res.status = httplib::NotFound_404;
            }
        });

        // POST /app/connections
        m_server->Post("/app/connections", [this](const httplib::Request& req, httplib::Response& res) {
            auto content = nlohmann::json();

            try {
                auto conn_json = nlohmann::json::parse(req.body);

                if (!conn_json.contains("output") || !conn_json.contains("input")) {
                    content["error"] = "connection must specify both 'output' and 'input'";
                    res.set_content(content.dump(), "application/json");
                    res.status = httplib::BadRequest_400;
                    return;
                }

                auto& output = conn_json["output"];
                auto& input = conn_json["input"];

                if (!output.contains("component") || !output.contains("port")) {
                    content["error"] = "output must specify 'component' and 'port'";
                    res.set_content(content.dump(), "application/json");
                    res.status = httplib::BadRequest_400;
                    return;
                }

                if (!input.contains("component") || !input.contains("port")) {
                    content["error"] = "input must specify 'component' and 'port'";
                    res.set_content(content.dump(), "application/json");
                    res.status = httplib::BadRequest_400;
                    return;
                }

                auto source_comp_id = output["component"].get<std::string>();
                auto source_port_name = output["port"].get<std::string>();
                auto target_comp_id = input["component"].get<std::string>();
                auto target_port_name = input["port"].get<std::string>();

                auto source_comp = m_app.get_component(source_comp_id);
                if (!source_comp) {
                    content["error"] = std::format("source component not found: {}", source_comp_id);
                    res.set_content(content.dump(), "application/json");
                    res.status = httplib::NotFound_404;
                    return;
                }

                auto target_comp = m_app.get_component(target_comp_id);
                if (!target_comp) {
                    content["error"] = std::format("target component not found: {}", target_comp_id);
                    res.set_content(content.dump(), "application/json");
                    res.status = httplib::NotFound_404;
                    return;
                }

                bool success = source_comp->connect(source_port_name, target_comp, target_port_name);

                if (success) {
                    content["success"] = std::format("connected {}:{} to {}:{}",
                                                     source_comp_id, source_port_name,
                                                     target_comp_id, target_port_name);
                    content["connection"] = {
                        {"output", {{"component", source_comp_id}, {"port", source_port_name}}},
                        {"input", {{"component", target_comp_id}, {"port", target_port_name}}}
                    };
                    res.set_content(content.dump(), "application/json");
                    res.status = httplib::Created_201;
                } else {
                    content["error"] = std::format("failed to connect {}:{} to {}:{}",
                                                   source_comp_id, source_port_name,
                                                   target_comp_id, target_port_name);
                    res.set_content(content.dump(), "application/json");
                    res.status = httplib::BadRequest_400;
                }
            } catch (const std::exception& ex) {
                content["error"] = ex.what();
                res.set_content(content.dump(), "application/json");
                res.status = httplib::BadRequest_400;
            }
        });

        // DELETE /app/components/:id/ports/:port_name/connections
        m_server->Delete("/app/components/:id/ports/:port_name/connections",
                         [this](const httplib::Request& req, httplib::Response& res) {
            auto comp_id = req.path_params.at("id");
            auto port_name = req.path_params.at("port_name");
            auto content = nlohmann::json();

            if (auto comp = m_app.get_component(comp_id); comp != nullptr) {
                const auto& ports = comp->ports();
                auto port_it = ports.find(port_name);

                if (port_it != ports.end() && port_it->second != nullptr) {
                    if (auto* output_port = dynamic_cast<output_port_base*>(port_it->second)) {
                        auto disconnected_count = output_port->disconnect();
                        content["success"] = std::format("disconnected {} connections from port '{}'",
                                                         disconnected_count, port_name);
                        content["disconnected_count"] = disconnected_count;
                        res.set_content(content.dump(), "application/json");
                        res.status = httplib::OK_200;
                    } else {
                        content["error"] = std::format("port '{}' is not an output port", port_name);
                        res.set_content(content.dump(), "application/json");
                        res.status = httplib::BadRequest_400;
                    }
                } else {
                    content["error"] = std::format("port not found: {}", port_name);
                    res.set_content(content.dump(), "application/json");
                    res.status = httplib::NotFound_404;
                }
            } else {
                content["error"] = std::format("component not found: {}", comp_id);
                res.set_content(content.dump(), "application/json");
                res.status = httplib::NotFound_404;
            }
        });

        // DELETE /app/connections
        m_server->Delete("/app/connections",
                         [this](const httplib::Request& req, httplib::Response& res) {
            auto content = nlohmann::json();

            try {
                // Parse JSON body
                auto conn_json = nlohmann::json::parse(req.body);

                // Validate required fields
                if (!conn_json.contains("output") || !conn_json.contains("input")) {
                    content["error"] = "connection must specify both 'output' and 'input'";
                    res.set_content(content.dump(), "application/json");
                    res.status = httplib::BadRequest_400;
                    return;
                }

                auto& output = conn_json["output"];
                auto& input = conn_json["input"];

                // Validate output structure
                if (!output.contains("component") || !output.contains("port")) {
                    content["error"] = "output must specify 'component' and 'port'";
                    res.set_content(content.dump(), "application/json");
                    res.status = httplib::BadRequest_400;
                    return;
                }

                // Validate input structure
                if (!input.contains("component") || !input.contains("port")) {
                    content["error"] = "input must specify 'component' and 'port'";
                    res.set_content(content.dump(), "application/json");
                    res.status = httplib::BadRequest_400;
                    return;
                }

                auto source_comp_id = output["component"].get<std::string>();
                auto source_port_name = output["port"].get<std::string>();
                auto target_comp_id = input["component"].get<std::string>();
                auto target_port_name = input["port"].get<std::string>();

                auto source_comp = m_app.get_component(source_comp_id);
                if (!source_comp) {
                    content["error"] = std::format("source component not found: {}", source_comp_id);
                    res.set_content(content.dump(), "application/json");
                    res.status = httplib::NotFound_404;
                    return;
                }

                auto target_comp = m_app.get_component(target_comp_id);
                if (!target_comp) {
                    content["error"] = std::format("target component not found: {}", target_comp_id);
                    res.set_content(content.dump(), "application/json");
                    res.status = httplib::NotFound_404;
                    return;
                }

                const auto& source_ports = source_comp->ports();
                auto source_port_it = source_ports.find(source_port_name);
                if (source_port_it == source_ports.end() || source_port_it->second == nullptr) {
                    content["error"] = std::format("source port not found: {}", source_port_name);
                    res.set_content(content.dump(), "application/json");
                    res.status = httplib::NotFound_404;
                    return;
                }

                auto* output_port = dynamic_cast<output_port_base*>(source_port_it->second);
                if (!output_port) {
                    content["error"] = std::format("source port '{}' is not an output port", source_port_name);
                    res.set_content(content.dump(), "application/json");
                    res.status = httplib::BadRequest_400;
                    return;
                }

                const auto& target_ports = target_comp->ports();
                auto target_port_it = target_ports.find(target_port_name);
                if (target_port_it == target_ports.end() || target_port_it->second == nullptr) {
                    content["error"] = std::format("target port not found: {}", target_port_name);
                    res.set_content(content.dump(), "application/json");
                    res.status = httplib::NotFound_404;
                    return;
                }

                auto* input_port = dynamic_cast<input_port_base*>(target_port_it->second);
                if (!input_port) {
                    content["error"] = std::format("target port '{}' is not an input port", target_port_name);
                    res.set_content(content.dump(), "application/json");
                    res.status = httplib::BadRequest_400;
                    return;
                }

                bool was_connected = output_port->disconnect(input_port);
                if (was_connected) {
                    content["success"] = std::format("disconnected {}:{} from {}:{}",
                                                     source_comp_id, source_port_name,
                                                     target_comp_id, target_port_name);
                    content["connection"] = {
                        {"output", {{"component", source_comp_id}, {"port", source_port_name}}},
                        {"input", {{"component", target_comp_id}, {"port", target_port_name}}}
                    };
                    res.set_content(content.dump(), "application/json");
                    res.status = httplib::OK_200;
                } else {
                    content["error"] = std::format("ports were not connected: {}:{} -> {}:{}",
                                                   source_comp_id, source_port_name,
                                                   target_comp_id, target_port_name);
                    res.set_content(content.dump(), "application/json");
                    res.status = httplib::BadRequest_400;
                }
            } catch (const nlohmann::json::exception& e) {
                content["error"] = std::format("invalid JSON in request body: {}", e.what());
                res.set_content(content.dump(), "application/json");
                res.status = httplib::BadRequest_400;
            }
        });
    }

    int m_port;
    composite::application m_app;
    std::unique_ptr<httplib::Server> m_server;
    std::thread m_server_thread;
};

TEST_CASE("HTTP Integration - Port Information") {
    port_test_server server;
    httplib::Client client(server.base_url());

    SECTION("Get all ports for component") {
        auto path = "/app/components/source/ports";
        auto result = client.Get(path);

        REQUIRE(result);
        REQUIRE(result->status == httplib::OK_200);

        auto json = nlohmann::json::parse(result->body);
        REQUIRE(json.contains("component_id"));
        REQUIRE(json["component_id"].get<std::string>() == "source");
        REQUIRE(json.contains("ports"));
        REQUIRE(json["ports"].is_array());
        REQUIRE(json["ports"].size() == 1);
        REQUIRE(json["ports"][0]["name"].get<std::string>() == "data_out");
        REQUIRE(json["ports"][0]["type"].get<std::string>() == "output");
        REQUIRE(json["ports"][0]["is_connected"].get<bool>() == false);
        REQUIRE(json["ports"][0]["connection_count"].get<int>() == 0);
    }

    SECTION("Get ports for non-existent component") {
        auto path = "/app/components/invalid/ports";
        auto result = client.Get(path);

        REQUIRE(result);
        REQUIRE(result->status == httplib::NotFound_404);

        auto json = nlohmann::json::parse(result->body);
        REQUIRE(json.contains("error"));
    }
}

TEST_CASE("HTTP Integration - Port Connections") {
    port_test_server server;
    httplib::Client client(server.base_url());

    SECTION("Create connection between components") {
        auto body = nlohmann::json{
            {"output", {{"component", "source"}, {"port", "data_out"}}},
            {"input", {{"component", "sink"}, {"port", "data_in"}}}
        }.dump();

        auto result = client.Post("/app/connections", body, "application/json");

        REQUIRE(result);
        REQUIRE(result->status == httplib::Created_201);

        auto json = nlohmann::json::parse(result->body);
        REQUIRE(json.contains("success"));
        REQUIRE(json.contains("connection"));
        REQUIRE(json["connection"]["output"]["component"].get<std::string>() == "source");
        REQUIRE(json["connection"]["output"]["port"].get<std::string>() == "data_out");
        REQUIRE(json["connection"]["input"]["component"].get<std::string>() == "sink");
        REQUIRE(json["connection"]["input"]["port"].get<std::string>() == "data_in");

        // Verify connection was made
        auto ports_result = client.Get("/app/components/source/ports");
        REQUIRE(ports_result);
        auto ports_json = nlohmann::json::parse(ports_result->body);
        REQUIRE(ports_json["ports"][0]["is_connected"].get<bool>() == true);
        REQUIRE(ports_json["ports"][0]["connection_count"].get<int>() == 1);
    }

    SECTION("Create connection with invalid component") {
        auto body = nlohmann::json{
            {"output", {{"component", "invalid"}, {"port", "data_out"}}},
            {"input", {{"component", "sink"}, {"port", "data_in"}}}
        }.dump();

        auto result = client.Post("/app/connections", body, "application/json");

        REQUIRE(result);
        REQUIRE(result->status == httplib::NotFound_404);

        auto json = nlohmann::json::parse(result->body);
        REQUIRE(json.contains("error"));
    }

    SECTION("Create connection with missing fields") {
        auto body = nlohmann::json{
            {"output", {{"component", "source"}}}
        }.dump();

        auto result = client.Post("/app/connections", body, "application/json");

        REQUIRE(result);
        REQUIRE(result->status == httplib::BadRequest_400);

        auto json = nlohmann::json::parse(result->body);
        REQUIRE(json.contains("error"));
    }
}

TEST_CASE("HTTP Integration - Port Disconnections") {
    port_test_server server;
    httplib::Client client(server.base_url());

    // First create a connection
    auto conn_body = nlohmann::json{
        {"output", {{"component", "source"}, {"port", "data_out"}}},
        {"input", {{"component", "sink"}, {"port", "data_in"}}}
    }.dump();
    auto conn_result = client.Post("/app/connections", conn_body, "application/json");
    REQUIRE(conn_result);
    REQUIRE(conn_result->status == httplib::Created_201);

    SECTION("Disconnect specific connection") {
        auto body = nlohmann::json{
            {"output", {{"component", "source"}, {"port", "data_out"}}},
            {"input", {{"component", "sink"}, {"port", "data_in"}}}
        }.dump();

        auto result = client.Delete("/app/connections", body, "application/json");

        REQUIRE(result);
        REQUIRE(result->status == httplib::OK_200);

        auto json = nlohmann::json::parse(result->body);
        REQUIRE(json.contains("success"));
        REQUIRE(json.contains("connection"));

        // Verify disconnection
        auto ports_result = client.Get("/app/components/source/ports");
        REQUIRE(ports_result);
        auto ports_json = nlohmann::json::parse(ports_result->body);
        REQUIRE(ports_json["ports"][0]["is_connected"].get<bool>() == false);
        REQUIRE(ports_json["ports"][0]["connection_count"].get<int>() == 0);
    }

    SECTION("Disconnect all connections from port") {
        auto path = "/app/components/source/ports/data_out/connections";
        auto result = client.Delete(path);

        REQUIRE(result);
        REQUIRE(result->status == httplib::OK_200);

        auto json = nlohmann::json::parse(result->body);
        REQUIRE(json.contains("success"));
        REQUIRE(json.contains("disconnected_count"));
        REQUIRE(json["disconnected_count"].get<int>() == 1);

        // Verify disconnection
        auto ports_result = client.Get("/app/components/source/ports");
        REQUIRE(ports_result);
        auto ports_json = nlohmann::json::parse(ports_result->body);
        REQUIRE(ports_json["ports"][0]["is_connected"].get<bool>() == false);
    }

    SECTION("Disconnect non-existent connection") {
        // First disconnect the actual connection
        client.Delete("/app/components/source/ports/data_out/connections");

        // Try to disconnect again using body-based DELETE
        auto body = nlohmann::json{
            {"output", {{"component", "source"}, {"port", "data_out"}}},
            {"input", {{"component", "sink"}, {"port", "data_in"}}}
        }.dump();

        auto result = client.Delete("/app/connections", body, "application/json");

        REQUIRE(result);
        REQUIRE(result->status == httplib::BadRequest_400);

        auto json = nlohmann::json::parse(result->body);
        REQUIRE(json.contains("error"));
    }

    SECTION("Disconnect from invalid port") {
        auto path = "/app/components/source/ports/invalid/connections";
        auto result = client.Delete(path);

        REQUIRE(result);
        REQUIRE(result->status == httplib::NotFound_404);

        auto json = nlohmann::json::parse(result->body);
        REQUIRE(json.contains("error"));
    }
}
