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
 * along with this program. If not, see http://www.gnu.org/licenses/.
 */

#pragma once

#include <any>
#include <cxxabi.h>
#include <fmt/core.h>
#include <functional>
#include <map>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>
#include <iostream>

namespace composite {

namespace properties {

constexpr std::string_view null_prop = "composite::properties::null";

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

template <typename T> struct is_optional : std::false_type {};
template <typename T> struct is_optional<std::optional<T>> : std::true_type {};
template <typename T> inline constexpr bool is_optional_v = is_optional<T>::value;

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
      properties_error(fmt::format("property {} of component {} is not runtime configurable", prop, comp_id)),
      prop(prop) {}

    std::string prop;

}; // class configurability_error

class type_error : public properties_error {
public:
    type_error(std::string_view comp_id, std::string_view prop, std::string_view type) :
      properties_error(fmt::format("unknown type {} for property {} of component {}", type, prop, comp_id)),
      prop(prop),
      type(type) {}

    std::string prop;
    std::string type;

}; // class type_error

class key_error : public properties_error {
public:
    key_error(std::string_view comp_id, std::string_view prop) :
      properties_error(fmt::format("unknown property {} of component {}", prop, comp_id)),
      prop(prop) {}

    std::string prop;

}; // class key_error

class value_error : public properties_error {
public:
    value_error(std::string_view comp_id, std::string_view prop, std::string_view value) :
      properties_error(fmt::format("invalid value for property {} of component {}: {}", prop, comp_id, value)),
      prop(prop),
      value(value) {}

    std::string prop;
    std::string value;

}; // class key_error

} // namespace properties

class property_set; // forward declaration

class property {
public:
    using change_func_type = std::function<bool()>;
    using indexed_change_func_type = std::function<bool(std::size_t)>;
    using any_change_listener = std::variant<std::monostate, change_func_type, indexed_change_func_type>;
    using struct_reset_func = std::function<void(std::any&)>;
    using struct_emplace_back_func = std::function<std::size_t(std::any&)>;
    using struct_erase_func = std::function<void(std::any&, std::size_t)>;
    using struct_registration_func = std::function<void(property_set&, void*)>;
    using struct_getter_func = std::function<void*(const std::any&, std::size_t)>;
    using struct_list_size_func = std::function<std::size_t(const std::any&)>;

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

    auto is_optional() const -> bool {
        return m_type.ends_with('?');
    }

    auto value() const -> const std::any& {
        return m_value;
    }

    auto value() -> std::any& {
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

    auto change_listener() const -> any_change_listener {
        return m_change_func;
    }

    auto change_listener(change_func_type value) -> void {
        if (this->is_list()) {
            throw std::runtime_error("list properties must use indexed change listener");
        }
        m_change_func = value;
    }

    auto change_listener(indexed_change_func_type value) -> void {
        if (!this->is_list()) {
            throw std::runtime_error("non-list properties must use non-indexed change listener");
        }
        m_change_func = value;
    }

    auto is_structured() const -> bool{
        return m_struct != nullptr;
    }

    auto structured() -> property_set& {
        return *m_struct;
    }

    auto structured() const -> const property_set& {
        return *m_struct;
    }

    auto structured(std::shared_ptr<property_set> s) -> void {
        m_struct = std::move(s);
    }

    auto struct_registration(struct_registration_func func) -> void {
        m_struct_registration = std::move(func);
    }

    auto struct_registration(property_set& set, void* ptr) const -> void {
        if (m_struct_registration) {
            return m_struct_registration(set, ptr);
        }
    }

    auto struct_reset(struct_reset_func func) -> void {
        m_struct_reset = std::move(func);
    }

    auto struct_reset() -> void {
        if (m_struct_reset) {
            m_struct_reset(m_value);
        }
    }

    auto struct_emplace_back(struct_emplace_back_func func) -> void {
        m_struct_emplace_back = std::move(func);
    }

    auto struct_emplace_back() -> std::optional<std::size_t> {
        if (m_struct_emplace_back) {
            return {m_struct_emplace_back(m_value)};
        }
        return std::nullopt;
    }

    auto struct_erase(struct_erase_func func) -> void {
        m_struct_erase = std::move(func);
    }

    auto struct_erase(std::size_t index) -> void {
        if (m_struct_erase) {
            m_struct_erase(m_value, index);
        }
    }

    auto struct_getter(struct_getter_func func) -> void {
        m_struct_getter = std::move(func);
    }

    auto struct_getter(std::size_t index) const -> void* {
        if (!m_struct_getter) {
            return nullptr;
        }
        return m_struct_getter(m_value, index);
    }

    auto struct_list_size(struct_list_size_func func) -> void {
        m_struct_list_size = std::move(func);
    }

    auto struct_list_size() const -> std::size_t {
        if (!m_struct_list_size) {
            return {};
        }
        return m_struct_list_size(m_value);
    }

    auto is_list() const -> bool {
        return m_type.starts_with("[]");
    }

private:
    std::string m_type;
    std::any m_value;
    std::string m_units;
    properties::config_type m_configurability{};
    any_change_listener m_change_func;
    std::shared_ptr<property_set> m_struct; // optional
    struct_reset_func m_struct_reset;
    struct_emplace_back_func m_struct_emplace_back;
    struct_erase_func m_struct_erase;
    struct_registration_func m_struct_registration;
    struct_getter_func m_struct_getter;
    struct_list_size_func m_struct_list_size;

}; // class property

class property_set {
public:
    using property_map_type = std::map<std::string, property, std::less<>>;
    using change_func_type = property::change_func_type;
    using indexed_change_func_type = property::indexed_change_func_type;

