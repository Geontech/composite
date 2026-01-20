/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "composite/core/application.hpp"
#include "composite/metrics/metrics.hpp"
#include "composite/properties/serialization.hpp"

#include "helpers.hpp"
#include "property_changeset.hpp"
#include "property_handlers.hpp"

#include <chrono>
#include <cstdlib>
#include <format>
#include <httplib.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <thread>
#include <variant>

namespace composite {

auto to_json(nlohmann::json& json_obj, const port_base& port) {
    json_obj["name"] = port.name();
}

auto to_json(nlohmann::json& json_obj, const std::map<std::string, port_base*>& ports) {
    json_obj = nlohmann::json::array();
    for (const auto& [name, port] : ports) {
        if (port != nullptr) {
            json_obj.push_back(*port);
        }
    }
}

auto to_json(nlohmann::json& json_obj, const component::connection& connection) {
    json_obj["input"] = {{"component", connection.input.first}, {"port", connection.input.second}};
    json_obj["output"] = {{"component", connection.output.first}, {"port", connection.output.second}};
}

auto to_json(nlohmann::json& json_obj, const std::vector<component::connection>& connections) {
    json_obj = nlohmann::json::array();
    for (auto& conn : connections) {
        json_obj.push_back(conn);
    }
}

auto to_json(nlohmann::json& json_obj, const component& comp) {
    json_obj["id"] = comp.id();
    json_obj["ports"] = comp.ports();
    json_obj["connections"] = comp.connections();
    json_obj["properties"] = properties::property_serializer::to_json(comp.property_set());
}

auto to_json(nlohmann::json& json_obj, const std::vector<std::shared_ptr<component>>& comps) {
    json_obj = nlohmann::json::array();
    for (const auto& comp : comps) {
        if (comp != nullptr) {
            json_obj.push_back(*comp.get());
        }
    }
}

auto to_json(nlohmann::json& json_obj, const application& app) {
    json_obj["name"] = app.name();
    const auto& components = app.components();
    json_obj["components"] = components;
    json_obj["connections"] = nlohmann::json::array();
    for (const auto& comp : components) {
        if (comp != nullptr) {
            for (const auto& conn : comp->connections()) {
                json_obj["connections"].push_back(conn);
            }
        }
    }
}

// Use response helpers from property_handlers for consistency
using property_handlers::set_cors;
using property_handlers::json_ok;
using property_handlers::json_created;
using property_handlers::error;

// Helper to serialize a port to JSON
auto port_to_json(const std::string& name, port_base* port) -> nlohmann::json {
    auto obj = nlohmann::json{{"name", name}};
    if (auto* out = dynamic_cast<output_port_base*>(port)) {
        obj["type"] = "output";
        obj["is_connected"] = out->is_connected();
        obj["connection_count"] = out->connection_count();
        obj["connected_ports"] = out->connected_ports();
    } else if (dynamic_cast<input_port_base*>(port)) {
        obj["type"] = "input";
    }
    return obj;
}

// Helper struct for connection request parsing
struct connection_request {
    std::string source_comp_id;
    std::string source_port_name;
    std::string target_comp_id;
    std::string target_port_name;

    // Parse and validate a connection request body, returns error message if invalid
    static auto parse(const std::string& body) -> std::variant<connection_request, std::string> {
        nlohmann::json json;
        try {
            json = nlohmann::json::parse(body);
        } catch (...) {
            return "invalid JSON in request body";
        }

        if (!json.contains("output") || !json.contains("input")) {
            return "connection must specify both 'output' and 'input'";
        }

        auto& output = json["output"];
        auto& input = json["input"];

        if (!output.contains("component") || !output.contains("port")) {
            return "output must specify 'component' and 'port'";
        }
        if (!input.contains("component") || !input.contains("port")) {
            return "input must specify 'component' and 'port'";
        }

        return connection_request{
            output["component"].get<std::string>(),
            output["port"].get<std::string>(),
            input["component"].get<std::string>(),
            input["port"].get<std::string>()
        };
    }

