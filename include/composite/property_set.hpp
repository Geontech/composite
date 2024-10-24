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

#include <any>
#include <cxxabi.h>
#include <map>
#include <string>
#include <string_view>

namespace composite {

auto to_config_type(std::string_view value) -> std::string {
    if (value == "int") {
        return "int32";
    } else if (value == "unsigned int") {
        return "uint32";
    } else if (value == "long") {
        return "int64";
    } else if (value == "unsigned long") {
        return "uin642";
    }
    return value;
}

class property {
public:
    std::string type;
    std::any value;
}; // class property

class property_set {
public:
    using property_map_type = std::map<std::string, property>;

    template <typename T>
    auto add_property(std::string_view name, T* prop) -> void {
        auto typeid_name = std::string{typeid(T).name()};
        auto status = int{};
        if (auto demangled_name = abi::__cxa_demangle(typeid_name.c_str(), nullptr, nullptr, &status); status == 0) {
            typeid_name = demangled_name;
            std::free(demangled_name);
        }
        typeid_name = to_config_type(typeid_name);
        m_properties.try_emplace(std::string{name}, property{typeid_name, prop});
    }

    template <typename T>
    auto set_property(std::string_view name, T value) -> void {
        if (m_properties.contains(std::string{name})) {
            *(*std::any_cast<T*>(&(m_properties.at(std::string{name}).value))) = value;
        }
    }

    template <typename T>
    auto get_property(std::string_view name) const -> T {
        return *std::any_cast<T*>(m_properties.at(std::string{name}).value);
    }

    auto properties() const -> const property_map_type& {
        return m_properties;
    }

private:
    property_map_type m_properties;

}; // class property_set

} // namespace composite
