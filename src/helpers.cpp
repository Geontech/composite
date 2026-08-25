/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "helpers.hpp"
#include "composite/core/application.hpp"
#include "composite/core/logger.hpp"
#include "composite/core/register.hpp"
#include "composite/ports/input_port.hpp"
#include "composite/ports/output_port.hpp"
#include "composite/util/cpu_affinity.hpp"

#include <format>
#include <iostream>
#include <random>
#include <spdlog/spdlog.h>
#include <stdexcept>

namespace composite {

namespace {
auto close_func(void* p) -> void {
    if (p != nullptr) {
        dlclose(p);
    }
}
} // namespace

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

auto make_component(const nlohmann::json& comp_json) -> std::shared_ptr<composite::component> {
    // Validate field TYPES, not just presence: a non-string "library"/"id"
    // (e.g. `"library": 42`) would otherwise throw nlohmann::json::type_error out
    // of .get<std::string>(), which is not a std::runtime_error and historically
    // escaped the loader to std::terminate.
    if (!comp_json.contains("library") || !comp_json["library"].is_string()) {
        spdlog::error("component 'library' field is required and must be a string");
        return {};
    }
    if (!comp_json.contains("id") || !comp_json["id"].is_string()) {
        spdlog::error("component 'id' field is required and must be a string");
        return {};
    }
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
        spdlog::error("failed to open {}: {}", library, dlerror());
        return {};
    }
    dlerror(); // clear existing

    // Get component id (required)
    auto comp_id = comp_json["id"].get<std::string>();

    // ABI handshake: a component built with COMPOSITE_REGISTER_COMPONENT exports
    // composite_abi_version(). Refuse to call create() on a library that lacks it or
    // reports an incompatible ABI — both indicate a stale/foreign build whose create()
    // signature or component contract may not match this framework (calling create()
    // through a mismatched function pointer would be UB). This is the single, uniform
    // (id, args) ABI; the old per-arity create() probing is gone.
    using abi_fn = unsigned long (*)();
    auto abi_func = reinterpret_cast<abi_fn>(dlsym(comp_handle.get(), "composite_abi_version"));
    if (abi_func == nullptr) {
        spdlog::error("{}: missing 'composite_abi_version' symbol — rebuild the component with "
                      "COMPOSITE_REGISTER_COMPONENT (this framework expects ABI v{})",
                      library, composite::abi_version);
        return {};
    }
    if (auto v = (*abi_func)(); v != composite::abi_version) {
        spdlog::error("{}: component ABI version {} != framework ABI version {} — rebuild the component", library, v,
                      composite::abi_version);
        return {};
    }

    // Build construction args. Prefer the structured "args" object; accept the legacy
    // scalar "create_arg" string (mapped to {"type": ...}) so existing configs keep working.
    composite::create_args args;
    if (comp_json.contains("args") && comp_json["args"].is_object()) {
        args.values = comp_json["args"];
    } else if (comp_json.contains("create_arg") && comp_json["create_arg"].is_string()) {
        args.values = nlohmann::json{{"type", comp_json["create_arg"].get<std::string>()}};
    }

    std::shared_ptr<composite::component> inner;

    // create() may throw (a component throws on an unknown/unsupported type arg).
    // Catch it HERE, while comp_handle (the mapping) is still alive: otherwise the
    // unique_ptr destructs during unwinding and dlclose()s the library before the
    // exception reaches its handler — unmapping the in-flight exception's
    // type_info/vtable (which can live in the .so) → crash. Consuming it here also
    // turns a config typo into a clean load failure instead of std::terminate.
    try {
        using function_ptr = std::shared_ptr<composite::component> (*)(std::string_view, const composite::create_args&);
        auto create_func = reinterpret_cast<function_ptr>(dlsym(comp_handle.get(), "create"));
        if (auto err = dlerror(); err != nullptr) {
            spdlog::error("failed to find the 'create' symbol from {}: {}", library, err);
            return {};
        }
        inner = (*create_func)(comp_id, args);
    } catch (const std::exception& e) {
        spdlog::error("create() for '{}' from {} threw: {}", comp_id, library, e.what());
        return {};
    } catch (...) {
        spdlog::error("create() for '{}' from {} threw a non-std exception", comp_id, library);
        return {};
    }

    if (inner == nullptr) {
        spdlog::error("failed to create component '{}' from library {}", comp_id, library);
        return {};
    }
    // The component must honor the configured id. Some libraries hardcode it
    // (e.g. a 0-arg create()), which would otherwise silently shadow another
    // instance (add_component rejects the duplicate) and leave connections/REST
    // addressing a component that isn't in the graph.
    if (inner->id() != comp_id) {
        spdlog::error("library {} returned component id '{}', expected '{}' — create() must use the provided id",
                      library, inner->id(), comp_id);
        return {};
    }