    [[nodiscard]] auto to_json() const -> nlohmann::json {
        return {
            {"output", {{"component", source_comp_id}, {"port", source_port_name}}},
            {"input", {{"component", target_comp_id}, {"port", target_port_name}}}
        };
    }
};

// ============================================================================
// Metrics JSON Serialization
// ============================================================================

auto metric_snapshot_to_json(const metrics::metric_snapshot& snap) -> nlohmann::json {
    auto json_obj = nlohmann::json::object();
    json_obj["name"] = snap.name;
    json_obj["description"] = snap.description;
    json_obj["unit"] = snap.unit;
    json_obj["type"] = std::string{metrics::to_string(snap.type)};

    // Labels
    auto labels_obj = nlohmann::json::object();
    for (const auto& [k, v] : snap.labels) {
        labels_obj[k] = v;
    }
    json_obj["labels"] = labels_obj;

    // Value based on type
    std::visit([&json_obj](auto&& val) {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, uint64_t>) {
            json_obj["value"] = val;
        } else if constexpr (std::is_same_v<T, int64_t>) {
            json_obj["value"] = val;
        } else if constexpr (std::is_same_v<T, double>) {
            json_obj["value"] = val;
        } else if constexpr (std::is_same_v<T, metrics::histogram_snapshot>) {
            auto hist_obj = nlohmann::json::object();
            hist_obj["count"] = val.count;
            hist_obj["sum"] = val.sum;
            hist_obj["boundaries"] = val.boundaries;
            hist_obj["bucket_counts"] = val.bucket_counts;
            json_obj["value"] = hist_obj;
        }
    }, snap.value);