    template <typename T>
    auto add_property(std::string_view name, T* prop) -> property& {
        using ValueT = std::remove_cvref_t<T>;
        auto typeid_name = std::string{typeid(ValueT).name()};
        if constexpr (properties::is_optional_v<ValueT>) {
            typeid_name = std::string{typeid(typename ValueT::value_type).name()};
        }
        auto status = int{};
        if (auto demangled_name = abi::__cxa_demangle(typeid_name.c_str(), nullptr, nullptr, &status); status == 0) {
            typeid_name = demangled_name;
            std::free(demangled_name);
        }
        if (typeid_name == "bool" || typeid_name == "float" || typeid_name == "double") {
            // noop
        } else if (typeid_name == "short") {
            typeid_name = "int16";
        } else if (typeid_name == "unsigned short") {
            typeid_name = "uint16";
        } else if (typeid_name == "int") {
            typeid_name = "int32";
        } else if (typeid_name == "unsigned int") {
            typeid_name = "uint32";
        } else if (typeid_name == "long") {
            typeid_name = "int64";
        } else if (typeid_name == "unsigned long") {
            typeid_name = "uint64";
        } else if (typeid_name.find("basic_string") != std::string::npos) {
            typeid_name = "string";
        } else {
            throw properties::properties_error(fmt::format("unknown type {} for property {}", typeid_name, name));
        }
        if constexpr (properties::is_optional_v<ValueT>) {
            typeid_name += "?";
        }
        auto [iter, res] = m_properties.try_emplace(std::string{name}, property{typeid_name, prop});
        return iter->second;
    }

    template <typename T, typename Func>
    auto add_struct_property(std::string_view name, T* obj, Func&& register_fields) -> property& {
        auto nested_set = std::make_shared<property_set>();
        register_fields(*nested_set, obj);  // caller defined mechanism to bind fields
        auto p = property{"struct", obj};
        p.structured(nested_set);
        p.struct_reset([](std::any& s) {
            auto* tmp = std::any_cast<T*>(s);
            *tmp = T{};
        });
        auto [iter, _] = m_properties.try_emplace(std::string{name}, std::move(p));
        return iter->second;
    }

    template <typename T>
    auto add_list_property(std::string_view name, std::vector<T>* vec) -> property& {
        using ValueT = std::remove_cvref_t<T>;
        auto typeid_name = std::string{typeid(ValueT).name()};
        auto status = int{};
        if (auto demangled_name = abi::__cxa_demangle(typeid_name.c_str(), nullptr, nullptr, &status); status == 0) {
            typeid_name = demangled_name;
            std::free(demangled_name);
        }
        if (typeid_name == "bool" || typeid_name == "float" || typeid_name == "double") {
            // noop
        } else if (typeid_name == "short") {
            typeid_name = "int16";
        } else if (typeid_name == "unsigned short") {
            typeid_name = "uint16";
        } else if (typeid_name == "int") {
            typeid_name = "int32";
        } else if (typeid_name == "unsigned int") {
            typeid_name = "uint32";
        } else if (typeid_name == "long") {
            typeid_name = "int64";
        } else if (typeid_name == "unsigned long") {
            typeid_name = "uint64";
        } else if (typeid_name.find("basic_string") != std::string::npos) {
            typeid_name = "string";
        } else {
            throw properties::properties_error(fmt::format("unknown type {} for property {}", typeid_name, std::string{name}));
        }
        auto p = property{fmt::format("[]{}", typeid_name), vec};
        auto [iter, _] = m_properties.try_emplace(std::string{name}, std::move(p));
        return iter->second;
    }

