#include "composite/application.hpp"

#include <cstdlib>
#include <httplib.h>
#include <memory>
#include <nlohmann/json.hpp>

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
        const auto& [type, any] = value;
        auto prop_obj = nlohmann::json::object();
        prop_obj["name"] = name;
        prop_obj["type"] = type;
        props_obj.push_back(prop_obj);
    }
    json_obj["properties"] = props_obj;
}

auto to_json(nlohmann::json& json_obj, const std::vector<std::shared_ptr<component>>& comps) {
    for (const auto& comp : comps) {
        if (comp != nullptr) {
            json_obj.push_back(*comp.get());
        }
    }
}

auto to_json(nlohmann::json& json_obj, const application& app) {
    json_obj["name"] = app.name();
    auto components = app.components();
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

auto make_server(const application& app) -> std::unique_ptr<httplib::Server> {
    auto server = std::make_unique<httplib::Server>();
    
    // application get
    server->Get("/app", [&app](const httplib::Request& req, httplib::Response& res) {
        auto app_json = nlohmann::json(app);
        res.set_content(app_json.dump(), "application/json");
    });

    // get component by ID
    server->Get("/app/components/:id", [&app](const httplib::Request& req, httplib::Response& res) {
        auto comp_id = req.path_params.at("id");
        if (auto comp = app.get_component(comp_id); comp != nullptr) {
            auto comp_json = nlohmann::json(*comp);
            res.set_content(comp_json.dump(), "application/json");
        }
    });

    return server;
}

} // namespace composite