    // Timestamp as ISO 8601 (thread-safe)
    auto time_t = std::chrono::system_clock::to_time_t(snap.timestamp);
    std::tm tm{};
    gmtime_r(&time_t, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    json_obj["timestamp"] = buf;

    return json_obj;
}

auto metrics_to_json(const std::vector<metrics::metric_snapshot>& snapshots) -> nlohmann::json {
    auto result = nlohmann::json::object();

    auto time_t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    gmtime_r(&time_t, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    result["timestamp"] = buf;

    auto metrics_arr = nlohmann::json::array();
    for (const auto& snap : snapshots) {
        metrics_arr.push_back(metric_snapshot_to_json(snap));
    }
    result["metrics"] = metrics_arr;
    result["count"] = snapshots.size();

    return result;
}

auto set_component_properties(
  composite::application::component_ptr comp,
  const nlohmann::json& properties
) -> httplib::Response {
    auto res = httplib::Response{};
    try {
        spdlog::trace("patching component-level properties on {}", comp->id());

        // Parse property changeset
        auto changeset = property_changeset::from_json(properties);

        // Apply changes if there are any
        if (changeset.has_updates()) {
            comp->set_properties(changeset.properties(), composite::properties::config_type::RUNTIME);
            comp->property_change_handler();

            // Apply any pending lifecycle changes (start/stop based on "enabled" property)
            // This must be done AFTER set_properties() completes to avoid deadlock
            comp->apply_lifecycle_changes();
        }

        json_ok(res, {{"success", std::format("successfully set properties on component {}", comp->id())}});
    } catch (const std::exception& ex) {
        error(res, ex.what(), 400);
    }
    return res;
}

#ifdef COMPOSITE_USE_OPENSSL
auto make_server(
  application& app,
  composite::component_handles_type& handles,
  const std::string& cert,
  const std::string& key,
  const std::string& ca
) -> std::unique_ptr<httplib::Server> {
    auto server = std::make_unique<httplib::SSLServer>(cert.c_str(), key.c_str(), ca.c_str());
#else
auto make_server(application& app, composite::component_handles_type& handles) -> std::unique_ptr<httplib::Server> {
    auto server = std::make_unique<httplib::Server>();
#endif

    // Add a common handler for preflight requests (OPTIONS method)
    server->Options(".*", [](const httplib::Request &, httplib::Response &res) {
        res.set_header("Access-Control-Allow-Origin", "*"); // allow all origins
        res.set_header("Access-Control-Allow-Methods", "GET, POST, PATCH, DELETE, OPTIONS"); // allowed HTTP methods
        res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization"); // allowed headers
        res.set_header("Access-Control-Max-Age", "86400"); // cache preflight response for 1 day
        res.status = 204; // no content
    });

    const auto APP = std::string{"app"};
    const auto COMPONENTS = std::string{"components"};
    const auto CONNECTIONS = std::string{"connections"};

    // GET health
    auto endpoint = std::format("/{}/healthz", APP);
    server->Get(endpoint, [](const httplib::Request&, httplib::Response& res) {
        json_ok(res, {});
    });

    // ========================================================================
    // Metrics Endpoints
    // ========================================================================

    const auto METRICS = std::string{"metrics"};

    // GET /app/metrics - Snapshot of all metrics
    endpoint = std::format("/{}/{}", APP, METRICS);
    server->Get(endpoint, [](const httplib::Request& req, httplib::Response& res) {
        set_cors(res);

        auto& registry = metrics::registry::instance();
        std::vector<metrics::metric_snapshot> snapshots;

        // Check for prefix filter
        if (req.has_param("prefix")) {
            snapshots = registry.snapshot_by_prefix(req.get_param_value("prefix"));
        }
        // Check for label filter
        else if (req.has_param("label_key") && req.has_param("label_value")) {
            snapshots = registry.snapshot_by_label(
                req.get_param_value("label_key"),
                req.get_param_value("label_value"));
        }
        // All metrics
        else {
            snapshots = registry.snapshot_all();
        }

        auto result = metrics_to_json(snapshots);
        res.set_content(result.dump(2), "application/json");
        res.status = httplib::OK_200;
    });

    // GET /app/metrics/stream - SSE endpoint for metrics streaming
    endpoint = std::format("/{}/{}/stream", APP, METRICS);
    server->Get(endpoint, [](const httplib::Request& req, httplib::Response& res) {
        // Parse interval from query params (default 1000ms)
        int interval_ms = 1000;
        if (req.has_param("interval")) {
            try {
                interval_ms = std::stoi(req.get_param_value("interval"));
                if (interval_ms < 100) interval_ms = 100;      // Min 100ms
                if (interval_ms > 60000) interval_ms = 60000;  // Max 60s
            } catch (...) {
                // Use default
            }
        }

        // Optional prefix filter
        std::string prefix_filter;
        if (req.has_param("prefix")) {
            prefix_filter = req.get_param_value("prefix");
        }

        // Optional label filter
        std::string label_key, label_value;
        if (req.has_param("label_key") && req.has_param("label_value")) {
            label_key = req.get_param_value("label_key");
            label_value = req.get_param_value("label_value");
        }

        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection", "keep-alive");

        res.set_chunked_content_provider(
            "text/event-stream",
            [interval_ms, prefix_filter, label_key, label_value](
                std::size_t /*offset*/,
                httplib::DataSink& sink
            ) -> bool {
                // Check if client disconnected before doing any work
                if (sink.is_writable != nullptr && !sink.is_writable()) {
                    return false;  // Client disconnected
                }

                auto& registry = metrics::registry::instance();

                // Get filtered snapshots
                std::vector<metrics::metric_snapshot> snapshots;
                if (!prefix_filter.empty()) {
                    snapshots = registry.snapshot_by_prefix(prefix_filter);
                } else if (!label_key.empty()) {
                    snapshots = registry.snapshot_by_label(label_key, label_value);
                } else {
                    snapshots = registry.snapshot_all();
                }

                // Format as SSE event
                auto data = metrics_to_json(snapshots).dump();
                auto event = std::format("event: metrics\ndata: {}\n\n", data);

                if (!sink.write(event.c_str(), event.size())) {
                    return false;  // Connection closed
                }

                // Sleep in smaller chunks to detect disconnection faster
                // Poll every 100ms to check connection status
                constexpr int poll_interval_ms = 100;
                int remaining_ms = interval_ms;
                while (remaining_ms > 0) {
                    int sleep_ms = std::min(remaining_ms, poll_interval_ms);
                    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
                    remaining_ms -= sleep_ms;

                    // Check if client disconnected during sleep
                    if (sink.is_writable != nullptr && !sink.is_writable()) {
                        return false;  // Client disconnected
                    }
                }

                return true;  // Continue streaming
            }
        );
    });

    // GET application
    endpoint = std::format("/{}", APP);
    server->Get(endpoint, [&app](const httplib::Request&, httplib::Response& res) {
        json_ok(res, nlohmann::json(app));
    });

    // GET components
    endpoint = std::format("/{}/{}", APP, COMPONENTS);
    server->Get(endpoint, [&app](const httplib::Request&, httplib::Response& res) {
        json_ok(res, nlohmann::json(app.components()));
    });

    // POST components
    endpoint = std::format("/{}/{}", APP, COMPONENTS);
    server->Post(endpoint, [&app, &handles](const httplib::Request& req, httplib::Response& res) {
        try {
            auto comp_json = nlohmann::json::parse(req.body);

            if (!comp_json.contains("library")) {
                return error(res, "no component library provided", 400);
            }
            if (!comp_json.contains("id")) {
                return error(res, "no component id provided", 400);
            }

            auto comp_id = comp_json["id"].get<std::string>();
            if (app.get_component(comp_id) != nullptr) {
                return error(res, std::format("component id already exists: {}", comp_id), 409);
            }

            auto comp_ptr = composite::make_component(comp_json, handles);
            if (comp_ptr == nullptr) {
                auto msg = std::format("failed to create component {} from library {}",
                    comp_id,
                    comp_json["library"].get<std::string>());
                spdlog::error(msg);
                return error(res, msg, 500);
            }

            if (comp_json.contains("properties")) {
                spdlog::trace("setting component-level properties on {}", comp_ptr->id());
                auto props = std::vector<std::pair<std::string, std::string>>{};
                for (const auto& [name, value] : comp_json["properties"].get<std::map<std::string, std::string>>()) {
                    props.emplace_back(name, value);
                }
                comp_ptr->set_properties(props);
            }

            app.add_component(comp_ptr);
            spdlog::trace("added {} to application '{}'", comp_ptr->id(), app.name());
            json_created(res, {{"success", std::format("added {} to application '{}'", comp_ptr->id(), app.name())}});
        } catch (const std::exception& ex) {
            error(res, ex.what(), 400);
        }
    });

    // GET component by ID
    endpoint = std::format("/{}/{}/:id", APP, COMPONENTS);
    server->Get(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            json_ok(res, nlohmann::json(*comp));
        } else {
            error(res, std::format("component not found: {}", comp_id), 404);
        }
    });

    // PATCH components (batch)
    endpoint = std::format("/{}/{}", APP, COMPONENTS);
    server->Patch(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        nlohmann::json json_body;
        try {
            json_body = nlohmann::json::parse(req.body);
        } catch (...) {
            return error(res, "invalid json request", 400);
        }

        if (!json_body.contains("components")) {
            return error(res, "components not provided", 400);
        }

        for (const auto& comp_json : json_body["components"]) {
            if (!comp_json.contains("id")) {
                return error(res, "component id not provided", 400);
            }
            auto comp_id = comp_json["id"].get<std::string>();

            if (!comp_json.contains("properties")) {
                return error(res, std::format("component properties not provided for {}", comp_id), 400);
            }

            if (auto comp = app.get_component(comp_id); comp != nullptr) {
                res = set_component_properties(comp, comp_json["properties"]);
            } else {
                return error(res, std::format("component not found: {}", comp_id), 404);
            }
        }
    });

