/*
 * Copyright (C) 2025 Geon Technologies, LLC
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
 * along with this program. If not, see http://www.gnu.org/licenses/.
 */

#pragma once

#include <cxxabi.h>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace composite::properties {

constexpr std::string_view null_prop = "composite::properties::null";

enum class config_type {
    INITIALIZE,
    RUNTIME
};

enum class error {
    OK,
    INVALID_TYPE,
    INVALID_KEY,
    INVALID_VALUE
};

class properties_error : public std::runtime_error {
public:
    explicit properties_error(std::string_view msg) : std::runtime_error(std::string{msg}) {}
};

class configurability_error : public properties_error {
public:
    configurability_error(std::string_view comp_id, std::string_view prop) :
      properties_error(std::format("property {} of component {} is not runtime configurable", prop, comp_id)),
      prop(prop) {}
    std::string prop;
};

class type_error : public properties_error {
public:
    type_error(std::string_view comp_id, std::string_view prop, std::string_view type) :
      properties_error(std::format("unknown type {} for property {} of component {}", type, prop, comp_id)),
      prop(prop),
      type(type) {}
    std::string prop;
    std::string type;
};

class key_error : public properties_error {
public:
    key_error(std::string_view comp_id, std::string_view prop) :
      properties_error(std::format("unknown property {} of component {}", prop, comp_id)),
      prop(prop) {}
    std::string prop;
};

class value_error : public properties_error {
public:
    value_error(std::string_view comp_id, std::string_view prop, std::string_view value) :
      properties_error(std::format("invalid value for property {} of component {}: {}", prop, comp_id, value)),
      prop(prop),
      value(value) {}
    std::string prop;
    std::string value;
};

template <typename T> struct is_optional : std::false_type {};
template <typename T> struct is_optional<std::optional<T>> : std::true_type {};
template <typename T> inline constexpr bool is_optional_v = is_optional<T>::value;

template <typename T>
T convert_string_to(const std::string& value);

#define CONVERT_STRING_SPECIALIZATION(TYPE, FUNC) \
template <> inline TYPE convert_string_to<TYPE>(const std::string& value) { return FUNC(value); }

CONVERT_STRING_SPECIALIZATION(bool, [](const std::string& v){ return v == "1" || v == "true"; })
CONVERT_STRING_SPECIALIZATION(int16_t, [](const std::string& v){ return static_cast<int16_t>(std::stoi(v)); })
CONVERT_STRING_SPECIALIZATION(uint16_t, [](const std::string& v){ return static_cast<uint16_t>(std::stoul(v)); })
CONVERT_STRING_SPECIALIZATION(int32_t, [](const std::string& v){ return static_cast<int32_t>(std::stoi(v)); })
CONVERT_STRING_SPECIALIZATION(uint32_t, [](const std::string& v){ return static_cast<uint32_t>(std::stoul(v)); })
CONVERT_STRING_SPECIALIZATION(int64_t, [](const std::string& v){ return static_cast<int64_t>(std::stoll(v)); })
CONVERT_STRING_SPECIALIZATION(uint64_t, [](const std::string& v){ return static_cast<uint64_t>(std::stoull(v)); })
CONVERT_STRING_SPECIALIZATION(float, std::stof)
CONVERT_STRING_SPECIALIZATION(double, std::stod)
CONVERT_STRING_SPECIALIZATION(std::string, [](const std::string& v){ return v; })

#undef CONVERT_STRING_SPECIALIZATION

class type_registry {
public:
    template <typename T>
    static auto get_type_name() -> std::string {
        using ValueT = std::remove_cvref_t<T>;

        if constexpr (is_optional_v<ValueT>) {
            using InnerT = typename ValueT::value_type;
            auto typeid_name = std::string{typeid(InnerT).name()};
            return demangle_and_normalize(typeid_name);
        } else {
            auto typeid_name = std::string{typeid(ValueT).name()};
            return demangle_and_normalize(typeid_name);
        }
    }

    static auto normalize_type_string(std::string_view type) -> std::string {
        auto result = std::string{type};
        if (result.ends_with('?')) {
            result.pop_back();
        }
        if (result.starts_with("[]")) {
            result = result.substr(2);
        }
        return result;
    }

private:
    static auto demangle_and_normalize(const std::string& typeid_name) -> std::string {
        auto status = int{};
        auto result = typeid_name;

        if (auto demangled = abi::__cxa_demangle(typeid_name.c_str(), nullptr, nullptr, &status); status == 0) {
            result = demangled;
            std::free(demangled);
        }

        return normalize_type_name(result);
    }

    static auto normalize_type_name(std::string name) -> std::string {
        if (name == "bool" || name == "float" || name == "double") {
            return name;
        }
        if (name == "short") {
            return "int16";
        }
        if (name == "unsigned short") {
            return "uint16";
        }
        if (name == "int") {
            return "int32";
        }
        if (name == "unsigned int") {
            return "uint32";
        }
        if (name == "long") {
            return "int64";
        }
        if (name == "unsigned long") {
            return "uint64";
        }
        if (name.find("basic_string") != std::string::npos) {
            return "string";
        }

        throw properties_error(std::format("unknown type {}", name));
    }
};

} // namespace composite::properties
