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

#include "composite/core/application.hpp"
#include "property_serializer.hpp"

#include "helpers.hpp"
#include "property_changeset.hpp"
#include "property_rest_api.hpp"

#include <cstdlib>
#include <format>
#include <httplib.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

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

auto to_json(nlohmann::json& json_obj, const composite::property& prop) {
    property_serializer::to_json(json_obj, prop);
}

auto to_json(nlohmann::json& json_obj, const composite::property_set& set) -> void {
    property_serializer::to_json(json_obj, set);
}

auto to_json(nlohmann::json& json_obj, const component& comp) {
    json_obj["id"] = comp.id();
    json_obj["name"] = comp.name();
    json_obj["ports"] = comp.ports();
    json_obj["connections"] = comp.connections();
    auto props_obj = nlohmann::json{};
    for (const auto& [name, value] : comp.properties()) {
        auto prop_obj = nlohmann::json::object();
        to_json(prop_obj, value);
        props_obj[name] = prop_obj;
    }
    json_obj["properties"] = props_obj;
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

auto set_cors_header(httplib::Response& res) -> void {
    res.set_header("Access-Control-Allow-Origin", "*"); // allow all origins
}

auto set_component_properties(
  composite::application::component_ptr comp,
  const nlohmann::json& properties
) -> httplib::Response {
    auto res = httplib::Response{};
    auto content = nlohmann::json();
    try {
        spdlog::trace("patching component-level properties on {}", comp->id());

        // Parse property changeset
        auto changeset = property_changeset::from_json(properties);

        // Apply changes if there are any
        if (changeset.has_updates()) {
            if (!changeset.scalar_properties().empty()) {
                comp->set_properties(changeset.scalar_properties(), composite::properties::config_type::RUNTIME);
            }
            if (!changeset.list_properties().empty()) {
                comp->set_properties(changeset.list_properties(), composite::properties::config_type::RUNTIME);
            }
            if (!changeset.struct_properties().empty()) {
                comp->set_properties(changeset.struct_properties(), composite::properties::config_type::RUNTIME);
            }
            comp->property_change_handler();

            // Apply any pending lifecycle changes (start/stop based on "enabled" property)
            // This must be done AFTER set_properties() completes to avoid deadlock
            comp->apply_lifecycle_changes();
        }

        content["success"] = std::format("successfully set properties on component {}", comp->id());
        res.set_content(content.dump(), "application/json");
        res.status = httplib::OK_200;
    } catch (const std::exception& ex) {
        content["error"] = ex.what();
        res.set_content(content.dump(), "application/json");
        res.status = httplib::BadRequest_400;
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
    server->Get(endpoint, [&app](const httplib::Request&, httplib::Response& res) {
        set_cors_header(res);
        res.status = httplib::OK_200;
    });

    // GET application
    endpoint = std::format("/{}", APP);
    server->Get(endpoint, [&app](const httplib::Request&, httplib::Response& res) {
        set_cors_header(res);
        auto app_json = nlohmann::json(app);
        res.set_content(app_json.dump(2), "application/json");
        res.status = httplib::OK_200;
    });

    // GET components
    endpoint = std::format("/{}/{}", APP, COMPONENTS);
    server->Get(endpoint, [&app](const httplib::Request&, httplib::Response& res) {
        set_cors_header(res);
        auto comps_json = nlohmann::json(app.components());
        res.set_content(comps_json.dump(2), "application/json");
        res.status = httplib::OK_200;
    });

    // POST components
    endpoint = std::format("/{}/{}", APP, COMPONENTS);
    server->Post(endpoint, [&app, &handles](const httplib::Request& req, httplib::Response& res) {
        set_cors_header(res);
        try {
            // Parse JSON body
            auto comp_json = nlohmann::json::parse(req.body);
            // Check for name
            if (!comp_json.contains("name")) {
                auto content = nlohmann::json::object();
                content["error"] = std::string{"no component name provided"};
                res.set_content(content.dump(), "application/json");
                res.status = httplib::BadRequest_400;
                return;
            }
            // Add component to application
            auto comp_ptr = composite::make_component(comp_json, handles);
            if (comp_ptr == nullptr) {
                auto msg = std::format("failed to create component {}", comp_json["name"].get<std::string>());
                auto content = nlohmann::json::object();
                content["error"] = msg;
                spdlog::error(msg);
                res.set_content(content.dump(), "application/json");
                res.status = httplib::InternalServerError_500;
                return;
            }
            // Set properties
            if (comp_json.contains("properties")) {
                try {
                    // Set component-level properties
                    spdlog::trace("setting component-level properties on {}", comp_ptr->id());
                    auto props = std::vector<std::pair<std::string,std::string>>{};
                    for (const auto& [name, value] : comp_json["properties"].get<std::map<std::string,std::string>>()) {
                        props.emplace_back(name, value);
                    }
                    comp_ptr->set_properties(props);
                } catch (const std::runtime_error& err) {
                    auto content = nlohmann::json();
                    content["error"] = err.what();
                    res.set_content(content.dump(), "application/json");
                    res.status = httplib::InternalServerError_500;
                }
            }
            // Add to application
            app.add_component(comp_ptr);
            auto msg = std::format("added {} to application '{}'", comp_ptr->id(), app.name());
            spdlog::trace(msg);
            auto content = nlohmann::json::object();
            content["success"] = msg;
            res.set_content(content.dump(), "application/json");
            res.status = httplib::Created_201;
        } catch (const std::exception& ex) {
            auto content = nlohmann::json();
            content["error"] = ex.what();
            res.set_content(content.dump(), "application/json");
            res.status = httplib::BadRequest_400;
        }
    });

    // GET component by ID
    endpoint = std::format("/{}/{}/:id", APP, COMPONENTS);
    server->Get(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        set_cors_header(res);
        auto comp_id = req.path_params.at("id");
        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            auto comp_json = nlohmann::json(*comp);
            res.set_content(comp_json.dump(), "application/json");
            res.status = httplib::OK_200;
        } else {
            auto content = nlohmann::json::object();
            content["error"] = std::format("component not found: {}", comp_id);
            res.set_content(content.dump(), "application/json");
            res.status = httplib::BadRequest_400;
        }
    });

    // PATCH components
    endpoint = std::format("/{}/{}", APP, COMPONENTS);
    server->Patch(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        set_cors_header(res);
        auto content = nlohmann::json();
        auto json_body = nlohmann::json();
        // Parse request
        try {
            json_body = nlohmann::json::parse(req.body);
        } catch (...) {
            content["error"] = std::string{"invalid json request"};
            res.set_content(content.dump(), "application/json");
            res.status = httplib::BadRequest_400;
            return;
        }
        // Check for components key
        if (!json_body.contains("components")) {
            content["error"] = std::string{"components not provided"};
            res.set_content(content.dump(), "application/json");
            res.status = httplib::BadRequest_400;
            return;
        }
        for (const auto& comp_json : json_body["components"]) {
            // Check for id key
            if (!comp_json.contains("id")) {
                content["error"] = std::string{"component id not provided"};
                res.set_content(content.dump(), "application/json");
                res.status = httplib::BadRequest_400;
                return;
            }
            auto comp_id = comp_json["id"].get<std::string>();
            // Check for properties key
            if (!comp_json.contains("properties")) {
                content["error"] = std::format("component properties not provided for {}", comp_id);
                res.set_content(content.dump(), "application/json");
                res.status = httplib::BadRequest_400;
                return;
            }
            // Get component and set properties
            if (auto comp = app.get_component(comp_id); comp != nullptr) {
                res = set_component_properties(comp, comp_json["properties"]);
            } else {
                content["error"] = std::format("component not found: {}", comp_id);
                res.set_content(content.dump(), "application/json");
                res.status = httplib::NotFound_404;
            }
        }
    });

    // PATCH component by ID
    endpoint = std::format("/{}/{}/:id", APP, COMPONENTS);
    server->Patch(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        set_cors_header(res);
        auto content = nlohmann::json();
        auto comp_id = req.path_params.at("id");
        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            // Parse request
            auto json_body = nlohmann::json();
            try {
                json_body = nlohmann::json::parse(req.body);
            } catch (...) {
                content["error"] = std::string{"invalid json request"};
                res.set_content(content.dump(), "application/json");
                res.status = httplib::BadRequest_400;
                return;
            }
            if (!json_body.contains("properties")) {
                content["error"] = std::string{"properties not provided"};
                res.set_content(content.dump(), "application/json");
                res.status = httplib::BadRequest_400;
                return;
            }
            res = set_component_properties(comp, json_body["properties"]);
        } else {
            content["error"] = std::format("component not found: {}", comp_id);
            res.set_content(content.dump(), "application/json");
            res.status = httplib::NotFound_404;
        }
    });

    // GET /app/components/:id/properties
    endpoint = std::format("/{}/{}/:id/properties", APP, COMPONENTS);
    server->Get(endpoint, [&app, &APP, &COMPONENTS](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto path = std::format("/{}/{}/{}/properties", APP, COMPONENTS, comp_id);

        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            res = properties::rest::property_handlers::get_properties_collection(comp.get(), req, path);
        } else {
            res = properties::rest::rest_helpers::error_response_obj(
                properties::rest::error_code::COMPONENT_NOT_FOUND,
                std::format("Component '{}' not found", comp_id),
                path
            );
        }
    });

    // GET /app/components/:id/properties/:name
    endpoint = std::format("/{}/{}/:id/properties/:name", APP, COMPONENTS);
    server->Get(endpoint, [&app, &APP, &COMPONENTS](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto prop_name = req.path_params.at("name");
        auto path = std::format("/{}/{}/{}/properties/{}", APP, COMPONENTS, comp_id, prop_name);

        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            res = properties::rest::property_handlers::get_property(comp.get(), prop_name, path);
        } else {
            res = properties::rest::rest_helpers::error_response_obj(
                properties::rest::error_code::COMPONENT_NOT_FOUND,
                std::format("Component '{}' not found", comp_id),
                path
            );
        }
    });

    // PUT /app/components/:id/properties/:name
    endpoint = std::format("/{}/{}/:id/properties/:name", APP, COMPONENTS);
    server->Put(endpoint, [&app, &APP, &COMPONENTS](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto prop_name = req.path_params.at("name");
        auto path = std::format("/{}/{}/{}/properties/{}", APP, COMPONENTS, comp_id, prop_name);

        // Parse JSON body
        auto [json_body, parse_error] = properties::rest::rest_helpers::parse_json_body(req.body, path);
        if (parse_error) {
            res = *parse_error;
            return;
        }

        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            res = properties::rest::property_handlers::put_property(comp.get(), prop_name, json_body, path);
        } else {
            res = properties::rest::rest_helpers::error_response_obj(
                properties::rest::error_code::COMPONENT_NOT_FOUND,
                std::format("Component '{}' not found", comp_id),
                path
            );
        }
    });

    // PATCH /app/components/:id/properties/:name
    endpoint = std::format("/{}/{}/:id/properties/:name", APP, COMPONENTS);
    server->Patch(endpoint, [&app, &APP, &COMPONENTS](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto prop_name = req.path_params.at("name");
        auto path = std::format("/{}/{}/{}/properties/{}", APP, COMPONENTS, comp_id, prop_name);

        // Parse JSON body
        auto [json_body, parse_error] = properties::rest::rest_helpers::parse_json_body(req.body, path);
        if (parse_error) {
            res = *parse_error;
            return;
        }

        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            res = properties::rest::property_handlers::patch_property(comp.get(), prop_name, json_body, path);
        } else {
            res = properties::rest::rest_helpers::error_response_obj(
                properties::rest::error_code::COMPONENT_NOT_FOUND,
                std::format("Component '{}' not found", comp_id),
                path
            );
        }
    });

    // DELETE /app/components/:id/properties/:name
    endpoint = std::format("/{}/{}/:id/properties/:name", APP, COMPONENTS);
    server->Delete(endpoint, [&app, &APP, &COMPONENTS](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto prop_name = req.path_params.at("name");
        auto path = std::format("/{}/{}/{}/properties/{}", APP, COMPONENTS, comp_id, prop_name);

        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            res = properties::rest::property_handlers::delete_property(comp.get(), prop_name, path);
        } else {
            res = properties::rest::rest_helpers::error_response_obj(
                properties::rest::error_code::COMPONENT_NOT_FOUND,
                std::format("Component '{}' not found", comp_id),
                path
            );
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
        auto path = std::format("/{}/{}/{}/properties/{}/items", APP, COMPONENTS, comp_id, prop_name);
        auto base_path = path;

        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            res = properties::rest::property_handlers::get_list_items(comp.get(), prop_name, path, base_path);
        } else {
            res = properties::rest::rest_helpers::error_response_obj(
                properties::rest::error_code::COMPONENT_NOT_FOUND,
                std::format("Component '{}' not found", comp_id),
                path
            );
        }
    });

    // GET /app/components/:id/properties/:name/items/:index
    endpoint = std::format("/{}/{}/:id/properties/:name/items/:index", APP, COMPONENTS);
    server->Get(endpoint, [&app, &APP, &COMPONENTS](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto prop_name = req.path_params.at("name");
        auto index_str = req.path_params.at("index");
        auto path = std::format("/{}/{}/{}/properties/{}/items/{}", APP, COMPONENTS, comp_id, prop_name, index_str);

        // Parse index
        std::size_t index;
        try {
            index = std::stoull(index_str);
        } catch (const std::exception&) {
            auto details = nlohmann::json::object();
            details["index"] = index_str;
            res = properties::rest::rest_helpers::error_response_obj(
                properties::rest::error_code::INVALID_INDEX,
                std::format("Invalid index: {}", index_str),
                path,
                details
            );
            return;
        }

        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            res = properties::rest::property_handlers::get_list_item(comp.get(), prop_name, index, path);
        } else {
            res = properties::rest::rest_helpers::error_response_obj(
                properties::rest::error_code::COMPONENT_NOT_FOUND,
                std::format("Component '{}' not found", comp_id),
                path
            );
        }
    });

    // POST /app/components/:id/properties/:name/items (append)
    endpoint = std::format("/{}/{}/:id/properties/:name/items", APP, COMPONENTS);
    server->Post(endpoint, [&app, &APP, &COMPONENTS](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto prop_name = req.path_params.at("name");
        auto path = std::format("/{}/{}/{}/properties/{}/items", APP, COMPONENTS, comp_id, prop_name);
        auto base_path = path;

        // Parse JSON body
        auto [json_body, parse_error] = properties::rest::rest_helpers::parse_json_body(req.body, path);
        if (parse_error) {
            res = *parse_error;
            return;
        }

        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            res = properties::rest::property_handlers::post_list_item(comp.get(), prop_name, json_body, path, base_path);
        } else {
            res = properties::rest::rest_helpers::error_response_obj(
                properties::rest::error_code::COMPONENT_NOT_FOUND,
                std::format("Component '{}' not found", comp_id),
                path
            );
        }
    });

    // PUT /app/components/:id/properties/:name/items/:index
    endpoint = std::format("/{}/{}/:id/properties/:name/items/:index", APP, COMPONENTS);
    server->Put(endpoint, [&app, &APP, &COMPONENTS](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto prop_name = req.path_params.at("name");
        auto index_str = req.path_params.at("index");
        auto path = std::format("/{}/{}/{}/properties/{}/items/{}", APP, COMPONENTS, comp_id, prop_name, index_str);

        // Parse index
        std::size_t index;
        try {
            index = std::stoull(index_str);
        } catch (const std::exception&) {
            auto details = nlohmann::json::object();
            details["index"] = index_str;
            res = properties::rest::rest_helpers::error_response_obj(
                properties::rest::error_code::INVALID_INDEX,
                std::format("Invalid index: {}", index_str),
                path,
                details
            );
            return;
        }

        // Parse JSON body
        auto [json_body, parse_error] = properties::rest::rest_helpers::parse_json_body(req.body, path);
        if (parse_error) {
            res = *parse_error;
            return;
        }

        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            res = properties::rest::property_handlers::put_list_item(comp.get(), prop_name, index, json_body, path);
        } else {
            res = properties::rest::rest_helpers::error_response_obj(
                properties::rest::error_code::COMPONENT_NOT_FOUND,
                std::format("Component '{}' not found", comp_id),
                path
            );
        }
    });

    // DELETE /app/components/:id/properties/:name/items/:index
    endpoint = std::format("/{}/{}/:id/properties/:name/items/:index", APP, COMPONENTS);
    server->Delete(endpoint, [&app, &APP, &COMPONENTS](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto prop_name = req.path_params.at("name");
        auto index_str = req.path_params.at("index");
        auto path = std::format("/{}/{}/{}/properties/{}/items/{}", APP, COMPONENTS, comp_id, prop_name, index_str);

        // Parse index
        std::size_t index;
        try {
            index = std::stoull(index_str);
        } catch (const std::exception&) {
            auto details = nlohmann::json::object();
            details["index"] = index_str;
            res = properties::rest::rest_helpers::error_response_obj(
                properties::rest::error_code::INVALID_INDEX,
                std::format("Invalid index: {}", index_str),
                path,
                details
            );
            return;
        }

        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            res = properties::rest::property_handlers::delete_list_item(comp.get(), prop_name, index, path);
        } else {
            res = properties::rest::rest_helpers::error_response_obj(
                properties::rest::error_code::COMPONENT_NOT_FOUND,
                std::format("Component '{}' not found", comp_id),
                path
            );
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
        auto path = std::format("/{}/{}/{}/properties/{}/fields", APP, COMPONENTS, comp_id, prop_name);
        auto base_path = path;

        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            res = properties::rest::property_handlers::get_struct_fields(comp.get(), prop_name, path, base_path);
        } else {
            res = properties::rest::rest_helpers::error_response_obj(
                properties::rest::error_code::COMPONENT_NOT_FOUND,
                std::format("Component '{}' not found", comp_id),
                path
            );
        }
    });

    // GET /app/components/:id/properties/:name/fields/:field
    endpoint = std::format("/{}/{}/:id/properties/:name/fields/:field", APP, COMPONENTS);
    server->Get(endpoint, [&app, &APP, &COMPONENTS](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto prop_name = req.path_params.at("name");
        auto field_name = req.path_params.at("field");
        auto path = std::format("/{}/{}/{}/properties/{}/fields/{}", APP, COMPONENTS, comp_id, prop_name, field_name);

        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            res = properties::rest::property_handlers::get_struct_field(comp.get(), prop_name, field_name, path);
        } else {
            res = properties::rest::rest_helpers::error_response_obj(
                properties::rest::error_code::COMPONENT_NOT_FOUND,
                std::format("Component '{}' not found", comp_id),
                path
            );
        }
    });

    // PATCH /app/components/:id/properties/:name/fields/:field
    endpoint = std::format("/{}/{}/:id/properties/:name/fields/:field", APP, COMPONENTS);
    server->Patch(endpoint, [&app, &APP, &COMPONENTS](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto prop_name = req.path_params.at("name");
        auto field_name = req.path_params.at("field");
        auto path = std::format("/{}/{}/{}/properties/{}/fields/{}", APP, COMPONENTS, comp_id, prop_name, field_name);

        // Parse JSON body
        auto [json_body, parse_error] = properties::rest::rest_helpers::parse_json_body(req.body, path);
        if (parse_error) {
            res = *parse_error;
            return;
        }

        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            res = properties::rest::property_handlers::patch_struct_field(comp.get(), prop_name, field_name, json_body, path);
        } else {
            res = properties::rest::rest_helpers::error_response_obj(
                properties::rest::error_code::COMPONENT_NOT_FOUND,
                std::format("Component '{}' not found", comp_id),
                path
            );
        }
    });

    // GET /app/components/:id/properties/:name/items/:index/fields
    endpoint = std::format("/{}/{}/:id/properties/:name/items/:index/fields", APP, COMPONENTS);
    server->Get(endpoint, [&app, &APP, &COMPONENTS](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto prop_name = req.path_params.at("name");
        auto index_str = req.path_params.at("index");
        auto path = std::format("/{}/{}/{}/properties/{}/items/{}/fields", APP, COMPONENTS, comp_id, prop_name, index_str);
        auto base_path = path;

        // Parse index
        std::size_t index;
        try {
            index = std::stoull(index_str);
        } catch (const std::exception&) {
            auto details = nlohmann::json::object();
            details["index"] = index_str;
            res = properties::rest::rest_helpers::error_response_obj(
                properties::rest::error_code::INVALID_INDEX,
                std::format("Invalid index: {}", index_str),
                path,
                details
            );
            return;
        }

        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            res = properties::rest::property_handlers::get_struct_list_item_fields(comp.get(), prop_name, index, path, base_path);
        } else {
            res = properties::rest::rest_helpers::error_response_obj(
                properties::rest::error_code::COMPONENT_NOT_FOUND,
                std::format("Component '{}' not found", comp_id),
                path
            );
        }
    });

    // PATCH /app/components/:id/properties/:name/items/:index/fields/:field
    endpoint = std::format("/{}/{}/:id/properties/:name/items/:index/fields/:field", APP, COMPONENTS);
    server->Patch(endpoint, [&app, &APP, &COMPONENTS](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto prop_name = req.path_params.at("name");
        auto index_str = req.path_params.at("index");
        auto field_name = req.path_params.at("field");
        auto path = std::format("/{}/{}/{}/properties/{}/items/{}/fields/{}", APP, COMPONENTS, comp_id, prop_name, index_str, field_name);

        // Parse index
        std::size_t index;
        try {
            index = std::stoull(index_str);
        } catch (const std::exception&) {
            auto details = nlohmann::json::object();
            details["index"] = index_str;
            res = properties::rest::rest_helpers::error_response_obj(
                properties::rest::error_code::INVALID_INDEX,
                std::format("Invalid index: {}", index_str),
                path,
                details
            );
            return;
        }

        // Parse JSON body
        auto [json_body, parse_error] = properties::rest::rest_helpers::parse_json_body(req.body, path);
        if (parse_error) {
            res = *parse_error;
            return;
        }

        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            res = properties::rest::property_handlers::patch_struct_list_item_field(comp.get(), prop_name, index, field_name, json_body, path);
        } else {
            res = properties::rest::rest_helpers::error_response_obj(
                properties::rest::error_code::COMPONENT_NOT_FOUND,
                std::format("Component '{}' not found", comp_id),
                path
            );
        }
    });

    // ========================================================================
    // Validation & Discovery Operations
    // ========================================================================

    // POST /app/components/:id/properties/:name/validate
    endpoint = std::format("/{}/{}/:id/properties/:name/validate", APP, COMPONENTS);
    server->Post(endpoint, [&app, &APP, &COMPONENTS](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto prop_name = req.path_params.at("name");
        auto path = std::format("/{}/{}/{}/properties/{}/validate", APP, COMPONENTS, comp_id, prop_name);

        // Parse JSON body
        auto [json_body, parse_error] = properties::rest::rest_helpers::parse_json_body(req.body, path);
        if (parse_error) {
            res = *parse_error;
            return;
        }

        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            res = properties::rest::property_handlers::validate_property_value(comp.get(), prop_name, json_body, path);
        } else {
            res = properties::rest::rest_helpers::error_response_obj(
                properties::rest::error_code::COMPONENT_NOT_FOUND,
                std::format("Component '{}' not found", comp_id),
                path
            );
        }
    });

    // GET /app/components/:id/properties/:name/schema
    endpoint = std::format("/{}/{}/:id/properties/:name/schema", APP, COMPONENTS);
    server->Get(endpoint, [&app, &APP, &COMPONENTS](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        auto prop_name = req.path_params.at("name");
        auto path = std::format("/{}/{}/{}/properties/{}/schema", APP, COMPONENTS, comp_id, prop_name);

        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            res = properties::rest::property_handlers::get_property_schema(comp.get(), prop_name, path);
        } else {
            res = properties::rest::rest_helpers::error_response_obj(
                properties::rest::error_code::COMPONENT_NOT_FOUND,
                std::format("Component '{}' not found", comp_id),
                path
            );
        }
    });

    // ========================================================================
    // Port Connection Operations
    // ========================================================================

    // GET /app/components/:id/ports
    endpoint = std::format("/{}/{}/:id/ports", APP, COMPONENTS);
    server->Get(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        set_cors_header(res);
        auto comp_id = req.path_params.at("id");
        auto content = nlohmann::json();

        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            auto ports_json = nlohmann::json::array();

            for (const auto& [port_name, port_ptr] : comp->ports()) {
                if (port_ptr == nullptr) { continue; }

                auto port_obj = nlohmann::json::object();
                port_obj["name"] = port_name;

                // Check if it's an output port and add connection info
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

    // GET /app/components/:id/ports/:port_name
    endpoint = std::format("/{}/{}/:id/ports/:port_name", APP, COMPONENTS);
    server->Get(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        set_cors_header(res);
        auto comp_id = req.path_params.at("id");
        auto port_name = req.path_params.at("port_name");
        auto content = nlohmann::json();

        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            const auto& ports = comp->ports();
            auto port_it = ports.find(port_name);

            if (port_it != ports.end() && port_it->second != nullptr) {
                auto port_ptr = port_it->second;
                content["name"] = port_name;
                content["component_id"] = comp_id;

                if (auto* output_port = dynamic_cast<output_port_base*>(port_ptr)) {
                    content["type"] = "output";
                    content["is_connected"] = output_port->is_connected();
                    content["connection_count"] = output_port->connection_count();
                    content["connected_ports"] = output_port->connected_ports();
                } else if (dynamic_cast<input_port_base*>(port_ptr)) {
                    content["type"] = "input";
                }

                res.set_content(content.dump(2), "application/json");
                res.status = httplib::OK_200;
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

    // DELETE /app/components/:id/ports/:port_name/connections
    endpoint = std::format("/{}/{}/:id/ports/:port_name/connections", APP, COMPONENTS);
    server->Delete(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        set_cors_header(res);
        auto comp_id = req.path_params.at("id");
        auto port_name = req.path_params.at("port_name");
        auto content = nlohmann::json();

        if (auto comp = app.get_component(comp_id); comp != nullptr) {
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
    endpoint = std::format("/{}/{}", APP, CONNECTIONS);
    server->Delete(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        set_cors_header(res);
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

            // Get source component
            auto source_comp = app.get_component(source_comp_id);
            if (!source_comp) {
                content["error"] = std::format("source component not found: {}", source_comp_id);
                res.set_content(content.dump(), "application/json");
                res.status = httplib::NotFound_404;
                return;
            }

            // Get target component
            auto target_comp = app.get_component(target_comp_id);
            if (!target_comp) {
                content["error"] = std::format("target component not found: {}", target_comp_id);
                res.set_content(content.dump(), "application/json");
                res.status = httplib::NotFound_404;
                return;
            }

            // Get source output port
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

            // Get target input port
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

            // Perform disconnect
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

    // POST /app/connections
    endpoint = std::format("/{}/{}", APP, CONNECTIONS);
    server->Post(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        set_cors_header(res);
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

            // Get source component
            auto source_comp = app.get_component(source_comp_id);
            if (!source_comp) {
                content["error"] = std::format("source component not found: {}", source_comp_id);
                res.set_content(content.dump(), "application/json");
                res.status = httplib::NotFound_404;
                return;
            }

            // Get target component
            auto target_comp = app.get_component(target_comp_id);
            if (!target_comp) {
                content["error"] = std::format("target component not found: {}", target_comp_id);
                res.set_content(content.dump(), "application/json");
                res.status = httplib::NotFound_404;
                return;
            }

            // Attempt to connect
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
                content["error"] = std::format("failed to connect {}:{} to {}:{} (check port types and names)",
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

    return server;
}

} // namespace composite