    // PATCH component by ID
    endpoint = std::format("/{}/{}/:id", APP, COMPONENTS);
    server->Patch(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto comp = app.get_component(comp_id);
        if (!comp) {
            return error(res, std::format("component not found: {}", comp_id), 404);
        }

        nlohmann::json json_body;
        try {
            json_body = nlohmann::json::parse(req.body);
        } catch (...) {
            return error(res, "invalid json request", 400);
        }

        if (!json_body.contains("properties")) {
            return error(res, "properties not provided", 400);
        }

        res = set_component_properties(comp, json_body["properties"]);
    });

    // GET /app/components/:id/properties
    endpoint = std::format("/{}/{}/:id/properties", APP, COMPONENTS);
    server->Get(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            property_handlers::get_properties(comp.get(), res);
        } else {
            property_handlers::error(res, std::format("Component '{}' not found", comp_id), 404);
        }
    });

    // GET /app/components/:id/properties/:name
    endpoint = std::format("/{}/{}/:id/properties/:name", APP, COMPONENTS);
    server->Get(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto prop_name = req.path_params.at("name");
        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            property_handlers::get_property(comp.get(), prop_name, res);
        } else {
            property_handlers::error(res, std::format("Component '{}' not found", comp_id), 404);
        }
    });

    // PUT /app/components/:id/properties/:name
    endpoint = std::format("/{}/{}/:id/properties/:name", APP, COMPONENTS);
    server->Put(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto prop_name = req.path_params.at("name");
        nlohmann::json body;
        try { body = nlohmann::json::parse(req.body); }
        catch (...) { return property_handlers::error(res, "Invalid JSON", 400); }
        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            property_handlers::put_property(comp.get(), prop_name, body, res);
        } else {
            property_handlers::error(res, std::format("Component '{}' not found", comp_id), 404);
        }
    });

    // PATCH /app/components/:id/properties/:name
    endpoint = std::format("/{}/{}/:id/properties/:name", APP, COMPONENTS);
    server->Patch(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto prop_name = req.path_params.at("name");
        nlohmann::json body;
        try { body = nlohmann::json::parse(req.body); }
        catch (...) { return property_handlers::error(res, "Invalid JSON", 400); }
        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            property_handlers::patch_property(comp.get(), prop_name, body, res);
        } else {
            property_handlers::error(res, std::format("Component '{}' not found", comp_id), 404);
        }
    });

    // DELETE /app/components/:id/properties/:name
    endpoint = std::format("/{}/{}/:id/properties/:name", APP, COMPONENTS);
    server->Delete(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto prop_name = req.path_params.at("name");
        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            property_handlers::delete_property(comp.get(), prop_name, res);
        } else {
            property_handlers::error(res, std::format("Component '{}' not found", comp_id), 404);
        }
    });

    // ========================================================================
    // List Property Operations
    // ========================================================================

    // GET /app/components/:id/properties/:name/items
    endpoint = std::format("/{}/{}/:id/properties/:name/items", APP, COMPONENTS);
    server->Get(endpoint, [&app, &APP, &COMPONENTS](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto prop_name = req.path_params.at("name");
        auto base_href = std::format("/{}/{}/{}/properties/{}/items", APP, COMPONENTS, comp_id, prop_name);
        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            property_handlers::get_list_items(comp.get(), prop_name, base_href, res);
        } else {
            property_handlers::error(res, std::format("Component '{}' not found", comp_id), 404);
        }
    });

    // GET /app/components/:id/properties/:name/items/:index
    endpoint = std::format("/{}/{}/:id/properties/:name/items/:index", APP, COMPONENTS);
    server->Get(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto prop_name = req.path_params.at("name");
        auto index_str = req.path_params.at("index");
        std::size_t index;
        try { index = std::stoull(index_str); }
        catch (...) { return property_handlers::error(res, std::format("Invalid index: {}", index_str), 400); }
        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            property_handlers::get_list_item(comp.get(), prop_name, index, res);
        } else {
            property_handlers::error(res, std::format("Component '{}' not found", comp_id), 404);
        }
    });

    // POST /app/components/:id/properties/:name/items
    endpoint = std::format("/{}/{}/:id/properties/:name/items", APP, COMPONENTS);
    server->Post(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto prop_name = req.path_params.at("name");
        nlohmann::json body;
        try { body = nlohmann::json::parse(req.body); }
        catch (...) { return property_handlers::error(res, "Invalid JSON", 400); }
        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            property_handlers::post_list_item(comp.get(), prop_name, body, res);
        } else {
            property_handlers::error(res, std::format("Component '{}' not found", comp_id), 404);
        }
    });

    // PUT /app/components/:id/properties/:name/items/:index
    endpoint = std::format("/{}/{}/:id/properties/:name/items/:index", APP, COMPONENTS);
    server->Put(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto prop_name = req.path_params.at("name");
        auto index_str = req.path_params.at("index");
        std::size_t index;
        try { index = std::stoull(index_str); }
        catch (...) { return property_handlers::error(res, std::format("Invalid index: {}", index_str), 400); }
        nlohmann::json body;
        try { body = nlohmann::json::parse(req.body); }
        catch (...) { return property_handlers::error(res, "Invalid JSON", 400); }
        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            property_handlers::put_list_item(comp.get(), prop_name, index, body, res);
        } else {
            property_handlers::error(res, std::format("Component '{}' not found", comp_id), 404);
        }
    });

    // DELETE /app/components/:id/properties/:name/items/:index
    endpoint = std::format("/{}/{}/:id/properties/:name/items/:index", APP, COMPONENTS);
    server->Delete(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto prop_name = req.path_params.at("name");
        auto index_str = req.path_params.at("index");
        std::size_t index;
        try { index = std::stoull(index_str); }
        catch (...) { return property_handlers::error(res, std::format("Invalid index: {}", index_str), 400); }
        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            property_handlers::delete_list_item(comp.get(), prop_name, index, res);
        } else {
            property_handlers::error(res, std::format("Component '{}' not found", comp_id), 404);
        }
    });

    // ========================================================================
    // Struct Property Operations
    // ========================================================================

    // GET /app/components/:id/properties/:name/fields
    endpoint = std::format("/{}/{}/:id/properties/:name/fields", APP, COMPONENTS);
    server->Get(endpoint, [&app, &APP, &COMPONENTS](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto prop_name = req.path_params.at("name");
        auto base_href = std::format("/{}/{}/{}/properties/{}/fields", APP, COMPONENTS, comp_id, prop_name);
        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            property_handlers::get_struct_fields(comp.get(), prop_name, base_href, res);
        } else {
            property_handlers::error(res, std::format("Component '{}' not found", comp_id), 404);
        }
    });

    // GET /app/components/:id/properties/:name/fields/:field
    endpoint = std::format("/{}/{}/:id/properties/:name/fields/:field", APP, COMPONENTS);
    server->Get(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto prop_name = req.path_params.at("name");
        auto field_name = req.path_params.at("field");
        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            property_handlers::get_struct_field(comp.get(), prop_name, field_name, res);
        } else {
            property_handlers::error(res, std::format("Component '{}' not found", comp_id), 404);
        }
    });

    // PATCH /app/components/:id/properties/:name/fields/:field
    endpoint = std::format("/{}/{}/:id/properties/:name/fields/:field", APP, COMPONENTS);
    server->Patch(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto prop_name = req.path_params.at("name");
        auto field_name = req.path_params.at("field");
        nlohmann::json body;
        try { body = nlohmann::json::parse(req.body); }
        catch (...) { return property_handlers::error(res, "Invalid JSON", 400); }
        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            property_handlers::patch_struct_field(comp.get(), prop_name, field_name, body, res);
        } else {
            property_handlers::error(res, std::format("Component '{}' not found", comp_id), 404);
        }
    });

    // GET /app/components/:id/properties/:name/items/:index/fields
    endpoint = std::format("/{}/{}/:id/properties/:name/items/:index/fields", APP, COMPONENTS);
    server->Get(endpoint, [&app, &APP, &COMPONENTS](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto prop_name = req.path_params.at("name");
        auto index_str = req.path_params.at("index");
        std::size_t index;
        try { index = std::stoull(index_str); }
        catch (...) { return property_handlers::error(res, std::format("Invalid index: {}", index_str), 400); }
        auto base_href = std::format("/{}/{}/{}/properties/{}/items/{}/fields", APP, COMPONENTS, comp_id, prop_name, index);
        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            property_handlers::get_struct_list_item_fields(comp.get(), prop_name, index, base_href, res);
        } else {
            property_handlers::error(res, std::format("Component '{}' not found", comp_id), 404);
        }
    });

    // PATCH /app/components/:id/properties/:name/items/:index/fields/:field
    endpoint = std::format("/{}/{}/:id/properties/:name/items/:index/fields/:field", APP, COMPONENTS);
    server->Patch(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto prop_name = req.path_params.at("name");
        auto index_str = req.path_params.at("index");
        auto field_name = req.path_params.at("field");
        std::size_t index;
        try { index = std::stoull(index_str); }
        catch (...) { return property_handlers::error(res, std::format("Invalid index: {}", index_str), 400); }
        nlohmann::json body;
        try { body = nlohmann::json::parse(req.body); }
        catch (...) { return property_handlers::error(res, "Invalid JSON", 400); }
        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            property_handlers::patch_struct_list_item_field(comp.get(), prop_name, index, field_name, body, res);
        } else {
            property_handlers::error(res, std::format("Component '{}' not found", comp_id), 404);
        }
    });

    // ========================================================================
    // Port Connection Operations
    // ========================================================================

    // GET /app/components/:id/ports
    endpoint = std::format("/{}/{}/:id/ports", APP, COMPONENTS);
    server->Get(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto comp = app.get_component(comp_id);
        if (!comp) {
            return error(res, std::format("component not found: {}", comp_id), 404);
        }

        auto ports_json = nlohmann::json::array();
        for (const auto& [name, port] : comp->ports()) {
            if (port) { ports_json.push_back(port_to_json(name, port)); }
        }

        json_ok(res, {{"component_id", comp_id}, {"ports", ports_json}});
    });

    // GET /app/components/:id/ports/:port_name
    endpoint = std::format("/{}/{}/:id/ports/:port_name", APP, COMPONENTS);
    server->Get(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto port_name = req.path_params.at("port_name");

        auto comp = app.get_component(comp_id);
        if (!comp) {
            return error(res, std::format("component not found: {}", comp_id), 404);
        }

        const auto& ports = comp->ports();
        auto it = ports.find(port_name);
        if (it == ports.end() || !it->second) {
            return error(res, std::format("port not found: {}", port_name), 404);
        }

        auto result = port_to_json(port_name, it->second);
        result["component_id"] = comp_id;
        json_ok(res, result);
    });

    // DELETE /app/components/:id/ports/:port_name/connections
    endpoint = std::format("/{}/{}/:id/ports/:port_name/connections", APP, COMPONENTS);
    server->Delete(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto port_name = req.path_params.at("port_name");

        auto comp = app.get_component(comp_id);
        if (!comp) {
            return error(res, std::format("component not found: {}", comp_id), 404);
        }

        const auto& ports = comp->ports();
        auto it = ports.find(port_name);
        if (it == ports.end() || !it->second) {
            return error(res, std::format("port not found: {}", port_name), 404);
        }

        auto* output_port = dynamic_cast<output_port_base*>(it->second);
        if (!output_port) {
            return error(res, std::format("port '{}' is not an output port", port_name), 400);
        }

        auto count = output_port->disconnect();
        json_ok(res, {
            {"success", std::format("disconnected {} connections from port '{}'", count, port_name)},
            {"disconnected_count", count}
        });
    });

    // DELETE /app/connections
    endpoint = std::format("/{}/{}", APP, CONNECTIONS);
    server->Delete(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        auto result = connection_request::parse(req.body);
        if (auto* err = std::get_if<std::string>(&result)) {
            return error(res, *err, 400);
        }
        auto conn = std::get<connection_request>(result);

        auto source_comp = app.get_component(conn.source_comp_id);
        if (!source_comp) {
            return error(res, std::format("source component not found: {}", conn.source_comp_id), 404);
        }

        auto target_comp = app.get_component(conn.target_comp_id);
        if (!target_comp) {
            return error(res, std::format("target component not found: {}", conn.target_comp_id), 404);
        }

        const auto& source_ports = source_comp->ports();
        auto source_it = source_ports.find(conn.source_port_name);
        if (source_it == source_ports.end() || !source_it->second) {
            return error(res, std::format("source port not found: {}", conn.source_port_name), 404);
        }

        auto* output_port = dynamic_cast<output_port_base*>(source_it->second);
        if (!output_port) {
            return error(res, std::format("source port '{}' is not an output port", conn.source_port_name), 400);
        }

        const auto& target_ports = target_comp->ports();
        auto target_it = target_ports.find(conn.target_port_name);
        if (target_it == target_ports.end() || !target_it->second) {
            return error(res, std::format("target port not found: {}", conn.target_port_name), 404);
        }

        auto* input_port = dynamic_cast<input_port_base*>(target_it->second);
        if (!input_port) {
            return error(res, std::format("target port '{}' is not an input port", conn.target_port_name), 400);
        }

        if (output_port->disconnect(input_port)) {
            json_ok(res, {
                {"success", std::format("disconnected {}:{} from {}:{}",
                    conn.source_comp_id, conn.source_port_name,
                    conn.target_comp_id, conn.target_port_name)},
                {"connection", conn.to_json()}
            });
        } else {
            error(res, std::format("ports were not connected: {}:{} -> {}:{}",
                conn.source_comp_id, conn.source_port_name,
                conn.target_comp_id, conn.target_port_name), 400);
        }
    });

    // POST /app/connections
    endpoint = std::format("/{}/{}", APP, CONNECTIONS);
    server->Post(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        auto result = connection_request::parse(req.body);
        if (auto* err = std::get_if<std::string>(&result)) {
            return error(res, *err, 400);
        }
        auto conn = std::get<connection_request>(result);

        auto source_comp = app.get_component(conn.source_comp_id);
        if (!source_comp) {
            return error(res, std::format("source component not found: {}", conn.source_comp_id), 404);
        }

        auto target_comp = app.get_component(conn.target_comp_id);
        if (!target_comp) {
            return error(res, std::format("target component not found: {}", conn.target_comp_id), 404);
        }

        if (source_comp->connect(conn.source_port_name, target_comp, conn.target_port_name)) {
            json_created(res, {
                {"success", std::format("connected {}:{} to {}:{}",
                    conn.source_comp_id, conn.source_port_name,
                    conn.target_comp_id, conn.target_port_name)},
                {"connection", conn.to_json()}
            });
        } else {
            error(res, std::format("failed to connect {}:{} to {}:{} (check port types and names)",
                conn.source_comp_id, conn.source_port_name,
                conn.target_comp_id, conn.target_port_name), 400);
        }
    });

    return server;
}

} // namespace composite