    template <typename T, typename Func>
    auto add_struct_list_property(std::string_view name, std::vector<T>* vec, Func&& register_fields) -> property& {
        auto schema_set = std::make_shared<property_set>();
        T schema_instance{};
        register_fields(*schema_set, &schema_instance);

        auto p = property{"[]struct", vec};
        p.structured(schema_set);
        p.struct_registration([register_fields](property_set& ps, void* ptr) {
            register_fields(ps, static_cast<T*>(ptr));
        });
        p.struct_getter([](const std::any& vec_any, std::size_t index) -> void* {
            auto& tmp = *std::any_cast<std::vector<T>*>(vec_any);
            return &tmp[index];
        });
        p.struct_emplace_back([](std::any& vec_any) -> std::size_t {
            auto* tmp = std::any_cast<std::vector<T>*>(vec_any);
            tmp->emplace_back();
            return tmp->size() - 1;
        });
        p.struct_erase([](std::any& vec_any, std::size_t index) {
            auto* tmp = std::any_cast<std::vector<T>*>(vec_any);
            tmp->erase(tmp->begin() + index);
        });
        p.struct_list_size([](const std::any& vec_any) -> std::size_t {
            auto* tmp = std::any_cast<std::vector<T>*>(vec_any);
            return tmp->size();
        });

        auto [iter, _] = m_properties.try_emplace(std::string{name}, std::move(p));
        return iter->second;
    }

