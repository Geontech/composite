/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
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
 
#pragma once

#include "component.hpp"
#include "lifecycle.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace composite {

/**
 * @brief Manages a collection of components and their lifecycle.
 *
 * The `application` class is responsible for initializing, starting, and stopping
 * a set of components. It provides a central point for managing the overall
 * application's state and behavior through its constituent parts.
 */
class application : public lifecycle {
public:
    /// Alias for a shared pointer to a component
    using component_ptr = std::shared_ptr<component>;

    /**
     * @brief Constructs an application with a given name.
     * @param name The name of the application.
     */
    explicit application(std::string_view name) : m_name(name) {}

    /**
     * @brief Gets the name of the application.
     * @return A constant reference to the application's name.
     */
    auto name() const noexcept -> const std::string& {
        return m_name;
    }

    /**
     * @brief Initializes all components managed by the application.
     *
     * This method iterates through all registered components and calls their
     * `initialize()` method.
     * @see lifecycle::initialize()
     */
    auto initialize() -> void override {
        for (auto& component : m_components) {
            component->initialize();
        }
    }

    /**
     * @brief Starts all enabled components managed by the application.
     *
     * This method iterates through all registered components. If a component
     * has an "enabled" property set to true, its `start()` method is called.
     * @see lifecycle::start()
     */
    auto start() -> void override {
        for (auto& component : m_components) {
            if (component->get_property<bool>("enabled")){
                component->start();
            }
        }
    }

    /**
     * @brief Stops all components managed by the application.
     *
     * This method iterates through all registered components and calls their
     * `stop()` method.
     * @see lifecycle::stop()
     */
    auto stop() -> void override {
        for (auto& component : m_components) {
            component->stop();
        }
    }

    /**
     * @brief Adds a component to the application.
     * @param comp A shared pointer to the component to be added.
     */
    auto add_component(component_ptr comp) -> void {
        m_components.emplace_back(comp);
    }

    /**
     * @brief Retrieves a component by its ID.
     * @param id The unique identifier of the component to retrieve.
     * @return A shared pointer to the component if found, otherwise a nullptr.
     */
    auto get_component(std::string_view id) const -> component_ptr {
        for (const auto& component : m_components) {
            if (component->id() == id) {
                return component;
            }
        }
        return {nullptr};
    }

    /**
     * @brief Gets a constant reference to the vector of managed components.
     * @return A constant reference to the vector of component shared pointers.
     */
    auto components() const -> const std::vector<component_ptr>& {
        return m_components;
    }

    /**
     * @brief Removes all components from the application.
     *
     * This method clears the internal list of components. Note that this does
     * not call `stop()` or `destroy()` on the components; it merely removes
     * them from the application's management.
     */
    auto clear() -> void {
        m_components.clear();
    }

private:
    std::string m_name; ///< The name of the application.
    std::vector<component_ptr> m_components; ///< Components managed by this application.

}; // class application

} // namespace composite
