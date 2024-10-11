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

#include "input_port.hpp"
#include "lifecycle.hpp"
#include "output_port.hpp"
#include "port_set.hpp"
#include "property_set.hpp"

#include <string>
#include <string_view>
#include <thread>

namespace composite {

enum class retval : int {
    NORMAL,
    NOOP,
    FINISH,
    NO_YIELD
}; // enum class retval

class component : public lifecycle {
    static constexpr int DEFAULT_DELAY{1000000};

public:
    struct connection {
        std::pair<std::string, std::string> output;
        std::pair<std::string, std::string> input;
    };

    explicit component(std::string_view name) :
      m_name(name),
      m_id(m_name) {
        add_property("thread_delay", &m_delay);
    }

    ~component() override = default;

    auto name() const noexcept -> std::string {
        return m_name;
    }

    auto id() const noexcept -> std::string {
        return m_id;
    }

    auto id(std::string_view id) -> void {
        m_id = id;
    }

    auto initialize() -> void override {
        // To be implemented by subclasses
    }

    auto start() -> void override {
        m_thread = std::jthread(&component::thread_func, this);
    }

    auto stop() -> void override {
        m_thread.request_stop();
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    virtual auto process() -> retval = 0;

    auto add_port(port* port) {
        m_port_set.add_port(port);
    }

    auto get_port(std::string_view name) -> port* {
        return m_port_set.get_port(name);
    }

    auto ports() const -> const std::map<std::string, port*>& {
        return m_port_set.ports();
    }

    auto connect(
      std::string_view output_port_name,
      component* other,
      std::string_view input_port_name
    ) -> bool {
        auto out_port = get_port(output_port_name);
        if (out_port == nullptr) {
            return false;
        }
        if (other == nullptr) {
            return false;
        }
        auto in_port = other->get_port(input_port_name);
        if (in_port == nullptr) {
            return false;
        }
        if (out_port->type_id() != in_port->type_id()) {
            return false;
        }
        out_port->connect(in_port);
        m_connections.push_back({
            .output = std::make_pair(id(), std::string{output_port_name}),
            .input = std::make_pair(other->id(), std::string{input_port_name})
        });
        return true;
    }

    auto connections() const -> const std::vector<connection>& {
        return m_connections;
    }

    template <typename T>
    auto add_property(std::string_view name, T* prop) -> void {
        m_prop_set.add_property(name, prop);
    }

    template <typename T>
    auto set_property(std::string_view name, T value) -> void {
        m_prop_set.set_property(name, value);
    }

    template <typename T>
    auto get_property(std::string_view name) const -> T {
        return m_prop_set.get_property<T>(name);
    }

    auto properties() const -> const std::map<std::string, std::pair<std::string, std::any>>& {
        return m_prop_set.properties();
    }

private:
    std::string m_name;
    std::string m_id;
    std::jthread m_thread;
    std::chrono::nanoseconds m_delay{DEFAULT_DELAY};
    port_set m_port_set;
    property_set m_prop_set;
    std::vector<connection> m_connections;

    auto thread_func(std::stop_token token) -> void {
        while (!token.stop_requested()) {
            auto res = process();
            if (res == retval::NOOP) {
                std::this_thread::sleep_for(m_delay);
            } else if (res == retval::FINISH) {
                break;
            } else if (res == retval::NORMAL) {
                std::this_thread::yield();
            }
        }
    }

}; // class component

} // namespace composite