    auto set_properties(
      const std::vector<std::pair<std::string, std::string>>& values,
      properties::config_type config=properties::config_type::INITIALIZE,
      bool allow_unknown_key=false) -> void {
        for (const auto& [name, value] : values) {
            auto* prop = resolve_property(name);
            if (prop == nullptr) {
                if (allow_unknown_key) {
                    continue;
                }
                throw properties::key_error("", name);
            }
            using enum properties::config_type;
            if ((config == RUNTIME) && (prop->configurability() == INITIALIZE)) {
                throw properties::configurability_error("", name);
            }

            auto res = properties::error::OK;
            auto type = prop->type();
            if (prop->is_optional()) {
                type.pop_back(); // remove ?
            }
            std::string list_name;
            std::optional<std::size_t> list_idx;
            if (prop->is_list()) {
                type = type.substr(2); // remove []
                // Get the list name and index value
                if (const auto& p = parse_list_index_path(name)) {
                    list_name = p->base;
                    list_idx = p->index;
                }
            }

            // Set property value
            if (type == "bool") {
                auto v = bool{};
                if (value != composite::properties::null_prop) {
                    v = properties::convert_string_to<bool>(value);
                }
                if (prop->is_list()) {
                    if (list_idx.has_value()) {
                        if (value == composite::properties::null_prop) {
                            res = erase_list_property_item<bool>(list_name, *list_idx);
                        } else {
                            res = set_list_property_item(list_name, *list_idx, v);
                        }
                    } else {
                        res = append_list_property_item(list_name, v);
                    }
                } else {
                    res = set_property(name, v, value == composite::properties::null_prop);
                }
            } else if (type == "string") {
                auto v = std::string{};
                if (value != composite::properties::null_prop) {
                    v = value;
                }
                if (prop->is_list()) {
                    if (list_idx.has_value()) {
                        if (value == composite::properties::null_prop) {
                            res = erase_list_property_item<std::string>(list_name, *list_idx);
                        } else {
                            res = set_list_property_item(list_name, *list_idx, v);
                        }
                    } else {
                        res = append_list_property_item(list_name, v);
                    }
                } else {
                    res = set_property(name, v, value == composite::properties::null_prop);
                }
            } else if (type == "int16") {
                auto v = int16_t{};
                if (value != composite::properties::null_prop) {
                    v = properties::convert_string_to<int16_t>(value);
                }
                if (prop->is_list()) {
                    if (list_idx.has_value()) {
                        if (value == composite::properties::null_prop) {
                            res = erase_list_property_item<int16_t>(list_name, *list_idx);
                        } else {
                            res = set_list_property_item(list_name, *list_idx, v);
                        }
                    } else {
                        res = append_list_property_item(list_name, v);
                    }
                } else {
                    res = set_property(name, v, value == composite::properties::null_prop);
                }
            } else if (type == "uint16") {
                auto v = uint16_t{};
                if (value != composite::properties::null_prop) {
                    v = properties::convert_string_to<uint16_t>(value);
                }
                if (prop->is_list()) {
                    if (list_idx.has_value()) {
                        if (value == composite::properties::null_prop) {
                            res = erase_list_property_item<uint16_t>(list_name, *list_idx);
                        } else {
                            res = set_list_property_item(list_name, *list_idx, v);
                        }
                    } else {
                        res = append_list_property_item(list_name, v);
                    }
                } else {
                    res = set_property(name, v, value == composite::properties::null_prop);
                }
            } else if (type == "int32") {
                auto v = int32_t{};
                if (value != composite::properties::null_prop) {
                    v = properties::convert_string_to<int32_t>(value);
                }
                if (prop->is_list()) {
                    if (list_idx.has_value()) {
                        if (value == composite::properties::null_prop) {
                            res = erase_list_property_item<int32_t>(list_name, *list_idx);
                        } else {
                            res = set_list_property_item(list_name, *list_idx, v);
                        }
                    } else {
                        res = append_list_property_item(list_name, v);
                    }
                } else {
                    res = set_property(name, v, value == composite::properties::null_prop);
                }
            } else if (type == "uint32") {
                auto v = uint32_t{};
                if (value != composite::properties::null_prop) {
                    v = properties::convert_string_to<uint32_t>(value);
                }
                if (prop->is_list()) {
                    if (list_idx.has_value()) {
                        if (value == composite::properties::null_prop) {
                            res = erase_list_property_item<uint32_t>(list_name, *list_idx);
                        } else {
                            res = set_list_property_item(list_name, *list_idx, v);
                        }
                    } else {
                        res = append_list_property_item(list_name, v);
                    }
                } else {
                    res = set_property(name, v, value == composite::properties::null_prop);
                }
            } else if (type == "int64") {
                auto v = int64_t{};
                if (value != composite::properties::null_prop) {
                    v = properties::convert_string_to<int64_t>(value);
                }
                if (prop->is_list()) {
                    if (list_idx.has_value()) {
                        if (value == composite::properties::null_prop) {
                            res = erase_list_property_item<int64_t>(list_name, *list_idx);
                        } else {
                            res = set_list_property_item(list_name, *list_idx, v);
                        }
                    } else {
                        res = append_list_property_item(list_name, v);
                    }
                } else {
                    res = set_property(name, v, value == composite::properties::null_prop);
                }
            } else if (type == "uint64") {
                auto v = uint64_t{};
                if (value != composite::properties::null_prop) {
                    v = properties::convert_string_to<uint64_t>(value);
                }
                if (prop->is_list()) {
                    if (list_idx.has_value()) {
                        if (value == composite::properties::null_prop) {
                            res = erase_list_property_item<uint64_t>(list_name, *list_idx);
                        } else {
                            res = set_list_property_item(list_name, *list_idx, v);
                        }
                    } else {
                        res = append_list_property_item(list_name, v);
                    }
                } else {
                    res = set_property(name, v, value == composite::properties::null_prop);
                }
            } else if (type == "float") {
                auto v = float{};
                if (value != composite::properties::null_prop) {
                    v = properties::convert_string_to<float>(value);
                }
                if (prop->is_list()) {
                    if (list_idx.has_value()) {
                        if (value == composite::properties::null_prop) {
                            res = erase_list_property_item<float>(list_name, *list_idx);
                        } else {
                            res = set_list_property_item(list_name, *list_idx, v);
                        }
                    } else {
                        res = append_list_property_item(list_name, v);
                    }
                } else {
                    res = set_property(name, v, value == composite::properties::null_prop);
                }
            } else if (type == "double") {
                auto v = double{};
                if (value != composite::properties::null_prop) {
                    v = properties::convert_string_to<double>(value);
                }
                if (prop->is_list()) {
                    if (list_idx.has_value()) {
                        if (value == composite::properties::null_prop) {
                            res = erase_list_property_item<double>(list_name, *list_idx);
                        } else {
                            res = set_list_property_item(list_name, *list_idx, v);
                        }
                    } else {
                        res = append_list_property_item(list_name, v);
                    }
                } else {
                    res = set_property(name, v, value == composite::properties::null_prop);
                }
            } else if (type == "struct") {
                if (prop->is_list()) {
                    if (list_idx.has_value() && (value == composite::properties::null_prop)) {
                        res = erase_struct_list_property_item(prop, *list_idx);
                    }
                } else {
                    res = set_property(name, value, value == composite::properties::null_prop);
                }
            } else {
                throw properties::type_error("", name, type);
            }

            if (res == properties::error::INVALID_KEY && !allow_unknown_key) {
                throw properties::key_error("", name);
            } else if (res == properties::error::INVALID_VALUE) {
                throw properties::value_error("", name, value);
            }
        }
    }

