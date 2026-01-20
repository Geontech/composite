/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
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
    // Get component library path/name
    auto library = comp_json["library"].get<std::string>();

    // Transform library name if it doesn't end with .so
    // If just a component name like "udp_source", convert to "libudp_source.so"
    if (!library.ends_with(".so")) {
        library = std::format("lib{}.so", library);
    }

    spdlog::trace("loading component library: {}", library);

    // Get component module handle
    auto comp_handle = std::unique_ptr<void, decltype(&close_func)>(dlopen(library.c_str(), RTLD_NOW), close_func);
    if (!comp_handle) {
        std::cerr << std::format("failed to open {}: {}\n", library, dlerror());
        return {};
    }
    dlerror(); // clear existing

    // Get component id (required)
    auto comp_id = comp_json["id"].get<std::string>();

    // Component shared_ptr
    auto comp_ptr = std::shared_ptr<composite::component>{nullptr};

    // Get the create function and call with id
    if (comp_json.contains("create_arg")) {
        // Get create arg if present
        auto create_arg = comp_json["create_arg"].get<std::string>();
        // Create function with id and arg parameters
        using function_ptr = std::shared_ptr<composite::component> (*)(std::string_view, std::string_view);
        auto create_func = reinterpret_cast<function_ptr>(dlsym(comp_handle.get(), "create"));
        if (auto err = dlerror(); err != nullptr) {
            std::cerr << std::format("failed to find the 'create' symbol from {}: {}\n", library, err);
            return {};
        }
        dlerror(); // clear existing
        // Create a new component with id and arg
        comp_ptr = (*create_func)(comp_id, create_arg);
    } else {
        // Create function with only id parameter
        using function_ptr = std::shared_ptr<composite::component> (*)(std::string_view);
        auto create_func = reinterpret_cast<function_ptr>(dlsym(comp_handle.get(), "create"));
        if (auto err = dlerror(); err != nullptr) {
            std::cerr << std::format("failed to find the 'create' symbol from {}: {}\n", library, err);
            return {};
        }
        dlerror(); // clear existing
        // Create a new component with id
        comp_ptr = (*create_func)(comp_id);
    }
    if (comp_ptr == nullptr) {
        spdlog::error("failed to create component '{}' from library {}", comp_id, library);
        return comp_ptr;
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

auto parse_dpdk_config(const nlohmann::json& dpdk_json) -> dpdk::config {
    dpdk::config config;

    // Parse EAL arguments
    if (dpdk_json.contains("eal_args") && dpdk_json["eal_args"].is_array()) {
        config.eal_args = dpdk_json["eal_args"].get<std::vector<std::string>>();
    }

    // Parse port configurations
    if (dpdk_json.contains("ports") && dpdk_json["ports"].is_array()) {
        for (const auto& port_json : dpdk_json["ports"]) {
            dpdk::port_config port;

            port.port_id = port_json.value("port_id", 0);
            port.interface = port_json.value("interface", "");
            port.rx_queues = port_json.value("rx_queues", 1);
            port.tx_queues = port_json.value("tx_queues", 1);
            port.rx_descriptors = port_json.value("rx_descriptors", 1024);
            port.tx_descriptors = port_json.value("tx_descriptors", 1024);
            port.mempool_name = port_json.value("mempool_name", "mbuf_pool");
            port.mempool_size = port_json.value("mempool_size", 8192);
            port.mempool_cache_size = port_json.value("mempool_cache_size", 256);
            port.mbuf_data_room_size = port_json.value("mbuf_data_room_size", 2048);

            config.ports.push_back(port);
        }
    }

    return config;
}

#ifdef COMPOSITE_USE_OPENTELEMETRY
auto parse_telemetry_config(const nlohmann::json& telemetry_json) -> telemetry::config {
    telemetry::config config;

    // Parse basic fields
    config.enabled = telemetry_json.value("enabled", false);
    config.service_name = telemetry_json.value("service_name", "composite");
    config.service_version = telemetry_json.value("service_version", "");

    // Parse export interval
    if (telemetry_json.contains("export_interval")) {
        config.export_interval = std::chrono::milliseconds{
            telemetry_json["export_interval"].get<uint64_t>()
        };
    }

    // Parse exporter configuration
    if (telemetry_json.contains("exporter") && telemetry_json["exporter"].is_object()) {
        const auto& exporter_json = telemetry_json["exporter"];

        config.exporter.endpoint = exporter_json.value("endpoint", "http://localhost:4318");
        config.exporter.protocol = exporter_json.value("protocol", "http/protobuf");

        if (exporter_json.contains("timeout")) {
            config.exporter.timeout = std::chrono::milliseconds{
                exporter_json["timeout"].get<uint64_t>()
            };
        }

        config.exporter.headers = exporter_json.value("headers", "");
    }

    return config;
}
#endif

} // namespace composite
