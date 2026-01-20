/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "composite/core/application.hpp"
#include "composite/util/cpu_affinity.hpp"
#include "composite/version.hpp"
#include "helpers.hpp"
#include "property_changeset.hpp"

#ifdef COMPOSITE_USE_DPDK
#include "composite/dpdk/manager.hpp"
#endif

#ifdef COMPOSITE_USE_OPENTELEMETRY
#include "composite/telemetry/manager.hpp"
#endif

#include <argparse/argparse.hpp>
#include <atomic>
#include <csignal>
#include <dlfcn.h>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <future>
#include <httplib.h>
#include <iostream>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <unordered_set>
#include <vector>

namespace composite {

#ifdef COMPOSITE_USE_OPENSSL
auto make_server(
  application&,
  composite::component_handles_type&,
  const std::string&,
  const std::string&,
  const std::string&
) -> std::unique_ptr<httplib::Server>;
#else
auto make_server(application&, composite::component_handles_type&) -> std::unique_ptr<httplib::Server>;
#endif

} // namespace composite

auto main(int argc, char** argv) -> int {
    // ========================================
    // Argument Parsing
    // ========================================
    auto program = argparse::ArgumentParser{"composite-cli", VERSION};
    program.add_argument("config-file")
      .help("application configuration file");
    program.add_argument("-s", "--server")
      .help("REST server address")
      .default_value(std::string{"localhost"});
    program.add_argument("-p", "--port")
      .help("REST server port")
      .scan<'i', int>()
      .default_value(5000);
#ifdef COMPOSITE_USE_OPENSSL
    program.add_argument("-a", "--certificate-authority")
      .help("Path to a cert file for the certificate authority")
      .default_value(std::string{});
    program.add_argument("-c", "--client-certificate")
      .help("Path to a client certificate file for TLS")
      .required();
    program.add_argument("-k", "--client-key")
      .help("Path to a client key file for TLS")
      .required();
#endif
    program.add_argument("-l", "--log-level")
      .help("log level [trace, debug, info, warning, error, critical, off]")
      .default_value(std::string{"info"});
#ifdef COMPOSITE_USE_DPDK
    program.add_argument("--list-dpdk-ports")
      .help("list available DPDK ports and exit")
      .flag();
#endif

    try {
        program.parse_args(argc, argv);
    } catch (const std::runtime_error& err) {
        std::cerr << "Error parsing arguments: " << err.what() << "\n";
        std::cerr << program;
        return EXIT_FAILURE;
    }

    // ========================================
    // Signal Handler Setup
    // ========================================
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGINT);
    sigaddset(&sigset, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &sigset, nullptr) != 0) {
        std::cerr << "failed to block signals: " << strerror(errno) << std::endl;
        return EXIT_FAILURE;
    }

    // ========================================
    // Logging Configuration
    // ========================================
    auto level = spdlog::level::from_str(program.get<std::string>("--log-level"));
    spdlog::set_level(level);

    // ========================================
    // CPU Affinity Capture
    // ========================================
    // Get available CPUs from cgroups/container for affinity mapping
    std::vector<int> available_cores;
    if (auto cpuset_opt = composite::get_available_cpus()) {
        const auto& cpuset = *cpuset_opt;
        for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
            if (CPU_ISSET(cpu, &cpuset)) {
                available_cores.push_back(cpu);
            }
        }
        spdlog::info("Captured {} available CPU cores for component affinity", available_cores.size());
        spdlog::debug("Available physical CPU cores: [{}]",
            [&available_cores]() {
                std::string result;
                for (size_t i = 0; i < available_cores.size(); i++) {
                    if (i > 0) result += ", ";
                    result += std::to_string(available_cores[i]);
                }
                return result;
            }());
    } else {
        spdlog::warn("Failed to capture available CPUs, component cpu_affinity will not be available");
    }

    // ========================================
    // Configuration File Parsing
    // ========================================
    auto config_file = program.get<std::string>("config-file");

    // Validate config file exists
    if (!std::filesystem::exists(config_file)) {
        spdlog::error("config file not found: {}", config_file);
        return EXIT_FAILURE;
    }

    spdlog::info("Using config file at: {}", config_file);
    auto config_ifstream = std::ifstream{config_file};

    // Parse JSON with error handling
    nlohmann::json app_json;
    try {
        app_json = nlohmann::json::parse(config_ifstream);
    } catch (const nlohmann::json::parse_error& e) {
        spdlog::error("failed to parse config file: {}", e.what());
        return EXIT_FAILURE;
    }

