/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
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

#include "property_metadata.hpp"
#include "property_operations.hpp"
#include "property.hpp"

#include <format>
#include <map>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace composite {

class property_set {
public:
    using property_map_type = std::map<std::string, property, std::less<>>;
    using change_func_type = property::change_func_type;
    using indexed_change_func_type = property::indexed_change_func_type;

    /**
     * @brief Registers a scalar property with the property set
     * @tparam T The property value type (must be registered in type_registry)
     * @param name The property name (must be unique within this property set)
     * @param prop Pointer to the member variable storing the property value
     * @return Reference to the property for chaining .units() and .configurability()
     * @throws std::runtime_error if name already exists
     */
    template <typename T>
    auto add_property(std::string_view name, T* prop) -> property& {
        using ValueT = std::remove_cvref_t<T>;
        auto type_name = properties::type_registry::get_type_name<ValueT>();

        if constexpr (properties::is_optional_v<ValueT>) {
            type_name += "?";
        }

        auto [iter, res] = m_properties.try_emplace(std::string{name}, property{type_name, prop});
        return iter->second;
    }

    /**
     * @brief Registers a structured property with nested fields
     * @tparam T The struct type
     * @tparam Func Callable type for field registration
     * @param name The property name
     * @param obj Pointer to the struct instance
     * @param register_fields Lambda/function to register nested fields (signature: void(property_set&, T*))
     * @return Reference to the property for chaining
     */
    template <typename T, typename Func>
    auto add_struct_property(std::string_view name, T* obj, Func&& register_fields) -> property& {
        auto nested_set = std::make_shared<property_set>();
        register_fields(*nested_set, obj);

        auto p = property{"struct", obj};
        p.structured(nested_set);
        p.struct_reset([](std::any& s) {
            auto* tmp = std::any_cast<T*>(s);
            *tmp = T{};
        });

        auto [iter, _] = m_properties.try_emplace(std::string{name}, std::move(p));
        return iter->second;
    }