    auto set_properties(
      const std::vector<std::pair<std::string, std::vector<std::string>>>& values,
      properties::config_type config=properties::config_type::INITIALIZE,
      bool allow_unknown_key=false) -> void {
        for (const auto& [name, value] : values) {
            auto* prop = resolve_property(name);
            if (prop == nullptr) {
                if (allow_unknown_key) {
                    continue;
                }
                throw properties::key_error("", name);
            }
            using enum properties::config_type;
            if ((config == RUNTIME) && (prop->configurability() == INITIALIZE)) {
                throw properties::configurability_error("", name);
            }

            auto res = properties::error::OK;
            auto type = prop->type();
            if (prop->is_optional()) {
                type.pop_back(); // remove ?
            }

            // Full list assignment
            if (prop->is_list()) {
                type = type.substr(2); // remove []
                if (type == "bool") {
                    std::vector<bool> vals;
                    for (const auto& v : value) {
                        vals.push_back(properties::convert_string_to<bool>(v));
                    }
                    res = set_list_property(name, vals);
                } else if (type == "string") {
                    res = set_list_property(name, value);
                } else if (type == "int16") {
                    std::vector<int16_t> vals;
                    for (const auto& v : value) {
                        vals.push_back(properties::convert_string_to<int16_t>(v));
                    }
                    res = set_list_property(name, vals);
                } else if (type == "uint16") {
                    std::vector<uint16_t> vals;
                    for (const auto& v : value) {
                        vals.push_back(properties::convert_string_to<uint16_t>(v));
                    }
                    res = set_list_property(name, vals);
                } else if (type == "int32") {
                    std::vector<int32_t> vals;
                    for (const auto& v : value) {
                        vals.push_back(properties::convert_string_to<int32_t>(v));
                    }
                    res = set_list_property(name, vals);
                } else if (type == "uint32") {
                    std::vector<uint32_t> vals;
                    for (const auto& v : value) {
                        vals.push_back(properties::convert_string_to<uint32_t>(v));
                    }
                    res = set_list_property(name, vals);
                } else if (type == "int64") {
                    std::vector<int64_t> vals;
                    for (const auto& v : value) {
                        vals.push_back(properties::convert_string_to<int64_t>(v));
                    }
                    res = set_list_property(name, vals);
                } else if (type == "uint64") {
                    std::vector<uint64_t> vals;
                    for (const auto& v : value) {
                        vals.push_back(properties::convert_string_to<uint64_t>(v));
                    }
                    res = set_list_property(name, vals);
                } else if (type == "float") {
                    std::vector<float> vals;
                    for (const auto& v : value) {
                        vals.push_back(properties::convert_string_to<float>(v));
                    }
                    res = set_list_property(name, vals);
                } else if (type == "double") {
                    std::vector<double> vals;
                    for (const auto& v : value) {
                        vals.push_back(properties::convert_string_to<double>(v));
                    }
                    res = set_list_property(name, vals);
                } else {
                    throw properties::type_error("", name, type);
                }
            } else {
                // this override only supports listed types
                throw properties::type_error("", name, type);
            }

            if (res == properties::error::INVALID_KEY && !allow_unknown_key) {
                throw properties::key_error("", name);
            } else if (res == properties::error::INVALID_VALUE) {
                auto value_str = std::string{"["};
                value_str = std::accumulate(
                    value.begin(),
                    value.end(),
                    value_str,
                    [](const std::string& acc, std::string val) {
                        return acc + val + ", ";
                    }
                );
                value_str.back() = ']';
                throw properties::value_error("", name, value_str);
            }
        }
    }

