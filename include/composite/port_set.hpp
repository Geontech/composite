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
 
#pragma once

#include "port.hpp"

#include <map>
#include <string>

namespace composite {

class port_set {
    using port_map_t = std::map<std::string, port*>;
public:
    auto add_port(port* port) -> void {
        m_ports.try_emplace(port->name(), port);
    }

    auto get_port(std::string_view name) -> port* {
        if (m_ports.contains(std::string{name})) {
            return m_ports.at(std::string{name});
        }
        return nullptr;
    }

    auto ports() const -> const port_map_t& {
        return m_ports;
    }

private:
    port_map_t m_ports;

}; // class port_set

} // namespace composite