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
#include "composite/ports/input_port.hpp"
#include "composite/ports/output_port.hpp"
#include "helpers.hpp"

#ifdef COMPOSITE_USE_NATS
#include "composite/transports/nats/transport.hpp"
#endif

#include <format>
#include <iostream>
#include <random>
#include <spdlog/spdlog.h>

namespace composite {

auto close_func(void* p) -> void {
    dlclose(p);
};

auto generate_app_name() -> std::string {
    auto app_name = std::string{"composite-"};
    auto characters = std::string{"abcdefghijklmnopqrstuvwxyz0123456789"};
    auto rd = std::random_device{};
    auto generator = std::mt19937{rd()};
    auto distribution = std::uniform_int_distribution<std::size_t>{0, characters.size() - 1};
    for (auto i = 0; i < 8; ++i) {
        app_name += characters.at(distribution(generator));
    }
    return app_name;
}

auto make_component(const nlohmann::json& comp_json, component_handles_type& handles) -> std::shared_ptr<composite::component> {
    // Get component name
    auto name = comp_json["name"].get<std::string>();
    // Open component module
    auto comp_str = std::format("lib{}.so", name);
    spdlog::trace("component module: {}", comp_str);
    // Get component module handle
    auto comp_handle = std::unique_ptr<void, decltype(&close_func)>(dlopen(comp_str.c_str(), RTLD_NOW), close_func);
    if (!comp_handle) {
        std::cerr << std::format("failed to open {}: {}\n", comp_str, dlerror());
        return {};
    }
    dlerror(); // clear existing
    // Component shared_ptr
    auto comp_ptr = std::shared_ptr<composite::component>{nullptr};
    // Get the create function
    if (comp_json.contains("create_arg")) {
        // Get create arg if present
        auto create_arg = comp_json["create_arg"].get<std::string>();
        // Create function to include string_view argument
        using function_ptr = std::shared_ptr<composite::component> (*)(std::string_view);
        auto create_func = reinterpret_cast<function_ptr>(dlsym(comp_handle.get(), "create"));
        if (auto err = dlerror(); err != nullptr) {
            std::cerr << std::format("failed to find the 'create' symbol from {}: {}\n", comp_str, err);
            return {};
        }
        dlerror(); // clear existing
        // Create a new component
        comp_ptr = (*create_func)(create_arg);
    } else {
        // Empty create function
        using function_ptr = std::shared_ptr<composite::component> (*)();
        auto create_func = reinterpret_cast<function_ptr>(dlsym(comp_handle.get(), "create"));
        if (auto err = dlerror(); err != nullptr) {
            std::cerr << std::format("failed to find the 'create' symbol from {}: {}\n", comp_str, err);
            return {};
        }
        dlerror(); // clear existing
        // Create a new component
        comp_ptr = (*create_func)();
    }
    if (comp_ptr == nullptr) {
        spdlog::error("failed to create component {}", name);
        return comp_ptr;
    }
    // Set id if needed
    if (comp_json.contains("id")) {
        comp_ptr->id(comp_json["id"].get<std::string>());
    } else {
        comp_ptr->id(name);
    }
    // Store handle for closing later
    handles.emplace_back(std::move(comp_handle));
    spdlog::trace("component {} created", comp_ptr->id());
    return comp_ptr;
}

auto validate_component_connection(const nlohmann::json& conn) -> std::tuple<std::string, std::string, std::string> {
    if (!conn.contains("port")) {
        return {"", "", "missing 'port' field for component connection"};
    }
    auto component = conn["component"].get<std::string>();
    auto port = conn["port"].get<std::string>();
    return {component, port, {}};
}

auto validate_nats_connection(const nlohmann::json& conn) -> std::tuple<std::string, std::string, std::string> {
#ifndef COMPOSITE_USE_NATS
    return {"", "", "NATS support is not enabled"};
#endif
    if (!conn.contains("subject")) {
        return {{}, {}, "missing 'subject' field for NATS connection"};
    }
    auto url = conn["nats"].get<std::string>();
    auto subject = conn["subject"].get<std::string>();
    return {url, subject, {}};
}

auto validate_connection(const nlohmann::json& conn) -> std::tuple<std::string, std::string, std::string> {
    if (conn.contains("component")) {
        return validate_component_connection(conn);
    } else if (conn.contains("nats")) {
        return validate_nats_connection(conn);
    }
    return {{}, {}, "missing connection type"};
}

auto build_props_lists(const nlohmann::json& properties)
  -> std::tuple<
       std::vector<std::pair<std::string, std::string>>,
       std::vector<std::pair<std::string, std::vector<std::string>>>,
       std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>
     > {
    auto props = std::vector<std::pair<std::string, std::string>>{};
    auto list_props = std::vector<std::pair<std::string, std::vector<std::string>>>{};
    auto struct_props = std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>{};
    auto to_string_pair = [](const nlohmann::json& value) -> std::pair<bool, std::string> {
        if (value.is_null()) {
            return {true, {}};
        }
        if (value.is_string()) {
            auto str = value.get<std::string>();
            if (str.empty()) {
                return {true, {}};
            }
            return {false, str};
        }
        return {false, value.dump()};
    };

    std::function<void(const nlohmann::json&, const std::string&, std::vector<std::pair<std::string, std::string>>&)>
    flatten_struct = [&](const nlohmann::json& object,
                         const std::string& prefix,
                         std::vector<std::pair<std::string, std::string>>& out) {
        for (const auto& [child_key, child_value] : object.items()) {
            if (child_key == "enabled") {
                continue;
            }
            auto dotted = prefix.empty() ? std::string{child_key} : std::format("{}.{}", prefix, child_key);
            if (child_value.is_object()) {
                flatten_struct(child_value, dotted, out);
            } else {
                const auto& [is_null, str_val] = to_string_pair(child_value);
                if (is_null) {
                    out.emplace_back(dotted, composite::properties::null_prop);
                } else {
                    out.emplace_back(dotted, str_val);
                }
            }
        }
    };

    for (const auto& [key, value] : properties.items()) {
        if (key == "enabled") {
            continue;
        } else if (value.is_array()) {
            std::vector<std::string> obj_scalar_props;
            for (const auto& [k, v] : value.items()) {
                if (v.is_object()) {
                    std::vector<std::pair<std::string, std::string>> obj_props;
                    flatten_struct(v, "", obj_props);
                    props.emplace_back(key, composite::properties::null_prop);
                    struct_props.emplace_back(std::format("{}[]", key), obj_props);
                } else {
                    const auto& [is_null, str_val] = to_string_pair(v);
                    obj_scalar_props.emplace_back(
                        is_null ? std::string{composite::properties::null_prop} : str_val
                    );
                }
            }
            if (!obj_scalar_props.empty()) {
                list_props.emplace_back(key, obj_scalar_props);
            }
        } else if (value.is_object()) {
            std::vector<std::pair<std::string, std::string>> obj_props;
            flatten_struct(value, "", obj_props);
            struct_props.emplace_back(key, obj_props);
        } else {
            if (value.is_null() || value.empty()) {
                props.emplace_back(key, composite::properties::null_prop);
            } else {
                props.emplace_back(key, value.get<std::string>());
            }
        }
    }
    return {props, list_props, struct_props};
}

auto parse_transports(const nlohmann::json& transports_json)
  -> std::tuple<transport_registry, std::string> {
    transport_registry registry;

    if (!transports_json.is_array()) {
        return {std::move(registry), "transports must be an array"};
    }

    for (const auto& transport_json : transports_json) {
        // Validate required fields
        if (!transport_json.contains("id")) {
            return {std::move(registry), "transport missing 'id' field"};
        }
        if (!transport_json.contains("type")) {
            return {std::move(registry), std::format("transport '{}' missing 'type' field",
                transport_json["id"].get<std::string>())};
        }

        auto id = transport_json["id"].get<std::string>();
        auto type_str = transport_json["type"].get<std::string>();

        // Convert string to transport_type enum
        auto type_opt = from_string(type_str);
        if (!type_opt.has_value()) {
            return {std::move(registry), std::format("unknown transport type '{}' for transport '{}'", type_str, id)};
        }

        // Check for duplicate IDs
        if (registry.contains(id)) {
            return {std::move(registry), std::format("duplicate transport id: '{}'", id)};
        }

        // Store transport definition
        transport_definition def;
        def.id = id;
        def.type = *type_opt;
        def.config = transport_json;  // Store full JSON for later instantiation

        registry[id] = def;
    }

    return {std::move(registry), ""};
}

auto create_transport(const transport_definition& def)
  -> std::tuple<std::unique_ptr<transport_base>, std::string> {
    if (def.type == transport_type::nats) {
#ifndef COMPOSITE_USE_NATS
        return {nullptr, std::format("NATS support not enabled for transport '{}'", def.id)};
#else
        // Validate NATS-specific fields
        if (!def.config.contains("url")) {
            return {nullptr, std::format("NATS transport '{}' missing 'url' field", def.id)};
        }
        if (!def.config.contains("subject")) {
            return {nullptr, std::format("NATS transport '{}' missing 'subject' field", def.id)};
        }

        auto url = def.config["url"].get<std::string>();
        auto subject = def.config["subject"].get<std::string>();

        try {
            auto transport = std::make_unique<nats::transport>(url, subject);
            return {std::move(transport), ""};
        } catch (const std::exception& e) {
            return {nullptr, std::format("failed to create NATS transport '{}': {}", def.id, e.what())};
        }
#endif
    }

    return {nullptr, std::format("unknown transport type '{}' for transport '{}'", to_string(def.type), def.id)};
}

auto attach_component_transports(
    std::shared_ptr<component> comp,
    const nlohmann::json& transports_json,
    const transport_registry& registry
) -> std::string {

    if (!transports_json.is_object()) {
        return std::format("component '{}' transports must be an object", comp->id());
    }

    // Iterate over port_name -> [transport_ids] mappings
    for (const auto& [port_name, transport_ids] : transports_json.items()) {
        if (!transport_ids.is_array()) {
            return std::format("component '{}' port '{}' transports must be an array",
                comp->id(), port_name);
        }

        // Look up the port (will check both inputs and outputs)
        auto* output_port_ptr = comp->get_port<output_port_base>(port_name);

        if (output_port_ptr != nullptr) {
            // It's an output port - attach transports
            for (const auto& transport_id_json : transport_ids) {
                if (!transport_id_json.is_string()) {
                    return std::format("component '{}' port '{}' transport ID must be a string",
                        comp->id(), port_name);
                }

                auto transport_id = transport_id_json.get<std::string>();

                // Look up transport definition in registry
                auto it = registry.find(transport_id);
                if (it == registry.end()) {
                    return std::format("component '{}' port '{}' references unknown transport '{}'",
                        comp->id(), port_name, transport_id);
                }

                // Create a new transport instance from the definition
                auto [transport, error] = create_transport(it->second);
                if (!error.empty()) {
                    return std::format("component '{}' port '{}': {}", comp->id(), port_name, error);
                }

                // Attach transport to the output port
                output_port_ptr->add_transport(std::move(transport));

                spdlog::debug("Attached transport '{}' to component '{}' port '{}'",
                    transport_id, comp->id(), port_name);
            }
        } else {
            // Check if it's an input port
            auto* input_port = comp->get_port<input_port_base>(port_name);
            if (input_port != nullptr) {
                // Input transports not yet implemented
                return std::format("component '{}' port '{}' is an input port - input transports not yet supported",
                    comp->id(), port_name);
            } else {
                return std::format("component '{}' has no port named '{}'",
                    comp->id(), port_name);
            }
        }
    }

    return "";
}

} // namespace composite