#ifdef COMPOSITE_USE_DPDK
    bool dpdk_initialized = false;
    auto shutdown_dpdk = [&]() {
        if (!dpdk_initialized) {
            return;
        }
        spdlog::trace("shutting down DPDK");
        composite::dpdk::manager::instance().shutdown();
        dpdk_initialized = false;
    };

    // ========================================
    // DPDK Port Discovery (early exit if requested)
    // ========================================
    if (program.get<bool>("--list-dpdk-ports")) {
        // Initialize DPDK (best effort; uses config if present)
        auto dpdk_cfg = composite::dpdk::config{};
        if (app_json.contains("dpdk")) {
            dpdk_cfg = composite::parse_dpdk_config(app_json["dpdk"]);
        } else {
            spdlog::warn("DPDK config not found; attempting discovery with default EAL args");
        }
        if (!composite::dpdk::manager::instance().initialize(dpdk_cfg)) {
            spdlog::error("failed to initialize DPDK");
            return EXIT_FAILURE;
        }
        dpdk_initialized = true;

        // List ports
        spdlog::info("Available DPDK Ports");
        spdlog::info("====================");

        auto& mgr = composite::dpdk::manager::instance();
        auto ports = mgr.list_available_ports();
        if (ports.empty()) {
            spdlog::info("No ports detected");
        } else {
            for (const auto& port : ports) {
                spdlog::info(
                    std::format("Port {}: driver={}, max_rx_queues={}, max_tx_queues={}, socket={}",
                        port.port_id,
                        port.driver_name,
                        port.max_rx_queues,
                        port.max_tx_queues,
                        port.socket_id
                    )
                );
            }
        }

        // Cleanup and exit
        shutdown_dpdk();
        return EXIT_SUCCESS;
    }
#else
    auto shutdown_dpdk = [](){};
#endif

#ifdef COMPOSITE_USE_OPENSSL
    // ========================================
    // TLS Certificate Validation
    // ========================================
    auto ca_path = program.get<std::string>("--certificate-authority");
    auto cert_path = program.get<std::string>("--client-certificate");
    auto key_path = program.get<std::string>("--client-key");
    if (!std::filesystem::exists(cert_path)) {
        spdlog::error("client certificate file not found at {}", cert_path);
        return EXIT_FAILURE;
    }
    if (!std::filesystem::exists(key_path)) {
        spdlog::error("client key file not found at {}", key_path);
        return EXIT_FAILURE;
    }
    if (!ca_path.empty() && !std::filesystem::exists(ca_path)) {
        spdlog::error("certificate authority file not found at {}", ca_path);
        return EXIT_FAILURE;
    }
#endif

