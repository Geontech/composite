/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "composite/core/application.hpp"
#include "composite/metrics/metrics.hpp"
#include "composite/version.hpp" // COMPOSITE_VERSION for the OpenAPI document

#include "helpers.hpp"
#include "property_handlers/common.hpp" // CORS/JSON response helpers

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <format>
#include <httplib.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <stdexcept>
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
    json_obj["properties"] = comp.property_state();
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
using property_handlers::error;
using property_handlers::json_created;
using property_handlers::json_ok;
using property_handlers::set_cors;

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

        return connection_request{output["component"].get<std::string>(), output["port"].get<std::string>(),
                                  input["component"].get<std::string>(), input["port"].get<std::string>()};
    }

    [[nodiscard]] auto to_json() const -> nlohmann::json {
        return {{"output", {{"component", source_comp_id}, {"port", source_port_name}}},
                {"input", {{"component", target_comp_id}, {"port", target_port_name}}}};
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
    std::visit(
        [&json_obj](auto&& val) {
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
        },
        snap.value);

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

namespace {

/// One documented REST route. rest_catalog() is the single source for the served OpenAPI
/// document; it must be kept in lockstep with the routes registered in make_server() (the
/// "openapi catalog matches the registered routes" integration test guards against drift).
struct route_doc {
    std::string_view method; ///< lowercase HTTP method: get|post|patch|put|delete
    std::string_view path;   ///< OpenAPI-templated path (httplib uses :param; OpenAPI uses {param})
    std::string_view summary;
};

/// The complete REST control-plane surface, in OpenAPI {param} path templating.
inline auto rest_catalog() -> const std::vector<route_doc>& {
    static const std::vector<route_doc> catalog = {
        {"get", "/app/healthz", "Liveness probe (always 200)"},
        {"get", "/app/openapi.json", "This OpenAPI 3.1 API description"},
        {"get", "/app/metrics", "Snapshot of all metrics"},
        {"get", "/app/metrics/stream", "Server-Sent Events metrics stream"},
        {"get", "/app", "Full application graph"},
        {"post", "/app/start", "Start (reconcile to desired-enabled) all components"},
        {"post", "/app/stop", "Stop all component workers"},
        {"get", "/app/components", "List all components"},
        {"post", "/app/components", "Create and add a component"},
        {"patch", "/app/components", "Multi-component property batch (207 on partial failure)"},
        {"get", "/app/components/{id}", "Get one component"},
        {"delete", "/app/components/{id}", "Stop, disconnect, and unload a component"},
        {"patch", "/app/components/{id}", "Set a batch of properties on one component"},
        {"get", "/app/components/{id}/schema", "JSON Schema of the component's properties"},
        {"get", "/app/components/{id}/properties", "Full property state"},
        {"get", "/app/components/{id}/properties/{name}", "Get one property value"},
        {"put", "/app/components/{id}/properties/{name}", "Set/replace one property"},
        {"patch", "/app/components/{id}/properties/{name}", "Merge one property (RFC-7396)"},
        {"delete", "/app/components/{id}/properties/{name}", "Reset one property to its default"},
        {"get", "/app/components/{id}/ports", "List ports with connection status"},
        {"get", "/app/components/{id}/ports/{port_name}", "Get one port's details"},
        {"delete", "/app/components/{id}/ports/{port_name}/connections", "Disconnect all of a port's connections"},
        {"post", "/app/connections", "Create a connection"},
        {"delete", "/app/connections", "Remove a specific connection"},
    };
    return catalog;
}

/// Build the OpenAPI 3.1 document for the control plane from rest_catalog().
inline auto build_openapi(std::string_view app_name) -> nlohmann::json {
    auto paths = nlohmann::json::object();
    for (const auto& r : rest_catalog()) {
        paths[std::string{r.path}][std::string{r.method}] = {
            {"summary", r.summary},
            {"responses", {{"default", {{"description", "See the composite README REST reference."}}}}},
        };
    }
    return {
        {"openapi", "3.1.0"},
        {"info",
         {
             {"title", std::format("{} — composite control plane", app_name)},
             {"version", COMPOSITE_VERSION},
             {"description", "REST control plane for a composite streaming application. Property "
                             "values are native typed JSON; see the README for full semantics."},
         }},
        {"paths", std::move(paths)},
    };
}

} // namespace

auto set_component_properties(composite::application::component_ptr comp, const nlohmann::json& properties)
    -> httplib::Response {
    auto res = httplib::Response{};
    try {
        spdlog::trace("patching component-level properties on {}", comp->id());

        // Park the worker, apply the JSON batch atomically (validate-all-then-commit-all)
        // and run property_change_handler(); then flush any enabled-toggle lifecycle change.
        comp->set_properties(properties, composite::properties::config_type::RUNTIME);
        comp->apply_lifecycle_changes();

        json_ok(res, {{"success", std::format("successfully set properties on component {}", comp->id())}});
    } catch (const composite::properties::config_violation& ex) {
        error(res, ex.what(), 403);
    } catch (const composite::properties::unknown_property& ex) {
        error(res, ex.what(), 404);
    } catch (const composite::properties::validation_error& ex) {
        error(res, ex.what(), 400);
    } catch (const std::exception& ex) {
        error(res, ex.what(), 400);
    }
    return res;
}

#ifdef COMPOSITE_USE_OPENSSL
auto make_server(application& app, const std::string& cert, const std::string& key, const std::string& ca)
    -> std::unique_ptr<httplib::Server> {
    auto server = std::make_unique<httplib::SSLServer>(cert.c_str(), key.c_str(), ca.c_str());
#else
auto make_server(application& app) -> std::unique_ptr<httplib::Server> {
    auto server = std::make_unique<httplib::Server>();
#endif

    // ------------------------------------------------------------------------
    // Control-plane hardening: central exception barrier, request-size cap, and
    // SSE stream admission control. Shared by both server variants (post-#endif).
    // ------------------------------------------------------------------------
    // Any handler that throws lands here and returns a clean JSON error with an
    // appropriate status instead of dropping the connection. Per-route handlers
    // may map specific exceptions to richer statuses first; this is the net that
    // guarantees no exception escapes into httplib's worker loop.
    server->set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
        try {
            std::rethrow_exception(ep);
        } catch (const composite::properties::config_violation& ex) {
            error(res, ex.what(), 403);
        } catch (const composite::properties::unknown_property& ex) {
            error(res, ex.what(), 404);
        } catch (const composite::properties::validation_error& ex) {
            error(res, ex.what(), 400);
        } catch (const nlohmann::json::exception& ex) {
            error(res, ex.what(), 400);
        } catch (const std::invalid_argument& ex) {
            error(res, ex.what(), 400);
        } catch (const std::out_of_range& ex) {
            error(res, ex.what(), 400);
        } catch (const std::exception& ex) {
            spdlog::error("unhandled exception in REST handler: {}", ex.what());
            error(res, ex.what(), 500);
        } catch (...) {
            error(res, "internal server error", 500);
        }
    });

    // Bound request bodies so an oversized/hostile payload cannot exhaust memory.
    server->set_payload_max_length(8ULL * 1024 * 1024); // 8 MiB

    // Concurrent SSE metric streams are capped: each stream pins a server worker
    // thread for its lifetime, so unbounded streams would drain the pool and wedge
    // the whole API. A RAII guard (below) releases the slot when the stream's
    // content provider is destroyed (client disconnect or shutdown).
    static std::atomic<int> sse_active{0};
    static constexpr int max_sse_streams = 8;

    // Add a common handler for preflight requests (OPTIONS method)
    server->Options(".*", [](const httplib::Request&, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");                                  // allow all origins
        res.set_header("Access-Control-Allow-Methods", "GET, POST, PATCH, DELETE, OPTIONS"); // allowed HTTP methods
        res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");       // allowed headers
        res.set_header("Access-Control-Max-Age", "86400"); // cache preflight response for 1 day
        res.status = 204;                                  // no content
    });

    const auto APP = std::string{"app"};
    const auto COMPONENTS = std::string{"components"};
    const auto CONNECTIONS = std::string{"connections"};

    // GET health
    auto endpoint = std::format("/{}/healthz", APP);
    server->Get(endpoint, [](const httplib::Request&, httplib::Response& res) { json_ok(res, {}); });

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
            snapshots =
                registry.snapshot_by_label(req.get_param_value("label_key"), req.get_param_value("label_value"));
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
        // Admission control: bound the number of concurrent streams.
        if (sse_active.fetch_add(1, std::memory_order_acq_rel) >= max_sse_streams) {
            sse_active.fetch_sub(1, std::memory_order_acq_rel);
            return error(res, "too many concurrent metric streams", 503);
        }
        // Release the slot when this stream ends. The guard is copied into the
        // content-provider lambda below, so the decrement runs when httplib
        // destroys the provider (client disconnect or server shutdown).
        auto sse_slot =
            std::shared_ptr<void>(nullptr, [](void*) { sse_active.fetch_sub(1, std::memory_order_acq_rel); });

        // Parse interval from query params (default 1000ms)
        int interval_ms = 1000;
        if (req.has_param("interval")) {
            try {
                interval_ms = std::stoi(req.get_param_value("interval"));
                if (interval_ms < 100) interval_ms = 100;     // Min 100ms
                if (interval_ms > 60000) interval_ms = 60000; // Max 60s
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

        res.set_chunked_content_provider("text/event-stream",
                                         [interval_ms, prefix_filter, label_key, label_value,
                                          sse_slot](std::size_t /*offset*/, httplib::DataSink& sink) -> bool {
                                             // Check if client disconnected before doing any work
                                             if (sink.is_writable != nullptr && !sink.is_writable()) {
                                                 return false; // Client disconnected
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
                                                 return false; // Connection closed
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
                                                     return false; // Client disconnected
                                                 }
                                             }

                                             return true; // Continue streaming
                                         });
    });

    // GET application
    endpoint = std::format("/{}", APP);
    server->Get(endpoint,
                [&app](const httplib::Request&, httplib::Response& res) { json_ok(res, nlohmann::json(app)); });

    // GET /app/openapi.json - machine-readable OpenAPI 3.1 description of this control plane
    // (generated from rest_catalog()), so clients/UIs can discover the API and generate bindings.
    endpoint = std::format("/{}/openapi.json", APP);
    server->Get(endpoint,
                [&app](const httplib::Request&, httplib::Response& res) { json_ok(res, build_openapi(app.name())); });

    // POST /app/start - reconcile every component toward its desired `enabled` state
    // (starts the enabled ones). Safe to call repeatedly.
    endpoint = std::format("/{}/start", APP);
    server->Post(endpoint, [&app](const httplib::Request&, httplib::Response& res) {
        app.start();
        json_ok(res, {{"success", std::format("started application '{}'", app.name())}});
    });

    // POST /app/stop - stop every component's worker. Idempotent; the REST server keeps running.
    endpoint = std::format("/{}/stop", APP);
    server->Post(endpoint, [&app](const httplib::Request&, httplib::Response& res) {
        app.stop();
        json_ok(res, {{"success", std::format("stopped application '{}'", app.name())}});
    });

    // GET components
    endpoint = std::format("/{}/{}", APP, COMPONENTS);
    server->Get(endpoint, [&app](const httplib::Request&, httplib::Response& res) {
        json_ok(res, nlohmann::json(app.components()));
    });

    // POST components
    endpoint = std::format("/{}/{}", APP, COMPONENTS);
    server->Post(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
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

            auto comp_ptr = composite::make_component(comp_json);
            if (comp_ptr == nullptr) {
                auto msg = std::format("failed to create component {} from library {}", comp_id,
                                       comp_json["library"].get<std::string>());
                spdlog::error(msg);
                return error(res, msg, 500);
            }

            if (comp_json.contains("properties")) {
                spdlog::trace("setting component-level properties on {}", comp_ptr->id());
                comp_ptr->set_properties(comp_json["properties"], composite::properties::config_type::INITIALIZE,
                                         /*allow_unknown=*/true);
            }

            if (!app.add_component(comp_ptr)) {
                return error(res, std::format("component id already exists: {}", comp_id), 409);
            }
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

        if (!json_body.contains("components") || !json_body["components"].is_array()) {
            return error(res, "components not provided", 400);
        }

        // Best-effort batch: each component's set_properties is itself atomic
        // (validate-all-then-commit-all), but the batch is NOT transactional across
        // components — earlier components stay applied (committed live). Crucially we do NOT
        // mask a failure behind a later success (the prior code overwrote `res` each
        // iteration, so a mid-batch failure could be reported as 200). Instead, attempt
        // every component, aggregate per-component outcomes, and return 207 Multi-Status if
        // any failed so the client can see exactly what did/didn't apply and reconcile.
        auto results = nlohmann::json::array();
        std::size_t applied = 0;
        std::size_t failed = 0;
        auto record = [&](const std::string& id, int status, std::string_view detail) {
            nlohmann::json entry{{"status", status}};
            if (!id.empty()) {
                entry["id"] = id;
            }
            if (status >= 400) {
                entry["error"] = detail;
                ++failed;
            } else {
                ++applied;
            }
            results.push_back(std::move(entry));
        };

        for (const auto& comp_json : json_body["components"]) {
            if (!comp_json.contains("id") || !comp_json["id"].is_string()) {
                record("", 400, "component id not provided");
                continue;
            }
            auto comp_id = comp_json["id"].get<std::string>();
            if (!comp_json.contains("properties")) {
                record(comp_id, 400, "component properties not provided");
                continue;
            }
            auto comp = app.get_component(comp_id);
            if (comp == nullptr) {
                record(comp_id, 404, "component not found");
                continue;
            }
            auto comp_res = set_component_properties(comp, comp_json["properties"]);
            if (comp_res.status >= 400) {
                // set_component_properties returns {"error": "..."} on failure; surface it.
                std::string detail = comp_res.body;
                try {
                    detail = nlohmann::json::parse(comp_res.body).value("error", comp_res.body);
                } catch (...) { /* keep raw body */
                }
                record(comp_id, comp_res.status, detail);
            } else {
                record(comp_id, comp_res.status, {});
            }
        }

        if (failed > 0) {
            set_cors(res);
            res.status = 207; // Multi-Status: at least one component in the batch failed
            res.set_content(nlohmann::json{{"applied", applied}, {"failed", failed}, {"results", results}}.dump(2),
                            "application/json");
        } else {
            json_ok(res,
                    {{"success", std::format("set properties on {} component(s)", applied)}, {"results", results}});
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

    // DELETE component by ID - stop it, quiesce its connections, and unload it.
    // remove_component() does the connection-safe teardown (parked producer disconnects +
    // own-output disconnects); dropping the returned last owner runs ~component (idempotent
    // stop) and unmaps the library via the deleter-owned dlopen handle.
    endpoint = std::format("/{}/{}/:id", APP, COMPONENTS);
    server->Delete(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto removed = app.remove_component(comp_id);
        if (removed == nullptr) {
            return error(res, std::format("component not found: {}", comp_id), 404);
        }
        json_ok(res, {{"success", std::format("removed {} from application '{}'", comp_id, app.name())}});
        // `removed` drops here -> ~component (idempotent stop) + dlclose.
    });

    // ========================================================================
    // Property Operations (JSON in / JSON out; RFC-7396 semantics)
    //   PATCH a struct/keyed property with a partial object; null resets/erases.
    //   The old index-based list and struct-field routes are replaced by keyed
    //   collections addressed as nested JSON under the property name.
    // ========================================================================

    // GET /app/components/:id/schema  -> JSON Schema for the component's properties
    // (types, defaults, units, ranges, enum choices) — drives auto-generated config UIs.
    endpoint = std::format("/{}/{}/:id/schema", APP, COMPONENTS);
    server->Get(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto comp = app.get_component(comp_id);
        if (!comp) {
            return error(res, std::format("component not found: {}", comp_id), 404);
        }
        json_ok(res, comp->property_schema()); // takes the read lock internally
    });

    // GET /app/components/:id/properties  -> full property state
    endpoint = std::format("/{}/{}/:id/properties", APP, COMPONENTS);
    server->Get(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto comp = app.get_component(comp_id);
        if (!comp) {
            return error(res, std::format("component not found: {}", comp_id), 404);
        }
        auto state = comp->property_state(); // property_state() takes the read lock internally
        json_ok(res, state);
    });

    // GET /app/components/:id/properties/:name  -> single property value
    endpoint = std::format("/{}/{}/:id/properties/:name", APP, COMPONENTS);
    server->Get(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto prop_name = req.path_params.at("name");
        auto comp = app.get_component(comp_id);
        if (!comp) {
            return error(res, std::format("component not found: {}", comp_id), 404);
        }
        auto state = comp->property_state(); // property_state() takes the read lock internally
        if (!state.contains(prop_name)) {
            return error(res, std::format("property not found: {}", prop_name), 404);
        }
        json_ok(res, {{prop_name, state.at(prop_name)}});
    });

    // PUT/PATCH /app/components/:id/properties/:name  -> set/merge a single property
    endpoint = std::format("/{}/{}/:id/properties/:name", APP, COMPONENTS);
    auto put_one = [&app](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto prop_name = req.path_params.at("name");
        auto comp = app.get_component(comp_id);
        if (!comp) {
            return error(res, std::format("component not found: {}", comp_id), 404);
        }
        nlohmann::json body;
        try {
            body = nlohmann::json::parse(req.body);
        } catch (...) {
            return error(res, "invalid json request", 400);
        }
        res = set_component_properties(comp, {{prop_name, property_handlers::extract_value(body)}});
    };
    server->Put(endpoint, put_one);
    server->Patch(endpoint, put_one);

    // DELETE /app/components/:id/properties/:name  -> reset to default (RFC-7396 null)
    endpoint = std::format("/{}/{}/:id/properties/:name", APP, COMPONENTS);
    server->Delete(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto prop_name = req.path_params.at("name");
        auto comp = app.get_component(comp_id);
        if (!comp) {
            return error(res, std::format("component not found: {}", comp_id), 404);
        }
        res = set_component_properties(comp, {{prop_name, nullptr}});
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
            if (port) {
                ports_json.push_back(port_to_json(name, port));
            }
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
        // Hold the topology lock across resolve + disconnect so this cannot interleave a
        // concurrent component removal (would otherwise risk an edge mutation on a dying peer).
        auto topo = app.topology_lock();
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

        if (dynamic_cast<output_port_base*>(it->second) == nullptr) {
            return error(res, std::format("port '{}' is not an output port", port_name), 400);
        }

        // Route through component::disconnect_all so the worker is parked around
        // the producer-claim release and m_connections is updated.
        auto count = comp->disconnect_all(port_name);
        json_ok(res, {{"success", std::format("disconnected {} connections from port '{}'", count, port_name)},
                      {"disconnected_count", count}});
    });

    // DELETE /app/connections
    endpoint = std::format("/{}/{}", APP, CONNECTIONS);
    server->Delete(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        // Serialize edge mutation against a concurrent remove_component (see topology_lock()).
        auto topo = app.topology_lock();
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

        if (dynamic_cast<output_port_base*>(source_it->second) == nullptr) {
            return error(res, std::format("source port '{}' is not an output port", conn.source_port_name), 400);
        }

        const auto& target_ports = target_comp->ports();
        auto target_it = target_ports.find(conn.target_port_name);
        if (target_it == target_ports.end() || !target_it->second) {
            return error(res, std::format("target port not found: {}", conn.target_port_name), 404);
        }

        if (dynamic_cast<input_port_base*>(target_it->second) == nullptr) {
            return error(res, std::format("target port '{}' is not an input port", conn.target_port_name), 400);
        }

        // Route through component::disconnect so the worker is parked around the
        // producer-claim release and m_connections is updated.
        if (source_comp->disconnect(conn.source_port_name, target_comp, conn.target_port_name)) {
            json_ok(res, {{"success", std::format("disconnected {}:{} from {}:{}", conn.source_comp_id,
                                                  conn.source_port_name, conn.target_comp_id, conn.target_port_name)},
                          {"connection", conn.to_json()}});
        } else {
            error(res,
                  std::format("ports were not connected: {}:{} -> {}:{}", conn.source_comp_id, conn.source_port_name,
                              conn.target_comp_id, conn.target_port_name),
                  400);
        }
    });

    // POST /app/connections
    endpoint = std::format("/{}/{}", APP, CONNECTIONS);
    server->Post(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        // Hold the topology lock across resolve + connect: serializes against a concurrent
        // DELETE /app/components/:id so a connect cannot create an edge into a component that
        // remove_component has erased and is tearing down (would be a send-into-freed-ring UAF).
        // Because both endpoints are re-resolved via get_component UNDER this lock, a removed
        // component is simply not found here and the connect is rejected (404).
        auto topo = app.topology_lock();
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
            json_created(res,
                         {{"success", std::format("connected {}:{} to {}:{}", conn.source_comp_id,
                                                  conn.source_port_name, conn.target_comp_id, conn.target_port_name)},
                          {"connection", conn.to_json()}});
        } else {
            error(res,
                  std::format("failed to connect {}:{} to {}:{} (check port types and names)", conn.source_comp_id,
                              conn.source_port_name, conn.target_comp_id, conn.target_port_name),
                  400);
        }
    });

    return server;
}

} // namespace composite
