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

#include <concepts>
#include <mutex>
#include <sstream>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>

namespace composite {

enum class retval : int {
    NORMAL,
    NOOP,
    FINISH,
    NO_YIELD
}; // enum class retval

class component : public lifecycle {
    static constexpr uint32_t DEFAULT_DELAY{1000000};

public:
    struct connection {
        std::pair<std::string, std::string> output;
        std::pair<std::string, std::string> input;
    };

    explicit component(std::string_view name) :
      m_name(name),
      m_id(m_name),
      m_logger(spdlog::stdout_color_mt(m_name)) {
        add_property("thread_delay", &m_delay).units("ns");
        using enum composite::properties::config_type;
        add_property("enabled", &m_enabled).configurability(RUNTIME);
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
        m_enabled = true;
    }

    auto stop() -> void override {
        m_enabled = false;
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
      std::shared_ptr<component> other,
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

#ifdef COMPOSITE_USE_NATS
    auto connect(std::string_view port_name, std::string_view url, std::string_view subject, bool input=false) -> bool {
        auto port = get_port(port_name);
        if (port == nullptr) {
            return false;
        }
        auto res = port->connect(std::string{url}, std::string{subject});
        if (res) {
            if (input) {
                m_connections.push_back({
                    .output = std::make_pair(std::string{url}, std::string{subject}),
                    .input = std::make_pair(id(), std::string{port_name}),
                });
            } else {
                m_connections.push_back({
                    .output = std::make_pair(id(), std::string{port_name}),
                    .input = std::make_pair(std::string{url}, std::string{subject})
                });
            }
        }
        return res;
    }
#endif

    auto connections() const -> const std::vector<connection>& {
        return m_connections;
    }

    template <typename T>
    auto add_property(std::string_view name, T* prop) -> property& {
       return  m_prop_set.add_property(name, prop);
    }

    template <typename T>
    auto get_property(std::string_view name) const -> T {
        return m_prop_set.get_property<T>(name);
    }

    auto set_properties(
      const std::vector<std::pair<std::string, std::string>>& prop_values,
      properties::config_type config=properties::config_type::INITIALIZE,
      bool allow_invalid_key=false) -> void {
        m_prop_change_requested = true;
        auto lk = std::scoped_lock{m_prop_mtx};
        const auto& props = properties();
        for (const auto& [name, value] : prop_values) {
            if (!props.contains(name)) {
                auto err = properties::key_error(m_id, name);
                logger()->warn(err.what());
                throw err;
            }
            using enum properties::config_type;
            if ((config == RUNTIME) && (props.at(name).configurability() == INITIALIZE)) {
                auto err = properties::configurability_error(m_id, name);
                logger()->warn(err.what());
                throw err;
            }
            auto type = props.at(name).type();
            auto res = properties::error::OK;
            if (type == "bool") {
                res = m_prop_set.set_property(name, (value == "1" || value == "true") ? true : false);
            } else if (type == "string") {
                res = m_prop_set.set_property(name, value);
            } else if (type == "int32") {
                res = m_prop_set.set_property(name, static_cast<int32_t>(std::stoi(value)));
            } else if (type == "uint32") {
                res = m_prop_set.set_property(name, static_cast<uint32_t>(std::stoul(value)));
            } else if (type == "int64") {
                res = m_prop_set.set_property(name, static_cast<int64_t>(std::stoll(value)));
            } else if (type == "uint64") {
                res = m_prop_set.set_property(name, static_cast<uint64_t>(std::stoull(value)));
            } else if (type == "float") {
                res = m_prop_set.set_property(name, std::stof(value));
            } else if (type == "double") {
                res = m_prop_set.set_property(name, std::stod(value));
            } else {
                auto err = properties::type_error(m_id, name, type);
                logger()->warn(err.what());
                throw err;
            }
            if (res == properties::error::INVALID_KEY && !allow_invalid_key) {
                auto err = properties::key_error(m_id, name);
                logger()->warn(err.what());
                throw err;
            } else if (res == properties::error::INVALID_VALUE) {
                auto err = properties::value_error(m_id, name, value);
                logger()->warn(err.what());
                throw err;
            }
        }
        property_change_handler();
        m_prop_change_requested = false;
    }

    auto add_property_change_listener(std::string_view name, property_set::change_func_type func) -> void {
        m_prop_set.add_change_listener(name, func);
    }

    virtual auto property_change_handler() -> void {
        // To be implemented by subclasses
        // Gets executed at the end of set_properties function
    }

    auto properties() const -> const typename property_set::property_map_type& {
        return m_prop_set.properties();
    }

    auto log_level(spdlog::level::level_enum level) -> void {
        m_logger->set_level(level);
    }

protected:
    auto logger() const -> std::shared_ptr<spdlog::logger> {
        return m_logger;
    }

private:
    std::string m_name;
    std::string m_id;
    std::shared_ptr<spdlog::logger> m_logger;
    std::jthread m_thread;
    uint32_t m_delay{DEFAULT_DELAY};
    bool m_enabled{true};
    port_set m_port_set;
    property_set m_prop_set;
    std::mutex m_prop_mtx;
    std::atomic_bool m_prop_change_requested{};
    std::vector<connection> m_connections;

    auto thread_func(std::stop_token token) -> void {
        using enum retval;
        while (!token.stop_requested()) {
            if (m_prop_change_requested) {
                std::this_thread::yield();
            }
            auto lk = std::scoped_lock{m_prop_mtx};
            auto res = process();
            if (res == NOOP) {
                std::this_thread::sleep_for(std::chrono::nanoseconds{m_delay});
            } else if (res == FINISH) {
                break;
            } else if (res == NORMAL) {
                std::this_thread::yield();
            }
        }
    }

}; // class component

} // namespace composite