#ifdef COMPOSITE_USE_DPDK
    // ========================================
    // DPDK Initialization
    // ========================================
    std::vector<int> dpdk_logical_cores;
    if (app_json.contains("dpdk") && app_json["dpdk"].contains("enabled") && app_json["dpdk"]["enabled"].get<bool>()) {
        spdlog::debug("initializing DPDK");
        auto dpdk_cfg = composite::parse_dpdk_config(app_json["dpdk"]);

        // Translate DPDK EAL args from logical to physical cores
        if (!available_cores.empty()) {
            auto [translated_args, logical_cores] =
                composite::translate_dpdk_eal_args(dpdk_cfg.eal_args, available_cores);
            dpdk_cfg.eal_args = translated_args;
            dpdk_logical_cores = logical_cores;

            // Log the DPDK core allocation
            if (!dpdk_logical_cores.empty()) {
                std::string dpdk_cores_str;
                std::string physical_cores_str;
                for (size_t i = 0; i < dpdk_logical_cores.size(); i++) {
                    if (i > 0) {
                        dpdk_cores_str += ", ";
                        physical_cores_str += ", ";
                    }
                    int logical = dpdk_logical_cores[i];
                    dpdk_cores_str += std::to_string(logical);
                    physical_cores_str += std::to_string(available_cores[logical]);
                }
                spdlog::info("DPDK cores: logical [{}] -> physical [{}]",
                    dpdk_cores_str, physical_cores_str);
            }
        }

        if (!composite::dpdk::manager::instance().initialize(dpdk_cfg)) {
            spdlog::error("failed to initialize DPDK");
            return EXIT_FAILURE;
        }
        dpdk_initialized = true;

        // If no lcores were specified in config, query DPDK to see what it chose
        if (dpdk_logical_cores.empty() && !available_cores.empty()) {
            auto dpdk_physical_cores = composite::dpdk::manager::instance().get_dpdk_lcores();
            if (!dpdk_physical_cores.empty()) {
                // Map physical cores back to logical indices
                for (int physical_core : dpdk_physical_cores) {
                    auto it = std::find(available_cores.begin(), available_cores.end(), physical_core);
                    if (it != available_cores.end()) {
                        int logical_idx = std::distance(available_cores.begin(), it);
                        dpdk_logical_cores.push_back(logical_idx);
                    }
                }

                // Log what DPDK chose
                if (!dpdk_logical_cores.empty()) {
                    std::string dpdk_cores_str;
                    std::string physical_cores_str;
                    for (size_t i = 0; i < dpdk_logical_cores.size(); i++) {
                        if (i > 0) {
                            dpdk_cores_str += ", ";
                            physical_cores_str += ", ";
                        }
                        int logical = dpdk_logical_cores[i];
                        dpdk_cores_str += std::to_string(logical);
                        physical_cores_str += std::to_string(available_cores[logical]);
                    }
                    spdlog::info("DPDK auto-detected cores: logical [{}] -> physical [{}]",
                        dpdk_cores_str, physical_cores_str);
                }
            }
        }

        // Move main thread off DPDK cores to avoid interfering with polling
        if (!dpdk_logical_cores.empty() && !available_cores.empty()) {
            cpu_set_t non_dpdk_cpuset;
            CPU_ZERO(&non_dpdk_cpuset);

            // Build set of non-DPDK physical cores
            for (size_t i = 0; i < available_cores.size(); i++) {
                bool is_dpdk = std::find(dpdk_logical_cores.begin(),
                    dpdk_logical_cores.end(), static_cast<int>(i)) != dpdk_logical_cores.end();
                if (!is_dpdk) {
                    CPU_SET(available_cores[i], &non_dpdk_cpuset);
                }
            }

            if (CPU_COUNT(&non_dpdk_cpuset) > 0) {
                if (pthread_setaffinity_np(pthread_self(), sizeof(non_dpdk_cpuset), &non_dpdk_cpuset) == 0) {
                    spdlog::info("Moved main thread to {} non-DPDK cores", CPU_COUNT(&non_dpdk_cpuset));
                } else {
                    spdlog::warn("Failed to move main thread off DPDK cores: {}", strerror(errno));
                }
            } else {
                spdlog::warn("All cores assigned to DPDK, main thread cannot be moved off");
            }
        }

        spdlog::info("DPDK initialized with {} port(s)", dpdk_cfg.ports.size());
    }
#else
    std::vector<int> dpdk_logical_cores;  // Empty when DPDK not enabled
#endif

#ifdef COMPOSITE_USE_OPENTELEMETRY
    // ========================================
    // Telemetry Initialization (OpenTelemetry)
    // ========================================
    bool telemetry_initialized = false;
    auto shutdown_telemetry = [&]() {
        if (!telemetry_initialized) {
            return;
        }
        spdlog::trace("shutting down OpenTelemetry");
        composite::telemetry::manager::instance().shutdown();
        telemetry_initialized = false;
    };

    if (app_json.contains("telemetry") && app_json["telemetry"].value("enabled", false)) {
        spdlog::debug("initializing OpenTelemetry");
        auto telemetry_cfg = composite::parse_telemetry_config(app_json["telemetry"]);
        if (!composite::telemetry::manager::instance().initialize(telemetry_cfg)) {
            spdlog::error("failed to initialize OpenTelemetry");
            shutdown_dpdk();
            return EXIT_FAILURE;
        }
        telemetry_initialized = true;
    }
#else
    auto shutdown_telemetry = [](){};