    /**
     * @brief Registers a list property (vector of scalar values)
     * @tparam T The element type
     * @param name The property name
     * @param vec Pointer to the vector storing the list
     * @return Reference to the property for chaining
     */
    template <typename T>
    auto add_list_property(std::string_view name, std::vector<T>* vec) -> property& {
        using ValueT = std::remove_cvref_t<T>;
        auto type_name = properties::type_registry::get_type_name<ValueT>();

        auto p = property{std::format("[]{}", type_name), vec};
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

    // ========================================================================
    // Property Setting
    // ========================================================================
    //
    // Error Handling Pattern:
    // - set_properties() throws exceptions on validation failures (key_error, value_error, type_error)
    // - Individual set_*() methods return properties::error codes for programmatic error handling
    // - Getters throw exceptions on invalid access (failed pre-conditions)

    /**
     * @brief Sets multiple scalar properties from string values
     * @param values Vector of (name, value) pairs where value is a string representation
     * @param config Configurability requirement (INITIALIZE or RUNTIME)
     * @param allow_unknown_key If true, silently skip unknown property names; if false, throw
     * @throws properties::key_error if property name not found (and allow_unknown_key is false)
     * @throws properties::type_error if type conversion fails
     * @throws properties::value_error if change listener rejects the value
     * @throws properties::configurability_error if property doesn't allow runtime changes
     */
    auto set_properties(
      const std::vector<std::pair<std::string, std::string>>& values,
      properties::config_type config=properties::config_type::INITIALIZE,
      bool allow_unknown_key=false) -> void {

        for (const auto& [name, value] : values) {
            // Check if path is a struct list field access (e.g., "scaling[0].h_scale")
            if (auto struct_list_field = parse_struct_list_field_path(name)) {
                auto* list_prop = resolve_property(struct_list_field->list_name);
                if (!list_prop) {
                    if (allow_unknown_key) { continue; }
                    throw properties::key_error("", name);
                }

                validate_configurability(list_prop, name, config);

                auto res = set_struct_field(list_prop, struct_list_field->index, struct_list_field->field_name, value);
                handle_property_result(res, name, value, allow_unknown_key);
                continue;
            }

            auto* prop = resolve_property(name);
            if (prop == nullptr) {
                if (allow_unknown_key) { continue; }
                throw properties::key_error("", name);
            }

            validate_configurability(prop, name, config);

            auto type = get_base_type(prop);
            auto is_null = (value == properties::null_prop);

            // Handle structured properties
            if (type == "struct") {
                handle_struct_property(prop, name, value, is_null);
                continue;
            }

            // Parse list index if applicable
            std::string list_name;
            std::optional<std::size_t> list_idx;
            if (prop->is_list()) {
                if (const auto& p = parse_list_index_path(name)) {
                    list_name = p->base;
                    list_idx = p->index;
                }
            }

            // Dispatch based on type
            auto res = dispatch_set_property(type, prop, name, list_name, value, is_null, list_idx);

            handle_property_result(res, name, value, allow_unknown_key);
        }
    }

    auto set_properties(
      const std::vector<std::pair<std::string, std::vector<std::string>>>& values,
      properties::config_type config=properties::config_type::INITIALIZE,
      bool allow_unknown_key=false) -> void {

        for (const auto& [name, value] : values) {
            auto* prop = resolve_property(name);
            if (prop == nullptr) {
                if (allow_unknown_key) { continue; }
                throw properties::key_error("", name);
            }

            validate_configurability(prop, name, config);

            if (!prop->is_list()) {
                throw properties::type_error("", name, prop->type());
            }

            auto type = get_base_type(prop);
            auto res = dispatch_set_list_property(type, name, value);

            if (res == properties::error::INVALID_KEY && !allow_unknown_key) {
                throw properties::key_error("", name);
            } else if (res == properties::error::INVALID_VALUE) {
                auto value_str = format_list_value(value);
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
                if (allow_unknown_key) { continue; }
                throw properties::key_error("", name);
            }

            validate_configurability(prop, name, config);

            if (!prop->is_structured()) {
                throw properties::type_error("", name, prop->type());
            }

            if (prop->is_list()) {
                handle_struct_list_update(prop, name, value, allow_unknown_key);
            } else {
                handle_struct_update(prop, name, value, config, allow_unknown_key);
            }
        }
    }

    // ========================================================================
    // Individual Property Operations
    // ========================================================================

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
            return properties::error::OK;
        }

