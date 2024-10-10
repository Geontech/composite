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
#include "input_port.hpp"
#include "timestamp.hpp"

#include <ranges>
#include <string_view>
#include <typeinfo>

namespace composite {

template <traits::smart_ptr T>
class output_port : public port {
public:
    using value_type = typename T::element_type;
    using buffer_type = T;
    using timestamp_type = timestamp;

    explicit output_port(std::string_view name) : port(name) {}

    auto type_id() const noexcept -> std::size_t override {
        return typeid(T).hash_code();
    }

    auto send_data(buffer_type data, timestamp_type ts) -> void {
        for (auto i : std::views::iota(size_t{0}, m_connected_ports.size())) {
            if (auto port = m_connected_ports.at(i); port != nullptr) {
                if constexpr (traits::is_unique_ptr_v<T>) {
                    if (i == m_connected_ports.size() - 1) {
                        // last port, move incoming
                        port->add_data({std::move(data), ts});
                    } else {
                        // make a copy of the incoming data
                        auto data_copy = std::make_unique<value_type>(*data);
                        port->add_data({std::move(data_copy), ts});
                    }
                } else { // shared_ptr
                    port->add_data({data, ts});
                }
            }
        }
    }

    auto connect(port* port) -> void override {
        m_connected_ports.emplace_back(static_cast<input_port<T>*>(port));
    }

    auto disconnect() -> void {
        m_connected_ports.clear();
    }

    auto is_connected() const -> bool {
        return !m_connected_ports.empty();
    }

    void eos(bool value) const {
        for (auto port : m_connected_ports) {
            if (port) {
                port->eos(value);
            }
        }
    }

private:
    std::vector<input_port<T>*> m_connected_ports;

}; // class output_port

} // namespace composite
