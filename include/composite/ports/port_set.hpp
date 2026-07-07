/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "port_base.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace composite {

class port_set {
public:
    using port_map_type = std::map<std::string, port_base*>;

    auto add_port(port_base& port) -> void { add_port(&port); }

    auto add_port(port_base* port) -> void {
        if (!port) {
            return;
        }
        m_ports.try_emplace(std::string{port->name()}, port);
    }

    /**
     * @brief Get port by name with type checking
     */
    template <typename T>
        requires(std::is_base_of_v<input_port_base, T> || std::is_base_of_v<output_port_base, T>)
    auto get_port(std::string_view name) -> T* {
        if (auto it = m_ports.find(std::string{name}); it != m_ports.end()) {
            if (auto* casted = dynamic_cast<T*>(it->second)) {
                return casted;
            }
        }
        return nullptr;
    }

    /**
     * @brief Query port mutability by name
     */
    auto is_mutable_port(std::string_view name) const -> std::optional<bool> {
        if (auto it = m_ports.find(std::string{name}); it != m_ports.end()) {
            return it->second->is_mutable();
        }
        return std::nullopt;
    }

    auto get_element_type_id(std::string_view name) const -> std::optional<std::size_t> {
        if (auto it = m_ports.find(std::string{name}); it != m_ports.end()) {
            return it->second->element_type_id();
        }
        return std::nullopt;
    }

    auto ports() const -> const port_map_type& { return m_ports; }

private:
    port_map_type m_ports;

}; // class port_set

} // namespace composite
