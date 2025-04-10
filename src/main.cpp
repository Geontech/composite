/*
 * Copyright (C) 2024 Geon Technologies, LLC
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
 * along with this program.  If not, see http://www.gnu.org/licenses/.
 */
 
#include "composite/application.hpp"
#include "composite/version.hpp"
#include "helpers.hpp"

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
    // Create argument parser with options
    auto program = argparse::ArgumentParser{"composite-cli", VERSION};
    program.add_argument("-f", "--config-file")
      .help("application configuration file")
      .required();
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

    // Parse arguments
    try {
        program.parse_args(argc, argv);
    } catch (const std::runtime_error& err) {
        std::cerr << "Error parsing arguments: " << err.what() << "\n";
        std::cerr << program;
        return EXIT_FAILURE;
    }

    // Setup signal handlers
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGINT);
    sigaddset(&sigset, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &sigset, nullptr) != 0) {
        std::cerr << "failed to block signals: " << strerror(errno) << std::endl;
        return EXIT_FAILURE;
    }

    // Setup logging
    auto level = spdlog::level::from_str(program.get<std::string>("--log-level"));
    spdlog::set_level(level);

#ifdef COMPOSITE_USE_OPENSSL
    // Check certificate and key files exist
    auto ca_path = program.get<std::string>("--certificate-authority");
    auto cert_path = program.get<std::string>("--client-certificate");
    auto key_path = program.get<std::string>("--client-key");
    if (!std::filesystem::exists(cert_path)) {
        spdlog::error("client certificate file not found at {}", cert_path);
        return EXIT_FAILURE;
    }
    if (!std::filesystem::exists(cert_path)) {
        spdlog::error("client key file not found at {}", cert_path);
        return EXIT_FAILURE;
    }
    if (!ca_path.empty() && !std::filesystem::exists(ca_path)) {
        spdlog::error("certificate authority file not found at {}", ca_path);
        return EXIT_FAILURE;
    }