    auto set_properties(
      const std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>& values,
      properties::config_type config=properties::config_type::INITIALIZE,
      bool allow_unknown_key=false) -> void {
        for (const auto& [name, value] : values) {
            auto* prop = resolve_property(name);
            if (prop == nullptr) {
                if (allow_unknown_key) {
                    continue;
                }
                throw properties::key_error("", name);
            }
            using enum properties::config_type;
            if ((config == RUNTIME) && (prop->configurability() == INITIALIZE)) {
                throw properties::configurability_error("", name);
            }

            if (!prop->is_structured()) {
                throw properties::type_error("", name, prop->type());
            }

            // Set structured fields
            if (prop->is_list()) {
                const auto& p = parse_list_index_path(name);
                if (!p.has_value()) {
                    throw properties::key_error("", name);
                }
                if (!p->index.has_value()) { // append
                    // Construct a new struct type in the list
                    if (auto idx = prop->struct_emplace_back()) {
                        // Update the fields of the new struct
                        for (const auto& [k, v] : value) {
                            auto struct_prop_name = fmt::format("{}.{}", name, k);
                            auto res = set_struct_field(prop, *idx, k, v);
                            if (res == properties::error::INVALID_KEY) {
                                throw properties::key_error("", struct_prop_name);
                            } else if (res == properties::error::INVALID_VALUE) {
                                throw properties::value_error("", struct_prop_name, v);
                            }
                        }
                        auto valid_value = true;
                        auto cl = prop->change_listener();
                        if (std::holds_alternative<indexed_change_func_type>(cl)) {
                            valid_value = std::get<indexed_change_func_type>(cl)(*idx);
                        }
                        if (!valid_value) {
                            // TODO
                        }
                    } else {
                        throw properties::properties_error(fmt::format("failed to emplace new struct for structured list property {}", p->base));
                    }
                } else { // update
                    for (const auto& [k, v] : value) {
                        auto struct_prop_name = fmt::format("{}.{}", name, k);
                        auto res = set_struct_field(prop, *(p->index), k, v);
                        if (res == properties::error::INVALID_KEY && !allow_unknown_key) {
                            throw properties::key_error("", struct_prop_name);
                        } else if (res == properties::error::INVALID_VALUE) {
                            throw properties::value_error("", struct_prop_name, v);
                        }
                    }
                    auto valid_value = true;
                    auto cl = prop->change_listener();
                    if (std::holds_alternative<indexed_change_func_type>(cl)) {
                        valid_value = std::get<indexed_change_func_type>(cl)(*(p->index));
                    }
                    if (!valid_value) {
                        // TODO
                    }
                }
            } else {
                for (const auto& [k, v] : value) {
                    auto struct_prop_name = fmt::format("{}.{}", name, k);
                    auto& struct_prop_set = prop->structured();
                    auto* struct_prop = struct_prop_set.resolve_property(k);
                    if (struct_prop == nullptr) {
                        if (allow_unknown_key) {
                            continue;
                        }
                        throw properties::key_error("", struct_prop_name);
                    }
                    using enum properties::config_type;
                    if ((config == RUNTIME) && (struct_prop->configurability() == INITIALIZE)) {
                        throw properties::configurability_error("", struct_prop_name);
                    }

                    auto res = properties::error::OK;
                    auto type = struct_prop->type();
                    if (struct_prop->is_optional()) {
                        type.pop_back(); // remove ?
                    }

                    // Set property value
                    if (type == "bool") {
                        res = struct_prop_set.set_property(k, properties::convert_string_to<bool>(v));
                    } else if (type == "string") {
                        res = struct_prop_set.set_property(k, v);
                    } else if (type == "int16") {
                        res = struct_prop_set.set_property(k, properties::convert_string_to<int16_t>(v));
                    } else if (type == "uint16") {
                        res = struct_prop_set.set_property(k, properties::convert_string_to<uint16_t>(v));
                    } else if (type == "int32") {
                        res = struct_prop_set.set_property(k, properties::convert_string_to<int32_t>(v));
                    } else if (type == "uint32") {
                        res = struct_prop_set.set_property(k, properties::convert_string_to<uint32_t>(v));
                    } else if (type == "int64") {
                        res = struct_prop_set.set_property(k, properties::convert_string_to<int64_t>(v));
                    } else if (type == "uint64") {
                        res = struct_prop_set.set_property(k, properties::convert_string_to<uint64_t>(v));
                    } else if (type == "float") {
                        res = struct_prop_set.set_property(k, properties::convert_string_to<float>(v));
                    } else if (type == "double") {
                        res = struct_prop_set.set_property(k, properties::convert_string_to<double>(v));
                    } else {
                        throw properties::type_error("", struct_prop_name, type);
                    }
                    if (res == properties::error::INVALID_KEY && !allow_unknown_key) {
                        throw properties::key_error("", struct_prop_name);
                    } else if (res == properties::error::INVALID_VALUE) {
                        throw properties::value_error("", struct_prop_name, v);
                    }
                }
            }
        }
    }

    template <typename T>
    auto set_property(std::string_view name, T value, bool null_value=false) -> properties::error {
        auto* prop = resolve_property(name);
        if (!prop) {
            return properties::error::INVALID_KEY;
        }

        if (prop->is_structured()) {
            if (!null_value) {
                return properties::error::INVALID_VALUE;
            }
            prop->struct_reset();
        } else if (prop->is_optional()) {
            auto val_ptr = *std::any_cast<std::optional<T>*>(&(prop->value()));
            auto prev_value = std::optional<T>{};
            if (val_ptr->has_value()) {
                prev_value = val_ptr->value();
                if (null_value) {
                    val_ptr->reset();
                }
            }
            if (!null_value) {
                *val_ptr = value;
            }
            auto valid_value = true;
            auto cl = prop->change_listener();
            if (std::holds_alternative<change_func_type>(cl)) {
                valid_value = std::get<change_func_type>(cl)();
            }
            if (!valid_value) {
                *val_ptr = prev_value;
                return properties::error::INVALID_VALUE;
            }
        } else {
            auto val_ptr = *std::any_cast<T*>(&(prop->value()));
            auto prev_value = *val_ptr;
            *val_ptr = value;
            auto valid_value = true;
            auto cl = prop->change_listener();
            if (std::holds_alternative<change_func_type>(cl)) {
                valid_value = std::get<change_func_type>(cl)();
            }
            if (!valid_value) {
                *val_ptr = prev_value;
                return properties::error::INVALID_VALUE;
            }
        }
        return properties::error::OK;
    }

