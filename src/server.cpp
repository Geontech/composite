#include "composite/application.hpp"
#include "helpers.hpp"

#include <cstdlib>
#include <format>
#include <httplib.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace composite {

auto to_json(nlohmann::json& json_obj, const port& port) {
    json_obj["name"] = port.name();
}

auto to_json(nlohmann::json& json_obj, const std::map<std::string, port*>& ports) {
    json_obj = nlohmann::json::array();
    for (auto& [name, port] : ports) {
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
    json_obj["name"] = comp.name();
    json_obj["ports"] = comp.ports();
    json_obj["connections"] = comp.connections();
    auto props_obj = nlohmann::json::array();
    for (const auto& [name, value] : comp.properties()) {
        auto prop_obj = nlohmann::json::object();
        auto type = value.type();
        prop_obj["name"] = name;
        prop_obj["type"] = type;
        prop_obj["units"] = value.units();
        prop_obj["configurability"] = (static_cast<int>(value.configurability()) == 1) ? "runtime" : "initialize";
        if (type == "bool") {
            prop_obj["value"] = std::format("{}", comp.get_property<bool>(name));
        } else if (type == "string") {
            prop_obj["value"] = comp.get_property<std::string>(name);
        } else if (type == "int32") {
            prop_obj["value"] = std::to_string(comp.get_property<int32_t>(name));
        } else if (type == "uint32") {
            prop_obj["value"] = std::to_string(comp.get_property<uint32_t>(name));
        } else if (type == "int64") {
            prop_obj["value"] = std::to_string(comp.get_property<int64_t>(name));
        } else if (type == "uint64") {
            prop_obj["value"] = std::to_string(comp.get_property<uint64_t>(name));
        } else if (type == "float") {
            prop_obj["value"] = std::to_string(comp.get_property<float>(name));
        } else if (type == "double") {
            prop_obj["value"] = std::to_string(comp.get_property<double>(name));
        }
        props_obj.push_back(prop_obj);
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
        auto props = std::vector<std::pair<std::string,std::string>>{};
        for (const auto& prop : properties) {
            auto name = prop["name"].get<std::string>();
            auto value = prop["value"].get<std::string>();
            if (name == "enabled") {
                (value == "false" || value == "0") ? comp->stop() : comp->start();
            } else {
                props.emplace_back(name, value);
            }
        }
        if (!props.empty()) {
            comp->set_properties(props, composite::properties::config_type::RUNTIME);
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
        res.set_header("Access-Control-Allow-Methods", "GET, POST, PATCH, OPTIONS"); // allowed HTTP methods
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
        res.set_content(app_json.dump(), "application/json");
        res.status = httplib::OK_200;
    });

    // GET components
    endpoint = std::format("/{}/{}", APP, COMPONENTS);
    server->Get(endpoint, [&app](const httplib::Request&, httplib::Response& res) {
        set_cors_header(res);
        auto comps_json = nlohmann::json(app.components());
        res.set_content(comps_json.dump(), "application/json");
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
                    for (const auto& prop : comp_json["properties"]) {
                        props.emplace_back(prop["name"], prop["value"].get<std::string>());
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

    // POST connections
    endpoint = std::format("/{}/{}", APP, CONNECTIONS);
    server->Post(endpoint, [&app](const httplib::Request& req, httplib::Response& res) {
        set_cors_header(res);
        // TODO: get POST info
        // TODO: add new connection
    });

    return server;
}

} // namespace composite
