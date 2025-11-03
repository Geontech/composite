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

    auto add_port(port_base* port) -> void {
        m_ports.try_emplace(std::string{port->name()}, port);
    }

    /**
     * @brief Get port by name with type checking 
     */
    template <typename T>
    requires (
      std::is_base_of_v<input_port_base, T> ||
      std::is_base_of_v<output_port_base, T>
    )
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

    auto ports() const -> const port_map_type& {
        return m_ports;
    }

private:
    port_map_type m_ports;

}; // class port_set

} // namespace composite