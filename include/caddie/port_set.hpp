#pragma once

#include "port.hpp"

#include <map>
#include <string>

namespace caddie {

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

private:
    port_map_t m_ports;

}; // class port_set

} // namespace caddie