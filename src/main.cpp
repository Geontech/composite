#include "caddie/application.hpp"
#include "caddie/version.hpp"

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

auto set_property(std::shared_ptr<caddie::component> comp, const nlohmann::json& prop) {
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
    auto program = argparse::ArgumentParser{"caddie", VERSION};
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

    // Create a new application object
    auto app_name = app_json["name"].get<std::string>();
    auto app = caddie::application{app_name};

    // Get components and load them
    for (const auto& comp : app_json["components"]) {
        auto comp_str = fmt::format("lib{}.so", comp["name"].get<std::string>());
        spdlog::debug("component: {}", comp_str);
        auto comp_handle = dlopen(comp_str.c_str(), RTLD_NOW);
        if (!comp_handle) {
            std::cerr << fmt::format("Failed to open {}: {}\n", comp_str, dlerror());
            return EXIT_FAILURE;
        }
        dlerror(); // clear existing
        // Get the create function
        std::shared_ptr<caddie::component> (*create_func)();
        *(void**)(&create_func) = dlsym(comp_handle, "create");
        if (auto err = dlerror(); err != nullptr) {
            std::cerr << fmt::format("Failed to find the 'create' symbol from {}: {}\n", comp_str, err);
            return EXIT_FAILURE;
        }
        dlerror(); // clear existing
        // Create a new component
        auto comp_ptr = (*create_func)();
        // Set log level
        comp_ptr->log_level(spdlog::level::from_str(level));
        // Set application-level properties
        for (const auto& prop : app_json["properties"]) {
            set_property(comp_ptr, prop);
        }
        // Set component-level properties
        for (const auto& prop : comp["properties"]) {
            set_property(comp_ptr, prop);
        }
        // Add to application
        app.add_component(comp_ptr);
        // Close handle
        dlclose(comp_handle);
    }

    // Make connections
    for (const auto& conn : app_json["connections"]) {
        auto output = conn["output"];
        auto input = conn["input"];
        auto output_comp = output["component"].get<std::string>();
        auto output_port = output["port"].get<std::string>();
        auto input_comp = input["component"].get<std::string>();
        auto input_port = input["port"].get<std::string>();
        auto output_comp_ptr = app.get_component(output_comp);
        auto input_comp_ptr = app.get_component(input_comp);
        if (!output_comp_ptr->connect(output_port, input_comp_ptr, input_port)) {
            spdlog::error("Failed to connect {}:{} to {}:{}", output_comp, output_port, input_comp, input_port);
            return EXIT_FAILURE;
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
    app.initialize();

    // Start the application
    app.start();

    // Wait for signal to stop
    signal_future.wait();

    // Stop the application
    app.stop();

    return EXIT_SUCCESS;
}