#endif

    // ========================================
    // Application Initialization
    // ========================================
    auto comp_handles = composite::component_handles_type{};
    auto app_name = composite::generate_app_name();
    if (app_json.contains("name")) {
        app_name = app_json["name"].get<std::string>();
    }
    pthread_setname_np(pthread_self(), app_name.c_str());
    auto app = composite::application{app_name};

    // ========================================
    // Component Configuration Validation
    // ========================================
    // Validate components field exists
    if (!app_json.contains("components")) {
        spdlog::error("config file missing 'components' field");
        shutdown_dpdk();
        return EXIT_FAILURE;
    }
    if (!app_json["components"].is_array()) {
        spdlog::error("'components' field must be an array");
        shutdown_dpdk();
        return EXIT_FAILURE;
    }

    // Validate component definitions and check for duplicates
    auto component_ids = std::unordered_set<std::string>{};
    for (const auto& comp : app_json["components"]) {
        // Validate required fields
        if (!comp.contains("id")) {
            spdlog::error("component missing required 'id' field: {}", comp.dump());
            shutdown_dpdk();
            return EXIT_FAILURE;
        }
        if (!comp.contains("library")) {
            spdlog::error("component missing required 'library' field: {}", comp.dump());
            shutdown_dpdk();
            return EXIT_FAILURE;
        }

        // Check for duplicate IDs
        auto comp_id = comp["id"].get<std::string>();
        if (component_ids.contains(comp_id)) {
            spdlog::error("duplicate component id: '{}'", comp_id);
            shutdown_dpdk();
            return EXIT_FAILURE;
        }
        component_ids.insert(comp_id);
    }

    // ========================================
    // Component Loading and Configuration
    // ========================================
    // Error handler that cleans up application resources
    auto cleanup_and_exit = [&app, &shutdown_telemetry, &shutdown_dpdk](std::string_view msg) {
        spdlog::error("{}", msg);
        app.clear();
        shutdown_telemetry();
        shutdown_dpdk();
        return EXIT_FAILURE;
    };

    for (const auto& comp : app_json["components"]) {
        // Load component from library
        auto comp_ptr = composite::make_component(comp, comp_handles);
        if (comp_ptr == nullptr) {
            return cleanup_and_exit("failed to load component");
        }
        // Set log level
        comp_ptr->log_level(level);

        // Configure CPU affinity if specified
        if (!available_cores.empty() && comp.contains("cpu_affinity")) {
            auto affinity_str = comp["cpu_affinity"].get<std::string>();
            auto cpuset_opt = composite::parse_affinity_config(affinity_str, available_cores);
            if (cpuset_opt.has_value()) {
                comp_ptr->set_cpu_affinity(*cpuset_opt);
                spdlog::debug("Component '{}' cpu_affinity resolved cores: [{}]",
                    comp_ptr->id(),
                    [&cpuset_opt]() {
                        std::string result;
                        for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
                            if (CPU_ISSET(cpu, &(*cpuset_opt))) {
                                if (!result.empty()) {
                                    result += ", ";
                                }
                                result += std::to_string(cpu);
                            }
                        }
                        return result;
                    }());
                spdlog::debug("Component '{}' cpu_affinity configured: '{}'", comp_ptr->id(), affinity_str);
            } else if (affinity_str != "none" && !affinity_str.empty()) {
                return cleanup_and_exit(
                    std::format(
                        "Failed to parse cpu_affinity '{}' for component '{}'", affinity_str, comp_ptr->id()
                    )
                );
            }
        }

        // Set properties
        try {
            // Merge application-level properties with component-level properties
            auto props_json = nlohmann::json{};
            if (app_json.contains("properties")) {
                spdlog::trace("adding app-level properties to changeset for {}", comp_ptr->id());
                props_json.merge_patch(app_json["properties"]);
            }
            if (comp.contains("properties")) {
                spdlog::trace("adding component-level properties to changeset for {}", comp_ptr->id());
                props_json.merge_patch(comp["properties"]);
            }

            // Parse property changeset
            auto changeset = composite::property_changeset::from_json(props_json);

            // Apply changes if there are any
            if (changeset.has_updates()) {
                spdlog::trace("setting properties on component {}", comp_ptr->id());
                comp_ptr->set_properties(changeset.properties(), composite::properties::config_type::INITIALIZE, true);
                comp_ptr->property_change_handler();
            }
        } catch (const std::runtime_error& err) {
            return cleanup_and_exit(std::format("property error for component '{}': {}", comp_ptr->id(), err.what()));
        }
        // Add to application
        spdlog::trace("adding {} to application '{}'", comp_ptr->id(), app.name());
        app.add_component(comp_ptr);
    }

    // ========================================
    // Transport Registry Setup
    // ========================================
    auto transport_registry = composite::transport_registry{};
    if (app_json.contains("transports")) {
        spdlog::trace("parsing transport definitions");
        auto [registry, error] = composite::parse_transports(app_json["transports"]);
        if (!error.empty()) {
            return cleanup_and_exit(std::format("failed to parse transports: {}", error));
        }
        transport_registry = std::move(registry);
        spdlog::debug("parsed {} transport definition(s)", transport_registry.size());
    }

    // ========================================
    // Transport Attachment
    // ========================================
    for (const auto& comp : app_json["components"]) {
        if (!comp.contains("transports")) {
            continue;
        }

        auto comp_id = comp["id"].get<std::string>();
        auto comp_ptr = app.get_component(comp_id);
        if (comp_ptr == nullptr) {
            return cleanup_and_exit(std::format("component '{}' not found when attaching transports", comp_id));
        }

        spdlog::trace("attaching transports to component '{}'", comp_id);
        auto error = composite::attach_component_transports(comp_ptr, comp["transports"], transport_registry);
        if (!error.empty()) {
            return cleanup_and_exit(std::format("failed to attach transports to '{}': {}", comp_id, error));
        }
    }

    // ========================================
    // Component Connections
    // ========================================
    if (app_json.contains("connections")) {
        if (!app_json["connections"].is_array()) {
            return cleanup_and_exit("'connections' field must be an array");
        }

        for (const auto& conn : app_json["connections"]) {
            if (!conn.contains("output")) {
                return cleanup_and_exit(std::format("missing output for connection: {}", conn.dump()));
            }
            if (!conn.contains("input")) {
                return cleanup_and_exit(std::format("missing input for connection: {}", conn.dump()));
            }

            // Validate output section
            auto output = conn["output"];
            auto [output_comp, output_port, oerror] = composite::validate_connection(output);
            if (!oerror.empty()) {
                return cleanup_and_exit(std::format("invalid connection output: {}: {}", conn.dump(), oerror));
            }

            // Validate input section
            auto input = conn["input"];
            auto [input_comp, input_port, ierror] = composite::validate_connection(input);
            if (!ierror.empty()) {
                return cleanup_and_exit(std::format("invalid connection input: {}: {}", conn.dump(), ierror));
            }

            spdlog::trace("connecting {}:{} to {}:{}", output_comp, output_port, input_comp, input_port);

            // Get the output component
            auto output_comp_ptr = app.get_component(output_comp);
            if (output_comp_ptr == nullptr) {
                return cleanup_and_exit(std::format("output component '{}' not found", output_comp));
            }

            // Get the input component
            auto input_comp_ptr = app.get_component(input_comp);
            if (input_comp_ptr == nullptr) {
                return cleanup_and_exit(std::format("input component '{}' not found", input_comp));
            }

            // Connect the ports
            if (!output_comp_ptr->connect(output_port, input_comp_ptr, input_port)) {
                return cleanup_and_exit(std::format("failed to connect {}:{} to {}:{}", output_comp, output_port, input_comp, input_port));
            }
        }
    }

    // ========================================
    // REST Server Setup
    // ========================================
    auto server_addr = program.get<std::string>("--server");
    auto server_port = program.get<int>("--port");
