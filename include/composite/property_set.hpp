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
#include <format>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

namespace composite {

namespace properties {

enum class config_type {
    INITIALIZE,
    RUNTIME
}; // enum class config_type

enum class error {
    OK,
    INVALID_TYPE,
    INVALID_KEY,
    INVALID_VALUE
}; // enum class error

class properties_error : public std::runtime_error {
public:
    explicit properties_error(std::string_view msg) : std::runtime_error(std::string{msg}) {}

}; // class properties_error

class configurability_error : public properties_error {
public:
    configurability_error(std::string_view comp_id, std::string_view prop) :
      properties_error(std::format("property {} of component {} is not runtime configurable", prop, comp_id)) {}

}; // class configurability_error

class type_error : public properties_error {
public:
    type_error(std::string_view comp_id, std::string_view prop, std::string_view type) :
      properties_error(std::format("unknown type {} for property {} of component {}", type, prop, comp_id)) {}

}; // class type_error

class key_error : public properties_error {
public:
    key_error(std::string_view comp_id, std::string_view prop) :
      properties_error(std::format("unknown property {} of component {}", prop, comp_id)) {}

}; // class key_error

class value_error : public properties_error {
public:
    value_error(std::string_view comp_id, std::string_view prop, std::string_view value) :
      properties_error(std::format("invalid value for property {} of component {}: {}", prop, comp_id, value)) {}

}; // class key_error

} // namespace properties

class property {
public:
    using change_func_type = std::function<bool()>;

    property(std::string_view type, std::any value) :
      m_type(type),
      m_value(value) {
    }

    auto type() const -> std::string {
        return m_type;
    }

    auto type(std::string_view value) -> void {
        m_type = value;
    }

    auto value() const -> const std::any& {
        return m_value;
    }

    auto value(std::any value) -> void {
        m_value = value;
    }

    auto units() const -> std::string {
        return m_units;
    }

    auto units(std::string_view u) -> property& {
        m_units = std::string{u};
        return *this;
    }

    auto configurability() const -> properties::config_type {
        return m_configurability;
    }

    auto configurability(properties::config_type value) -> property& {
        m_configurability = value;
        return *this;
    }

    auto change_listener() const -> change_func_type {
        return m_change_func;
    }

    auto change_listener(change_func_type value) -> void {
        m_change_func = value;
    }

private:
    std::string m_type;
    std::any m_value;
    std::string m_units;
    properties::config_type m_configurability;
    change_func_type m_change_func;

}; // class property

class property_set {
public:
    using property_map_type = std::map<std::string, property>;
    using change_func_type = property::change_func_type;

    template <typename T>
    auto add_property(std::string_view name, T* prop) -> property& {
        auto typeid_name = std::string{typeid(T).name()};
        auto status = int{};
        if (auto demangled_name = abi::__cxa_demangle(typeid_name.c_str(), nullptr, nullptr, &status); status == 0) {
            typeid_name = demangled_name;
            std::free(demangled_name);
        }
        if (typeid_name == "int") {
            typeid_name = "int32";
        } else if (typeid_name == "unsigned int") {
            typeid_name = "uint32";
        } else if (typeid_name == "long") {
            typeid_name = "int64";
        } else if (typeid_name == "unsigned long") {
            typeid_name = "uint64";
        } else if (typeid_name.find("basic_string") != std::string::npos) {
            typeid_name = "string";
        }
        if (auto [iter, res] = m_properties.try_emplace(std::string{name}, property{typeid_name, prop}); res) {
            return m_properties.at(std::string{name});
        }
    }

    template <typename T>
    auto set_property(std::string_view name, T value) -> properties::error {
        if (!m_properties.contains(std::string{name})) {
            return properties::error::INVALID_KEY;
        }
        const auto& prop = m_properties.at(std::string{name});
        auto val_ptr = *std::any_cast<T*>(&(prop.value()));
        auto prev_value = *val_ptr;
        *val_ptr = value;
        if (prop.change_listener() && !prop.change_listener()()) {
            *val_ptr = prev_value;
            return properties::error::INVALID_VALUE;
        }
        return properties::error::OK;
    }

    template <typename T>
    auto get_property(std::string_view name) const -> T {
        return *std::any_cast<T*>(m_properties.at(std::string{name}).value());
    }

    auto properties() const -> const property_map_type& {
        return m_properties;
    }

    auto add_change_listener(std::string_view name, change_func_type func) -> void {
        if (m_properties.contains(std::string{name})) {
            m_properties.at(std::string{name}).change_listener(func);
        }
    }

private:
    property_map_type m_properties;

}; // class property_set

} // namespace composite