#endif

    // Get configuration file, then read and parse
    auto config_file = program.get<std::string>("--config-file");
    spdlog::info("Using config file at: {}", config_file);
    auto config_ifstream = std::ifstream{config_file};
    auto app_json = nlohmann::json::parse(config_ifstream);

    // Component handle holders    
    auto comp_handles = composite::component_handles_type{};

    // Create a new application object
    auto app_name = composite::generate_app_name();
    if (app_json.contains("name")) {
        app_name = app_json["name"].get<std::string>();
    }
    pthread_setname_np(pthread_self(), app_name.c_str());
    auto app = composite::application{app_name};

    // Get components and load them
    for (const auto& comp : app_json["components"]) {
        // Add component to application
        auto comp_ptr = composite::make_component(comp, comp_handles);
        if (comp_ptr == nullptr) {
            return EXIT_FAILURE;
        }
        // Set log level
        comp_ptr->log_level(level);
        // Set properties
        try {
            // Set application-level properties
            spdlog::trace("adding app-level properties to changeset for {}", comp_ptr->id());
            auto props = std::vector<std::pair<std::string,std::string>>{};
            for (const auto& prop : app_json["properties"]) {
                props.emplace_back(prop["name"], prop["value"].get<std::string>());
            }
            // Set component-level properties
            spdlog::trace("adding component-level properties to changeset for {}", comp_ptr->id());
            for (const auto& prop : comp["properties"]) {
                props.emplace_back(prop["name"], prop["value"].get<std::string>());
            }
            if (!props.empty()) {
                spdlog::trace("setting properties on component {}", comp_ptr->id());
                comp_ptr->set_properties(props, composite::properties::config_type::INITIALIZE, true);
            }
        } catch (const std::runtime_error& err) {
            spdlog::error(err.what());
            return EXIT_FAILURE;
        }
        // Add to application
        spdlog::trace("adding {} to application '{}'", comp_ptr->id(), app.name());
        app.add_component(comp_ptr);
    }

    // Make connections
    auto conn_exit = [&app](std::string_view msg) {
        spdlog::error(msg);
        app.clear();
        return EXIT_FAILURE;
    };
    for (const auto& conn : app_json["connections"]) {
        if (!conn.contains("output")) {
            return conn_exit(std::format("missing output for connection: {}", conn.dump()));
        }
        if (!conn.contains("input")) {
            return conn_exit(std::format("missing output for connection: {}", conn.dump()));
        }
        // Validate output section
        auto output = conn["output"];
        auto [output_comp, output_port, oerror] = composite::validate_connection(output);
        if (!oerror.empty()) {
            return conn_exit(std::format("invalid connection output: {}: {}", conn.dump(), oerror));
        }
        // Validate input section
        auto input = conn["input"];
        auto [input_comp, input_port, ierror] = composite::validate_connection(input);
        if (!ierror.empty()) {
            return conn_exit(std::format("invalid connection input: {}: {}", conn.dump(), ierror));
        }
        
        spdlog::trace("connecting {}:{} to {}:{}", output_comp, output_port, input_comp, input_port);
        if (input_comp.starts_with("nats://")) {
#ifndef COMPOSITE_USE_NATS
            return conn_exit(std::format("NATS support is not enabled: required for connection: {}", conn.dump()));
#endif
            // Get the output component port
            auto output_comp_ptr = app.get_component(output_comp);
            if (output_comp_ptr == nullptr) {
                return conn_exit(std::format("output component {} null during connection: {}", output_comp, conn.dump()));
            }
#ifdef COMPOSITE_USE_NATS
            if (!output_comp_ptr->connect(output_port, input_comp, input_port)) {
                return conn_exit(std::format("Failed to connect {}:{} to {}", output_comp, output_port, input_comp));
            }
#endif
        } else if (output_comp.starts_with("nats://")) {
            // Future release will enable this support
            return conn_exit(std::format("NATS support is not enabled: required for connection: {}", conn.dump()));
        } else {
            // Get the output component port
            auto output_comp_ptr = app.get_component(output_comp);
            if (output_comp_ptr == nullptr) {
                return conn_exit(std::format("output component {} null during connection: {}", output_comp, conn.dump()));
            }
            // Get the input component port
            auto input_comp_ptr = app.get_component(input_comp);
            if (input_comp_ptr == nullptr) {
                return conn_exit(std::format("input component {} null during connection: {}", input_comp, conn.dump()));
            }
            spdlog::trace("connecting {}:{} to {}:{}", output_comp, output_port, input_comp, input_port);
            if (!output_comp_ptr->connect(output_port, input_comp_ptr, input_port)) {
                return conn_exit(std::format("Failed to connect {}:{} to {}:{}", output_comp, output_port, input_comp, input_port));
            }
        }
    }

    // Create REST server for c&c
    auto server_addr = program.get<std::string>("--server");
    auto server_port = program.get<int>("--port");
#ifdef COMPOSITE_USE_OPENSSL
    auto server = composite::make_server(app, comp_handles, cert_path, key_path, ca_path);
#else
    auto server = composite::make_server(app, comp_handles);
#endif

    // Create the signal handling thread
    auto signal_future = std::async(std::launch::async, [sigset]() mutable -> int {
        auto signum = int{};
        if (auto rc = sigwait(&sigset, &signum); rc != 0) {
            spdlog::error("sigwait failed: {}", strerror(rc));
            return -1;
        }
        spdlog::trace("signal {} received, initiating shutdown...", signum);
        return signum;
    });

    try {
        // Initialize the application
        spdlog::trace("initializing application '{}'", app.name());
        app.initialize();

        // Start the application
        spdlog::trace("starting application '{}'", app.name());
        app.start();

        // Start REST server
        spdlog::trace("listening at {}:{}", server_addr, server_port);
        auto server_thread = std::jthread([&]() {
            server->listen(server_addr, server_port);
        });

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
        return EXIT_FAILURE;
    }

    spdlog::trace("complete");
    spdlog::shutdown();

    return EXIT_SUCCESS;
}