#ifdef COMPOSITE_USE_OPENSSL
    auto server = composite::make_server(app, comp_handles, cert_path, key_path, ca_path);
#else
    auto server = composite::make_server(app, comp_handles);
#endif

    // ========================================
    // Signal Handler Thread
    // ========================================
    auto signal_future = std::async(std::launch::async, [sigset]() mutable -> int {
        auto signum = int{};
        if (auto rc = sigwait(&sigset, &signum); rc != 0) {
            spdlog::error("sigwait failed: {}", strerror(rc));
            return -1;
        }
        spdlog::trace("signal {} received, initiating shutdown...", signum);
        return signum;
    });

    // ========================================
    // Application Lifecycle
    // ========================================
    try {
        // Initialize the application
        spdlog::trace("initializing application '{}'", app.name());
        app.initialize();

        // Start REST server
        spdlog::trace("listening at {}:{}", server_addr, server_port);
        auto server_thread = std::jthread([&]() {
            server->listen(server_addr, server_port);
        });

        // Start the application
        spdlog::trace("starting application '{}'", app.name());
        app.start();

        // Wait for signal to stop
        spdlog::trace("waiting for signal...");
        signal_future.wait();

        // Stop server
        spdlog::trace("stopping server...");
        server->stop();

        // Stop the application
        spdlog::trace("stopping application '{}'", app.name());
        app.stop();

        // Clean up the application resources
        spdlog::trace("clearing application '{}'", app.name());
        app.clear();
    } catch (const std::exception& e) {
        spdlog::critical("unhandled exception in main: {}", e.what());
        shutdown_telemetry();
        shutdown_dpdk();
        return EXIT_FAILURE;
    }

    shutdown_telemetry();
    shutdown_dpdk();

    spdlog::trace("complete");
    spdlog::shutdown();

    return EXIT_SUCCESS;
}
