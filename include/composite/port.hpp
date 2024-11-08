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

#include <concepts>
#include <memory>
#include <string>

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

    virtual auto connect(port* port) -> void {
        // to be implemented by derived class
    }

private:
    std::string m_name;

}; // class port

} // namespace composite