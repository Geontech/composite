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

#include <algorithm>
#include <concepts>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace composite {

namespace traits {

// shared_ptr
template<typename T> struct is_shared_ptr : std::false_type {};
template<typename T> struct is_shared_ptr<std::shared_ptr<T>> : std::true_type {};
template<typename T> constexpr bool is_shared_ptr_v = is_shared_ptr<T>::value;

// unique_ptr
template<typename T> struct is_unique_ptr : std::false_type {};
template<typename T> struct is_unique_ptr<std::unique_ptr<T>> : std::true_type {};
template<typename T> constexpr bool is_unique_ptr_v = is_unique_ptr<T>::value;

// concept for port types
template<typename T> concept smart_ptr = is_shared_ptr_v<T> || is_unique_ptr_v<T>;

} // namespace traits

class port {
public:
    explicit port(std::string_view name) : m_name(name) {}

    virtual ~port() = default;

    auto name() const noexcept -> std::string {
        return m_name;
    }

    virtual auto type_id() const noexcept -> std::size_t = 0;
    virtual auto is_unique_type() const noexcept -> bool = 0;

#ifdef COMPOSITE_USE_NATS
    virtual auto connect(std::string_view url, std::string_view subject) -> bool {
        // to be implemented by derived class
        return false;
    }
#endif

private:
    std::string m_name;

}; // class port

class input_port_base : public port {
public:
    using port::port;
    ~input_port_base() override {
        m_cv.notify_all();
    }

    auto depth() const -> std::size_t {
        const auto lock = std::scoped_lock{m_mtx};
        return m_depth;
    }

    auto depth(std::size_t value) -> void {
        const auto lock = std::scoped_lock{m_mtx};
        m_depth = value;
    }

    auto metadata(const composite::metadata& md) -> void {
        const auto lock = std::scoped_lock{m_mtx};
        m_metadata = md;
    }

protected:
    mutable std::mutex m_mtx;
    std::condition_variable m_cv;
    std::optional<composite::metadata> m_metadata;
    std::size_t m_depth{std::numeric_limits<std::size_t>::max()};

}; // class input_port_base

class output_port_base : public port {
public:
    using port::port;
    ~output_port_base() override = default;

    auto connect(input_port_base* port) -> void {
        m_connected_ports.emplace_back(port);
        // sort with unique_ptr ports at the back
        std::ranges::sort(m_connected_ports, [](const auto a, const auto b) { 
            return (!a->is_unique_type() && b->is_unique_type());
        });
    }

    auto disconnect() -> void {
        m_connected_ports.clear();
    }

    auto is_connected() const -> bool {
        return !m_connected_ports.empty();
    }

    auto send_metadata(const composite::metadata& value) const -> void {
        for (auto port : m_connected_ports) {
            if (port == nullptr) {
                continue;
            }
            port->metadata(value);
        }
    }

protected:
    std::vector<input_port_base*> m_connected_ports;

}; // class output_port_base

} // namespace composite