    // Tie the dlopen handle to the component's lifetime. The returned shared_ptr's
    // control block is owned by THIS binary (not the .so), and its deleter destroys
    // the component FIRST (inner.reset(), explicit so it runs before the captured
    // handle), then lets the captured handle dlclose() — so the library is unmapped
    // only after ~component (whose code/vtable live in that mapping) has run.
    auto* raw = inner.get();
    spdlog::trace("component {} created", comp_id);
    return std::shared_ptr<composite::component>(
        raw, [inner = std::move(inner), h = std::move(comp_handle)](composite::component*) mutable {
            inner.reset(); // ~component runs here, while h (the mapping) is still alive
            // h destructs at the end of this lambda → dlclose() after the component is gone
        });
}

auto validate_component_connection(const nlohmann::json& conn) -> std::tuple<std::string, std::string, std::string> {
    if (!conn.contains("port")) {
        return {"", "", "missing 'port' field for component connection"};
    }
    auto component = conn["component"].get<std::string>();
    auto port = conn["port"].get<std::string>();
    return {component, port, {}};
}

auto setup_component(composite::component& comp, const nlohmann::json& comp_json) -> void {
    // Process-wide component log level. Component loggers are unregistered spdlog loggers, so
    // spdlog::set_level cannot reach them — every creation path applies the level explicitly.
    comp.log_level(global_log_level());

    if (comp_json.contains("cpu_affinity")) {
        if (!comp_json["cpu_affinity"].is_string()) {
            throw std::runtime_error(
                std::format("component '{}': 'cpu_affinity' must be a string (e.g. \"0-3\")", comp.id()));
        }
        const auto affinity_str = comp_json["cpu_affinity"].get<std::string>();
        const auto& cores = process_available_cores();
        if (cores.empty()) {
            spdlog::warn("component '{}': cpu_affinity '{}' ignored (available CPUs could not be captured)",
                         comp.id(), affinity_str);
            return;
        }
        const auto cpuset_opt = parse_affinity_config(affinity_str, cores);
        if (cpuset_opt.has_value()) {
            comp.set_cpu_affinity(*cpuset_opt);
            spdlog::debug("component '{}' cpu_affinity configured: '{}'", comp.id(), affinity_str);
        } else if (affinity_str != "none" && !affinity_str.empty()) {
            throw std::runtime_error(
                std::format("failed to parse cpu_affinity '{}' for component '{}'", affinity_str, comp.id()));
        }
    }
}

auto validate_connection(const nlohmann::json& conn) -> std::tuple<std::string, std::string, std::string> {
    if (conn.contains("component")) {
        return validate_component_connection(conn);
    }
    return {{}, {}, "missing connection type"};
}

auto parse_dpdk_config(const nlohmann::json& dpdk_json) -> dpdk::config {
    dpdk::config config;

    // Parse EAL arguments
    if (dpdk_json.contains("eal_args") && dpdk_json["eal_args"].is_array()) {
        config.eal_args = dpdk_json["eal_args"].get<std::vector<std::string>>();
        // Reject --lcores HERE, unconditionally — not only in the logical->physical translator,
        // which runs only when CPU discovery produced a core list. Left to the translator alone,
        // a failed discovery let --lcores reach EAL untouched: exactly the silent mixing of
        // physical ids with the logical ids -l users write that the rejection exists to prevent.
        // Both spellings ("--lcores <map>" and "--lcores=<map>") are refused.
        for (const auto& arg : config.eal_args) {
            if (arg == "--lcores" || arg.starts_with("--lcores=")) {
                throw std::invalid_argument("DPDK --lcores is not supported: its core ids would bypass the "
                                            "logical->physical translation applied to -l. Express the core "
                                            "list with -l instead.");
            }
        }
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
        config.export_interval = std::chrono::milliseconds{telemetry_json["export_interval"].get<uint64_t>()};
    }

    // Parse exporter configuration
    if (telemetry_json.contains("exporter") && telemetry_json["exporter"].is_object()) {
        const auto& exporter_json = telemetry_json["exporter"];

        config.exporter.endpoint = exporter_json.value("endpoint", "http://localhost:4318");
        config.exporter.protocol = exporter_json.value("protocol", "http/protobuf");

        if (exporter_json.contains("timeout")) {
            config.exporter.timeout = std::chrono::milliseconds{exporter_json["timeout"].get<uint64_t>()};
        }

        config.exporter.headers = exporter_json.value("headers", "");
    }

    return config;
}
#endif

} // namespace composite