    auto set_struct_field(property* list_prop, size_t index, std::string_view field, const std::string& value_str) -> properties::error {
        if (!list_prop->is_structured()) {
            return properties::error::INVALID_TYPE;
        }

        auto bound = std::make_unique<property_set>();
        auto* item_ptr = list_prop->struct_getter(index);
        if (!item_ptr) {
            return properties::error::INVALID_VALUE;
        }
        list_prop->struct_registration(*bound, item_ptr);

        // parse field type from schema
        const auto* prop = bound->resolve_property(field);
        if (!prop) {
            return properties::error::INVALID_KEY;
        }

        auto type = prop->type();
        if (prop->is_optional()) {
            type.pop_back();
        }

        if (type == "bool") {
            return bound->set_property(field, properties::convert_string_to<bool>(value_str));
        } else if (type == "string") {
           return bound->set_property(field, value_str);
        } else if (type == "int16") {
            return bound->set_property(field, properties::convert_string_to<int16_t>(value_str));
        } else if (type == "uint16") {
            return bound->set_property(field, properties::convert_string_to<uint16_t>(value_str));
        } else if (type == "int32") {
            return bound->set_property(field, properties::convert_string_to<int32_t>(value_str));
        } else if (type == "uint32") {
            return bound->set_property(field, properties::convert_string_to<uint32_t>(value_str));
        } else if (type == "int64") {
            return bound->set_property(field, properties::convert_string_to<int64_t>(value_str));
        } else if (type == "uint64") {
            return bound->set_property(field, properties::convert_string_to<uint64_t>(value_str));
        } else if (type == "float") {
            return bound->set_property(field, properties::convert_string_to<float>(value_str));
        } else if (type == "double") {
            return bound->set_property(field, properties::convert_string_to<double>(value_str));
        }

        return properties::error::INVALID_TYPE;
    }

    template <typename T>
    auto get_property(std::string_view name) const -> T {
        const auto* prop = resolve_property(name);
        if (!prop) {
            throw properties::key_error("unknown", name);
        }
        return *std::any_cast<T*>(prop->value());
    }

    template <typename T>
    auto try_get_property(std::string_view name) const -> std::optional<T> {
        const auto* prop = resolve_property(name);
        if (!prop) {
            return std::nullopt;
        }
        if (prop->is_optional()) {
            return *std::any_cast<std::optional<T>*>(prop->value());
        }
        return std::nullopt;
    }

    template <typename T>
    auto get_list_property(std::string_view name) const -> const std::vector<T>& {
        const auto* prop = resolve_property(name);
        if (!prop || !prop->is_list()) {
            throw properties::key_error("unknown", name);
        }
        return *std::any_cast<std::vector<T>*>(prop->value());
    }

    template <typename T>
    auto set_list_property(std::string_view name, const std::vector<T>& value) -> properties::error {
        const auto* prop = resolve_property(name);
        if (!prop) {
            return properties::error::INVALID_KEY;
        }
        if (!prop->is_list()) {
            return properties::error::INVALID_TYPE;
        }
        auto& list = *std::any_cast<std::vector<T>*>(prop->value());
        auto old_value = list;
        list = value;
        auto valid_value = true;
        auto cl = prop->change_listener();
        if (std::holds_alternative<change_func_type>(cl)) {
            valid_value = std::get<change_func_type>(cl)();
        }
        if (!valid_value) {
            list = old_value;
            return properties::error::INVALID_VALUE;
        }
        return properties::error::OK;
    }

    template <typename T>
    auto get_list_property_item(std::string_view name, std::size_t index) const -> T {
        const auto* prop = resolve_property(name);
        if (!prop || !prop->is_list()) {
            throw properties::key_error("unknown", name);
        }
        const auto& list = *std::any_cast<std::vector<T>*>(prop->value());
        return list.at(index);
    }

    template <typename T>
    auto set_list_property_item(std::string_view name, std::size_t index, T value) -> properties::error {
        auto* prop = resolve_property(name);
        if (!prop) {
            return properties::error::INVALID_KEY;
        }
        if (!prop->is_list()) {
            return properties::error::INVALID_TYPE;
        }
        auto& list = *std::any_cast<std::vector<T>*>(prop->value());
        auto old_value = list.at(index);
        list.at(index) = value;
        auto valid_value = true;
        auto cl = prop->change_listener();
        if (std::holds_alternative<indexed_change_func_type>(cl)) {
            valid_value = std::get<indexed_change_func_type>(cl)(index);
        }
        if (!valid_value) {
            list.at(index) = old_value;
            return properties::error::INVALID_VALUE;
        }
        return properties::error::OK;
    }

    template <typename T>
    auto append_list_property_item(std::string_view name, T value) -> properties::error {
        using enum properties::error;
        const auto* prop = resolve_property(name);
        if (!prop) {
            return INVALID_KEY;
        }
        if (!prop->is_list()) {
            return INVALID_TYPE;
        }
        auto& list = *std::any_cast<std::vector<T>*>(prop->value());
        auto old_value = list;
        list.push_back(value);
        auto valid_value = true;
        auto cl = prop->change_listener();
        if (std::holds_alternative<indexed_change_func_type>(cl)) {
            valid_value = std::get<indexed_change_func_type>(cl)(list.size() - 1);
        }
        if (!valid_value) {
            list = old_value;
            return INVALID_VALUE;
        }
        return OK;
    }

