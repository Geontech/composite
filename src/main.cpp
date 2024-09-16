#include "composite/application.hpp"
#include "composite/version.hpp"

#include <argparse/argparse.hpp>
#include <atomic>
#include <csignal>
#include <dlfcn.h>
#include <fmt/core.h>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <vector>

auto set_property(std::shared_ptr<composite::component> comp, const nlohmann::json& prop) {
    auto type = prop["type"].get<std::string>();
    auto name = prop["name"].get<std::string>();
    auto value = prop["value"];
    if (type == "bool") {
        comp->set_property(name, value.get<bool>());
    } else if (type == "string") {
        comp->set_property(name, value.get<std::string>());
    } else if (type == "int32") {
        comp->set_property(name, value.get<int32_t>());
    } else if (type == "uint32") {
        comp->set_property(name, value.get<uint32_t>());
    } else if (type == "int64") {
        comp->set_property(name, value.get<int64_t>());
    } else if (type == "uint64") {
        comp->set_property(name, value.get<uint64_t>());
    } else if (type == "float") {
        comp->set_property(name, value.get<float>());
    }  else if (type == "double") {
        comp->set_property(name, value.get<double>());
    }
}

auto main(int argc, char** argv) -> int {
    // Create argument parser with options
    auto program = argparse::ArgumentParser{"composite-cli", VERSION};
    program.add_argument("-c", "--config")
        .help("application configuration file")
        .required();
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

    // Setup logging
    auto level = program.get<std::string>("--log-level");
    spdlog::set_level(spdlog::level::from_str(level));

    // Get configuration file, then read and parse
    auto config_file = program.get<std::string>("--config");
    spdlog::info("Using config file at: {}", config_file);
    auto config_ifstream = std::ifstream{config_file};
    auto app_json = nlohmann::json::parse(config_ifstream);

    // Component handle holders
    auto close_func = [](void* p) {
        dlclose(p);
    };
    auto comp_handles = std::vector<std::unique_ptr<void, decltype(close_func)>>{};

    // Create a new application object
    auto app_name = app_json["name"].get<std::string>();
    auto app = composite::application{app_name};

    // Get components and load them
    for (const auto& comp : app_json["components"]) {
        // Get component name
        auto name = comp["name"].get<std::string>();
        // Open component module
        auto comp_str = fmt::format("lib{}.so", name);
        spdlog::trace("component module: {}", comp_str);
        // Get component module handle
        auto comp_handle = std::unique_ptr<void, decltype(close_func)>(dlopen(comp_str.c_str(), RTLD_NOW), close_func);
        if (!comp_handle) {
            std::cerr << fmt::format("failed to open {}: {}\n", comp_str, dlerror());
            return EXIT_FAILURE;
        }
        dlerror(); // clear existing
        // Component shared_ptr
        auto comp_ptr = std::shared_ptr<composite::component>{nullptr};
        // Get the create function
        if (comp.contains("create_arg")) {
            // Get create arg if present
            auto create_arg = comp["create_arg"].get<std::string>();
            // Create function to include string_view argument
            using function_ptr = std::shared_ptr<composite::component> (*)(std::string_view);
            auto create_func = reinterpret_cast<function_ptr>(dlsym(comp_handle.get(), "create"));
            if (auto err = dlerror(); err != nullptr) {
                std::cerr << fmt::format("failed to find the 'create' symbol from {}: {}\n", comp_str, err);
                return EXIT_FAILURE;
            }
            dlerror(); // clear existing
            // Create a new component
            comp_ptr = (*create_func)(create_arg);
        } else {
            // Empty create function
            using function_ptr = std::shared_ptr<composite::component> (*)();
            auto create_func = reinterpret_cast<function_ptr>(dlsym(comp_handle.get(), "create"));
            if (auto err = dlerror(); err != nullptr) {
                std::cerr << fmt::format("failed to find the 'create' symbol from {}: {}\n", comp_str, err);
                return EXIT_FAILURE;
            }
            dlerror(); // clear existing
            // Create a new component
            comp_ptr = (*create_func)();
        }
        if (comp_ptr == nullptr) {
            spdlog::error("failed to create component {}", name);
            return EXIT_FAILURE;
        }
        // Set id if needed
        if (comp.contains("id")) {
            comp_ptr->id(comp["id"].get<std::string>());
        }
        spdlog::trace("component {} created", comp_ptr->id());
        // Set application-level properties
        spdlog::trace("setting app-level properties on {}", comp_ptr->id());
        for (const auto& prop : app_json["properties"]) {
            set_property(comp_ptr, prop);
        }
        // Set component-level properties
        spdlog::trace("setting component-level properties on {}", comp_ptr->id());
        for (const auto& prop : comp["properties"]) {
            set_property(comp_ptr, prop);
        }
        // Add to application
        spdlog::trace("adding {} to application '{}'", comp_ptr->id(), app.name());
        app.add_component(comp_ptr);
        // Store handle for closing later
        comp_handles.emplace_back(std::move(comp_handle));
    }

    // Make connections
    auto conn_exit = [&app](std::string_view msg) {
        spdlog::error(msg);
        app.clear();
        return EXIT_FAILURE;
    };
    for (const auto& conn : app_json["connections"]) {
        if (!conn.contains("output")) {
            return conn_exit(fmt::format("missing output for connection: {}", conn.dump()));
        }
        if (!conn.contains("input")) {
            return conn_exit(fmt::format("missing output for connection: {}", conn.dump()));
        }
        auto output = conn["output"];
        auto input = conn["input"];
        if (!output.contains("component")) {
            return conn_exit(fmt::format("missing component in connection output: {}", conn.dump()));
        }
        if (!output.contains("port")) {
            return conn_exit(fmt::format("missing port in connection output: {}", conn.dump()));
        }
        if (!input.contains("component")) {
            return conn_exit(fmt::format("missing component in connection input: {}", conn.dump()));
        }
        if (!input.contains("port")) {
            return conn_exit(fmt::format("missing port in connection input: {}", conn.dump()));
        }
        auto output_comp = output["component"].get<std::string>();
        auto output_port = output["port"].get<std::string>();
        auto input_comp = input["component"].get<std::string>();
        auto input_port = input["port"].get<std::string>();
        auto output_comp_ptr = app.get_component(output_comp);
        if (output_comp_ptr == nullptr) {
            return conn_exit(fmt::format("output component {} null during connection: {}", output_comp, conn.dump()));
        }
        auto input_comp_ptr = app.get_component(input_comp);
        if (input_comp_ptr == nullptr) {
            return conn_exit(fmt::format("input component {} null during connection: {}", input_comp, conn.dump()));
        }
        spdlog::trace("connecting {}:{} to {}:{}", output_comp, output_port, input_comp, input_port);
        if (!output_comp_ptr->connect(output_port, input_comp_ptr, input_port)) {
            return conn_exit(fmt::format("Failed to connect {}:{} to {}:{}", output_comp, output_port, input_comp, input_port));
        }
    }

    // Setup signal handlers
    auto signals = std::vector<int>{SIGINT, SIGKILL};
    auto sigset = sigset_t{};
    sigemptyset(&sigset);
    for (const auto& sig : signals) {
        sigaddset(&sigset, sig);
    }
    pthread_sigmask(SIG_BLOCK, &sigset, nullptr);
    auto signal_future = std::async(std::launch::async, [&sigset]() {
        auto signum = int{};
        sigwait(&sigset, &signum);
        printf("\r  \r");
        return signum;
    });

    // Initialize the application
    spdlog::trace("initializing application '{}'", app.name());
    app.initialize();

    // Start the application
    spdlog::trace("starting application '{}'", app.name());
    app.start();

    // Wait for signal to stop
    spdlog::trace("waiting for signal...");
    signal_future.wait();

    // Stop the application
    spdlog::trace("stopping application '{}'", app.name());
    app.stop();

    // Clean up the application resources
    spdlog::trace("clearing application '{}'", app.name());
    app.clear();

    return EXIT_SUCCESS;
}
