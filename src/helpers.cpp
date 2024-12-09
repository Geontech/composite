#include "composite/application.hpp"
#include "helpers.hpp"

#include <iostream>
#include <fmt/core.h>
#include <spdlog/spdlog.h>

namespace composite {

auto close_func(void* p) -> void {
    dlclose(p);
};

auto make_component(const nlohmann::json& comp_json, component_handles_type& handles) -> std::shared_ptr<composite::component> {
    // Get component name
    auto name = comp_json["name"].get<std::string>();
    // Open component module
    auto comp_str = fmt::format("lib{}.so", name);
    spdlog::trace("component module: {}", comp_str);
    // Get component module handle
    auto comp_handle = std::unique_ptr<void, decltype(&close_func)>(dlopen(comp_str.c_str(), RTLD_NOW), close_func);
    if (!comp_handle) {
        std::cerr << fmt::format("failed to open {}: {}\n", comp_str, dlerror());
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
            std::cerr << fmt::format("failed to find the 'create' symbol from {}: {}\n", comp_str, err);
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
            std::cerr << fmt::format("failed to find the 'create' symbol from {}: {}\n", comp_str, err);
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
    }
    // Store handle for closing later
    handles.emplace_back(std::move(comp_handle));
    spdlog::trace("component {} created", comp_ptr->id());
    return comp_ptr;
}

} // namespace composite