    template<typename T>
    auto erase_list_property_item(std::string_view name, std::size_t index) -> properties::error {
        const auto* prop = resolve_property(name);
        if (!prop || !prop->is_list()) {
            throw properties::key_error("unknown", name);
        }
        auto& list = *std::any_cast<std::vector<T>*>(prop->value());
        if (index >= list.size()) { // range error
            return properties::error::INVALID_KEY;
        }
        list.erase(list.begin() + index);
        auto cl = prop->change_listener();
        if (std::holds_alternative<indexed_change_func_type>(cl)) {
            auto valid_value = std::get<indexed_change_func_type>(cl)(index);
        }
        return properties::error::OK;
    }

    auto erase_struct_list_property_item(property* prop, size_t index) -> properties::error {
        if (!prop->is_structured()) {
            return properties::error::INVALID_TYPE;
        }
        auto size = prop->struct_list_size();
        if (index >= size) { // range error
            return properties::error::INVALID_KEY;
        }
        prop->struct_erase(index);
        auto cl = prop->change_listener();
        if (std::holds_alternative<indexed_change_func_type>(cl)) {
            auto valid_value = std::get<indexed_change_func_type>(cl)(index);
        }
        return properties::error::INVALID_TYPE;
    }

    auto properties() const -> const property_map_type& {
        return m_properties;
    }

    auto add_change_listener(std::string_view name, change_func_type func) -> void {
        if (auto* p = resolve_property(name)) {
            p->change_listener(func);
        }
    }

private:
    property_map_type m_properties;

    auto resolve_property(std::string_view path) -> property* {
        auto dot = path.find('.');
        auto bracket = path.find('[');
        if (dot == std::string_view::npos && bracket == std::string_view::npos) {
            auto it = m_properties.find(std::string{path});
            return it != m_properties.end() ? &it->second : nullptr;
        }

        if (bracket != std::string_view::npos && (dot == std::string_view::npos || (bracket < dot))) {
            auto name = path.substr(0, bracket);
            auto it = m_properties.find(std::string{name});
            if (it == m_properties.end() || !it->second.is_list()) {
                return nullptr;
            }

            auto end_bracket = path.find(']', bracket);
            if (end_bracket == std::string_view::npos) {
                return nullptr; // malformed
            }

            if (it->second.is_structured()) {
                if (it->second.is_list()) {
                    return &it->second;
                }
                auto tail_start = end_bracket + 1;
                while (tail_start < path.size() && path[tail_start] == '.') {
                    ++tail_start; // skip dots
                }
                if (tail_start >= path.size()) {
                    return nullptr;
                }
                auto tail = path.substr(tail_start);
                return it->second.structured().resolve_property(tail);
            } else if (dot == std::string_view::npos) {
                return &it->second;
            }

            return nullptr;
        }

        auto head = path.substr(0, dot);
        auto tail = path.substr(dot + 1);
        auto it = m_properties.find(std::string{head});
        if (it == m_properties.end() || !it->second.is_structured()) {
            return nullptr;
        }
        return it->second.structured().resolve_property(tail);
    }

    auto resolve_property(std::string_view path) const -> const property* {
        return const_cast<property_set*>(this)->resolve_property(path);
    }

    struct list_path_info {
        std::string_view base;
        std::optional<size_t> index;
    };

    inline auto parse_list_index_path(std::string_view name) -> std::optional<list_path_info> {
        auto bracket_pos = name.find('[');
        if (bracket_pos == std::string_view::npos) {
            return std::nullopt;
        }
        auto end_bracket = name.find(']', bracket_pos);
        if (end_bracket == std::string_view::npos) {
            return std::nullopt;
        }
        auto base = name.substr(0, bracket_pos);
        auto content = name.substr(bracket_pos +1, end_bracket - bracket_pos - 1);
        auto tail_start = end_bracket + 1;
        if (tail_start < name.size() && name[tail_start] == '.') {
            ++tail_start; // skip dots
        }
        auto tail = name.substr(tail_start);
        if (content.empty()) {
            return list_path_info{base, std::nullopt}; // append
        }
        try {
            auto idx = std::stoul(std::string{content});
            return list_path_info{base, idx}; // update
        } catch (...) {/*noop*/}
        return std::nullopt;
    }

    inline auto split_list_values(const std::string& value) const -> std::vector<std::string> {
        auto tmp = value;
        if (tmp.starts_with('[')) {
            tmp = tmp.substr(1);
        }
        if (tmp.ends_with(']')) {
            tmp.pop_back();
        }
        std::vector<std::string> tokens;
        std::string token;
        std::istringstream stream(tmp);
        while (std::getline(stream, token, ',')) {
            tokens.push_back(token);
        }
        return tokens;
    }

}; // class property_set

} // namespace composite