        if (prop->is_optional()) {
            return set_optional_property(prop, value, null_value);
        } else {
            return set_scalar_property(prop, value);
        }
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
        if (!prop || !prop->is_optional()) {
            return std::nullopt;
        }
        return *std::any_cast<std::optional<T>*>(prop->value());
    }

    // ========================================================================
    // List Property Operations
    // ========================================================================

    template <typename T>
    auto get_list_property(std::string_view name) const -> const std::vector<T>& {
        const auto* prop = resolve_property(name);
        if (!prop || !prop->is_list()) {
            throw properties::key_error("unknown", name);
        }
        return *std::any_cast<std::vector<T>*>(prop->value());
    }

    template <typename T>
    [[nodiscard]] auto set_list_property(std::string_view name, const std::vector<T>& value) -> properties::error {
        const auto* prop = resolve_property(name);
        if (!prop) { return properties::error::INVALID_KEY; }
        if (!prop->is_list()) { return properties::error::INVALID_TYPE; }

        auto& list = *std::any_cast<std::vector<T>*>(prop->value());
        auto old_value = list;
        list = value;

        if (!invoke_change_listener(prop->change_listener())) {
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
    [[nodiscard]] auto set_list_property_item(std::string_view name, std::size_t index, T value) -> properties::error {
        auto* prop = resolve_property(name);
        if (!prop) { return properties::error::INVALID_KEY; }
        if (!prop->is_list()) { return properties::error::INVALID_TYPE; }

        auto& list = *std::any_cast<std::vector<T>*>(prop->value());
        auto old_value = list.at(index);
        list.at(index) = value;

        if (!invoke_indexed_change_listener(prop->change_listener(), index)) {
            list.at(index) = old_value;
            return properties::error::INVALID_VALUE;
        }

        return properties::error::OK;
    }

    template <typename T>
    [[nodiscard]] auto append_list_property_item(std::string_view name, T value) -> properties::error {
        const auto* prop = resolve_property(name);
        if (!prop) { return properties::error::INVALID_KEY; }
        if (!prop->is_list()) { return properties::error::INVALID_TYPE; }

        auto& list = *std::any_cast<std::vector<T>*>(prop->value());
        auto old_value = list;
        list.push_back(value);

        if (!invoke_indexed_change_listener(prop->change_listener(), list.size() - 1)) {
            list = old_value;
            return properties::error::INVALID_VALUE;
        }

        return properties::error::OK;
    }

    template<typename T>
    [[nodiscard]] auto erase_list_property_item(std::string_view name, std::size_t index) -> properties::error {
        const auto* prop = resolve_property(name);
        if (!prop || !prop->is_list()) {
            throw properties::key_error("unknown", name);
        }

        auto& list = *std::any_cast<std::vector<T>*>(prop->value());
        if (index >= list.size()) {
            return properties::error::INVALID_KEY;
        }

        list.erase(list.begin() + index);
        invoke_indexed_change_listener(prop->change_listener(), index);

        return properties::error::OK;
    }

    // ========================================================================
    // Public Accessors
    // ========================================================================

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

    // ========================================================================
    // Type Dispatch Helpers
    // ========================================================================

    template <typename T>
    auto dispatch_scalar_operation(
        property* prop,
        std::string_view name,
        const std::string& value_str,
        bool is_null) -> properties::error {

        T value{};
        if (!is_null) {
            value = properties::convert_string_to<T>(value_str);
        }
        return set_property(name, value, is_null);
    }

    template <typename T>
    auto dispatch_list_operation(
        std::string_view list_name,
        std::optional<std::size_t> list_idx,
        const std::string& value_str,
        bool is_null) -> properties::error {

        if (list_idx.has_value()) {
            if (is_null) {
                return erase_list_property_item<T>(list_name, *list_idx);
            } else {
                T value = properties::convert_string_to<T>(value_str);
                return set_list_property_item(list_name, *list_idx, value);
            }
        } else {
            T value{};
            if (!is_null) {
                value = properties::convert_string_to<T>(value_str);
            }
            return append_list_property_item(list_name, value);
        }
    }

    auto dispatch_set_property(
        std::string_view type,
        property* prop,
        std::string_view name,
        std::string_view list_name,
        const std::string& value,
        bool is_null,
        std::optional<std::size_t> list_idx) -> properties::error {

        if (prop->is_list()) {
            // Dispatch to list operation based on type
            if (type == "bool") { return dispatch_list_operation<bool>(list_name, list_idx, value, is_null); }
            if (type == "string") { return dispatch_list_operation<std::string>(list_name, list_idx, value, is_null); }
            if (type == "int16") { return dispatch_list_operation<int16_t>(list_name, list_idx, value, is_null); }
            if (type == "uint16") { return dispatch_list_operation<uint16_t>(list_name, list_idx, value, is_null); }
            if (type == "int32") { return dispatch_list_operation<int32_t>(list_name, list_idx, value, is_null); }
            if (type == "uint32") { return dispatch_list_operation<uint32_t>(list_name, list_idx, value, is_null); }
            if (type == "int64") { return dispatch_list_operation<int64_t>(list_name, list_idx, value, is_null); }
            if (type == "uint64") { return dispatch_list_operation<uint64_t>(list_name, list_idx, value, is_null); }
            if (type == "float") { return dispatch_list_operation<float>(list_name, list_idx, value, is_null); }
            if (type == "double") { return dispatch_list_operation<double>(list_name, list_idx, value, is_null); }
        } else {
            // Dispatch to scalar operation based on type
            if (type == "bool") { return dispatch_scalar_operation<bool>(prop, name, value, is_null); }
            if (type == "string") { return dispatch_scalar_operation<std::string>(prop, name, value, is_null); }
            if (type == "int16") { return dispatch_scalar_operation<int16_t>(prop, name, value, is_null); }
            if (type == "uint16") { return dispatch_scalar_operation<uint16_t>(prop, name, value, is_null); }
            if (type == "int32") { return dispatch_scalar_operation<int32_t>(prop, name, value, is_null); }
            if (type == "uint32") { return dispatch_scalar_operation<uint32_t>(prop, name, value, is_null); }
            if (type == "int64") { return dispatch_scalar_operation<int64_t>(prop, name, value, is_null); }
            if (type == "uint64") { return dispatch_scalar_operation<uint64_t>(prop, name, value, is_null); }
            if (type == "float") { return dispatch_scalar_operation<float>(prop, name, value, is_null); }
            if (type == "double") { return dispatch_scalar_operation<double>(prop, name, value, is_null); }
        }

        throw properties::type_error("", std::string{name}, std::string{type});
    }

    auto dispatch_set_list_property(
        std::string_view type,
        std::string_view name,
        const std::vector<std::string>& values) -> properties::error {

        if (type == "bool") {
            std::vector<bool> vals;
            for (const auto& v : values) { vals.push_back(properties::convert_string_to<bool>(v)); }
            return set_list_property(name, vals);
        }
        if (type == "string") {
            return set_list_property(name, values);
        }
        if (type == "int16") {
            std::vector<int16_t> vals;
            for (const auto& v : values) { vals.push_back(properties::convert_string_to<int16_t>(v)); }
            return set_list_property(name, vals);
        }
        if (type == "uint16") {
            std::vector<uint16_t> vals;
            for (const auto& v : values) { vals.push_back(properties::convert_string_to<uint16_t>(v)); }
            return set_list_property(name, vals);
        }
        if (type == "int32") {
            std::vector<int32_t> vals;
            for (const auto& v : values) { vals.push_back(properties::convert_string_to<int32_t>(v)); }
            return set_list_property(name, vals);
        }
        if (type == "uint32") {
            std::vector<uint32_t> vals;
            for (const auto& v : values) { vals.push_back(properties::convert_string_to<uint32_t>(v)); }
            return set_list_property(name, vals);
        }
        if (type == "int64") {
            std::vector<int64_t> vals;
            for (const auto& v : values) { vals.push_back(properties::convert_string_to<int64_t>(v)); }
            return set_list_property(name, vals);
        }
        if (type == "uint64") {
            std::vector<uint64_t> vals;
            for (const auto& v : values) { vals.push_back(properties::convert_string_to<uint64_t>(v)); }
            return set_list_property(name, vals);
        }
        if (type == "float") {
            std::vector<float> vals;
            for (const auto& v : values) { vals.push_back(properties::convert_string_to<float>(v)); }
            return set_list_property(name, vals);
        }
        if (type == "double") {
            std::vector<double> vals;
            for (const auto& v : values) { vals.push_back(properties::convert_string_to<double>(v)); }
            return set_list_property(name, vals);
        }

        throw properties::type_error("", std::string{name}, std::string{type});
    }

    // ========================================================================
    // Structured Property Helpers
    // ========================================================================

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

        const auto* prop = bound->resolve_property(field);
        if (!prop) {
            return properties::error::INVALID_KEY;
        }

        auto type = get_base_type(prop);
        return dispatch_set_struct_field(*bound, field, type, value_str);
    }

    auto dispatch_set_struct_field(
        property_set& bound,
        std::string_view field,
        std::string_view type,
        const std::string& value_str) -> properties::error {

        if (type == "bool") { return bound.set_property(field, properties::convert_string_to<bool>(value_str)); }
        if (type == "string") { return bound.set_property(field, value_str); }
        if (type == "int16") { return bound.set_property(field, properties::convert_string_to<int16_t>(value_str)); }
        if (type == "uint16") { return bound.set_property(field, properties::convert_string_to<uint16_t>(value_str)); }
        if (type == "int32") { return bound.set_property(field, properties::convert_string_to<int32_t>(value_str)); }
        if (type == "uint32") { return bound.set_property(field, properties::convert_string_to<uint32_t>(value_str)); }
        if (type == "int64") { return bound.set_property(field, properties::convert_string_to<int64_t>(value_str)); }
        if (type == "uint64") { return bound.set_property(field, properties::convert_string_to<uint64_t>(value_str)); }
        if (type == "float") { return bound.set_property(field, properties::convert_string_to<float>(value_str)); }
        if (type == "double") { return bound.set_property(field, properties::convert_string_to<double>(value_str)); }

        return properties::error::INVALID_TYPE;
    }

    auto handle_struct_property(property* prop, std::string_view name, const std::string& value, bool is_null) -> void {
        if (prop->is_list()) {
            if (const auto& p = parse_list_index_path(name)) {
                if (p->index.has_value() && is_null) {
                    erase_struct_list_property_item(prop, *(p->index));
                }
            } else if (is_null) {
                // No bracket means clear the entire list
                // We need to get the vector type and clear it
                // The struct_reset operation needs special handling for lists
                clear_struct_list(prop);
            }
        } else if (is_null) {
            prop->struct_reset();
        }
    }

    auto clear_struct_list(property* prop) -> void {
        // For struct lists, we can't use struct_reset because it's not configured for lists
        // Instead, we need to manually clear the vector
        // The list size function gives us access to the underlying vector
        auto size = prop->struct_list_size();
        // Delete all items from the end to avoid index shifts
        for (size_t i = size; i > 0; --i) {
            prop->struct_erase(i - 1);
        }
    }

    auto handle_struct_list_update(
        property* prop,
        std::string_view name,
        const std::vector<std::pair<std::string, std::string>>& value,
        bool allow_unknown_key) -> void {

        const auto& p = parse_list_index_path(name);
        if (!p.has_value()) {
            throw properties::key_error("", name);
        }

        if (!p->index.has_value()) {
            // Append new struct
            if (auto idx = prop->struct_emplace_back()) {
                update_struct_fields(prop, name, *idx, value, allow_unknown_key);
                validate_indexed_change(*prop, *idx);
            } else {
                throw properties::properties_error(std::format("failed to emplace new struct for structured list property {}", p->base));
            }
        } else {
            // Update existing struct
            update_struct_fields(prop, name, *(p->index), value, allow_unknown_key);
            validate_indexed_change(*prop, *(p->index));
        }
    }

    auto handle_struct_update(
        property* prop,
        std::string_view name,
        const std::vector<std::pair<std::string, std::string>>& value,
        properties::config_type config,
        bool allow_unknown_key) -> void {

        for (const auto& [k, v] : value) {
            auto struct_prop_name = std::format("{}.{}", name, k);
            auto& struct_prop_set = prop->structured();
            auto* struct_prop = struct_prop_set.resolve_property(k);

            if (struct_prop == nullptr) {
                if (allow_unknown_key) { continue; }
                throw properties::key_error("", struct_prop_name);
            }

            validate_configurability(struct_prop, struct_prop_name, config);

            auto type = get_base_type(struct_prop);
            auto res = dispatch_set_struct_field(struct_prop_set, k, type, v);

            if (res == properties::error::INVALID_KEY && !allow_unknown_key) {
                throw properties::key_error("", struct_prop_name);
            } else if (res == properties::error::INVALID_VALUE) {
                throw properties::value_error("", struct_prop_name, v);
            }
        }
    }

    auto update_struct_fields(
        property* prop,
        std::string_view name,
        std::size_t idx,
        const std::vector<std::pair<std::string, std::string>>& fields,
        bool allow_unknown_key) -> void {

        for (const auto& [k, v] : fields) {
            auto struct_prop_name = std::format("{}.{}", name, k);
            auto res = set_struct_field(prop, idx, k, v);

            if (res == properties::error::INVALID_KEY && !allow_unknown_key) {
                throw properties::key_error("", struct_prop_name);
            } else if (res == properties::error::INVALID_VALUE) {
                throw properties::value_error("", struct_prop_name, v);
            }
        }
    }

    auto erase_struct_list_property_item(property* prop, size_t index) -> properties::error {
        if (!prop->is_structured()) {
            return properties::error::INVALID_TYPE;
        }

        auto size = prop->struct_list_size();
        if (index >= size) {
            return properties::error::INVALID_KEY;
        }

        prop->struct_erase(index);
        invoke_indexed_change_listener(prop->change_listener(), index);

        return properties::error::OK;
    }

    // ========================================================================
    // Validation and Helper Methods
    // ========================================================================

    auto validate_configurability(
        const property* prop,
        std::string_view name,
        properties::config_type config) -> void {

        using enum properties::config_type;
        if ((config == RUNTIME) && (prop->configurability() == INITIALIZE)) {
            throw properties::configurability_error("", name);
        }
    }

    auto get_base_type(const property* prop) -> std::string {
        auto type = prop->type();
        if (prop->is_optional()) {
            type.pop_back(); // remove ?
        }
        if (prop->is_list()) {
            type = type.substr(2); // remove []
        }
        return type;
    }

    auto handle_property_result(
        properties::error res,
        std::string_view name,
        const std::string& value,
        bool allow_unknown_key) -> void {

        if (res == properties::error::INVALID_KEY && !allow_unknown_key) {
            throw properties::key_error("", name);
        } else if (res == properties::error::INVALID_VALUE) {
            throw properties::value_error("", name, value);
        }
    }

    auto format_list_value(const std::vector<std::string>& values) -> std::string {
        auto value_str = std::string{"["};
        value_str = std::accumulate(
            values.begin(),
            values.end(),
            value_str,
            [](const std::string& acc, const std::string& val) {
                return acc + val + ", ";
            }
        );
        if (value_str.size() > 1) {
            value_str.pop_back(); // remove space
            value_str.back() = ']';
        } else {
            value_str += "]";
        }
        return value_str;
    }

    template <typename T>
    auto set_optional_property(property* prop, T value, bool null_value) -> properties::error {
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

        if (!invoke_change_listener(prop->change_listener())) {
            *val_ptr = prev_value;
            return properties::error::INVALID_VALUE;
        }

        return properties::error::OK;
    }

    template <typename T>
    auto set_scalar_property(property* prop, T value) -> properties::error {
        auto val_ptr = *std::any_cast<T*>(&(prop->value()));
        auto prev_value = *val_ptr;
        *val_ptr = value;

        if (!invoke_change_listener(prop->change_listener())) {
            *val_ptr = prev_value;
            return properties::error::INVALID_VALUE;
        }

        return properties::error::OK;
    }

    auto invoke_change_listener(const property::any_change_listener& listener) -> bool {
        if (std::holds_alternative<change_func_type>(listener)) {
            return std::get<change_func_type>(listener)();
        }
        return true;
    }

    auto invoke_indexed_change_listener(const property::any_change_listener& listener, std::size_t index) -> bool {
        if (std::holds_alternative<indexed_change_func_type>(listener)) {
            return std::get<indexed_change_func_type>(listener)(index);
        }
        return true;
    }

    auto validate_indexed_change(property& prop, std::size_t index) -> void {
        if (!invoke_indexed_change_listener(prop.change_listener(), index)) {
            // Rollback would be complex here, so we log the issue
            // In production, you might want to handle this differently
        }
    }

    // ========================================================================
    // Path Resolution
    // ========================================================================

    [[nodiscard]] auto resolve_property(std::string_view path) -> property* {
        auto dot = path.find('.');
        auto bracket = path.find('[');

        // Simple case: no dots or brackets
        if (dot == std::string_view::npos && bracket == std::string_view::npos) {
            auto it = m_properties.find(std::string{path});
            return it != m_properties.end() ? &it->second : nullptr;
        }

        // Handle list indexing
        if (bracket != std::string_view::npos && (dot == std::string_view::npos || (bracket < dot))) {
            return resolve_list_property(path, bracket);
        }

        // Handle structured property
        return resolve_structured_property(path, dot);
    }

    [[nodiscard]] auto resolve_property(std::string_view path) const -> const property* {
        return const_cast<property_set*>(this)->resolve_property(path);
    }

    auto resolve_list_property(std::string_view path, std::size_t bracket) -> property* {
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
            // Check if there's a tail after the bracket (e.g., "scaling[0].h_scale")
            auto tail = extract_tail_after_bracket(path, end_bracket);
            if (!tail.empty()) {
                // For structured lists with a tail, resolve the field within the schema
                return it->second.structured().resolve_property(tail);
            }
            // No tail means we want the list property itself (e.g., "scaling[0]")
            return &it->second;
        } else if (path.find('.') == std::string_view::npos) {
            return &it->second;
        }

        return nullptr;
    }

    auto resolve_structured_property(std::string_view path, std::size_t dot) -> property* {
        auto head = path.substr(0, dot);
        auto tail = path.substr(dot + 1);
        auto it = m_properties.find(std::string{head});

        if (it == m_properties.end() || !it->second.is_structured()) {
            return nullptr;
        }

        return it->second.structured().resolve_property(tail);
    }

    auto extract_tail_after_bracket(std::string_view path, std::size_t end_bracket) -> std::string_view {
        auto tail_start = end_bracket + 1;
        while (tail_start < path.size() && path[tail_start] == '.') {
            ++tail_start; // skip dots
        }
        if (tail_start >= path.size()) {
            return {};
        }
        return path.substr(tail_start);
    }

    struct list_path_info {
        std::string_view base;
        std::optional<size_t> index;
    };

    struct struct_list_field_info {
        std::string list_name;
        std::size_t index;
        std::string field_name;
    };

    auto parse_list_index_path(std::string_view name) -> std::optional<list_path_info> {
        auto bracket_pos = name.find('[');
        if (bracket_pos == std::string_view::npos) {
            return std::nullopt;
        }

        auto end_bracket = name.find(']', bracket_pos);
        if (end_bracket == std::string_view::npos) {
            return std::nullopt;
        }

        auto base = name.substr(0, bracket_pos);
        auto content = name.substr(bracket_pos + 1, end_bracket - bracket_pos - 1);

        if (content.empty()) {
            return list_path_info{base, std::nullopt}; // append
        }

        try {
            auto idx = std::stoul(std::string{content});
            return list_path_info{base, idx}; // update
        } catch (...) {
            return std::nullopt;
        }
    }

    auto parse_struct_list_field_path(std::string_view name) -> std::optional<struct_list_field_info> {
        // Look for pattern: list_name[index].field_name
        auto bracket_pos = name.find('[');
        if (bracket_pos == std::string_view::npos) {
            return std::nullopt;
        }

        auto end_bracket = name.find(']', bracket_pos);
        if (end_bracket == std::string_view::npos) {
            return std::nullopt;
        }

        // Check if there's a dot after the bracket (indicating a field access)
        if (end_bracket + 1 >= name.size() || name[end_bracket + 1] != '.') {
            return std::nullopt;
        }

        auto list_name = name.substr(0, bracket_pos);
        auto index_str = name.substr(bracket_pos + 1, end_bracket - bracket_pos - 1);
        auto field_name = name.substr(end_bracket + 2); // skip "]."

        if (index_str.empty() || field_name.empty()) {
            return std::nullopt;
        }

        try {
            auto idx = std::stoul(std::string{index_str});
            return struct_list_field_info{std::string{list_name}, idx, std::string{field_name}};
        } catch (...) {
            return std::nullopt;
        }
    }

}; // class property_set

} // namespace composite
