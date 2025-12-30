/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "property_set.hpp"
#include <nlohmann/json.hpp>

namespace composite::properties {

/**
 * @brief JSON serialization utilities for the property system
 */
class property_serializer {
public:
    /**
     * @brief Serialize a property to JSON
     */
    static auto to_json(const property& prop, std::string_view name) -> nlohmann::json {
        auto json_obj = nlohmann::json::object();
        json_obj["name"] = name;
        json_obj["type"] = prop.type_name();
        json_obj["units"] = prop.units();
        json_obj["configurability"] = (prop.configurability() == config_type::RUNTIME)
                                      ? "runtime" : "initialize";
        json_obj["value"] = value_to_json(prop);
        return json_obj;
    }

    /**
     * @brief Serialize a property_set to JSON
     */
    static auto to_json(const property_set& set) -> nlohmann::json {
        auto json_obj = nlohmann::json::object();
        for (const auto& [name, prop] : set.properties()) {
            json_obj[name] = to_json(prop, name);
        }
        return json_obj;
    }

    /**
     * @brief Serialize only property values (not metadata) to JSON
     */
    static auto values_to_json(const property_set& set) -> nlohmann::json {
        auto json_obj = nlohmann::json::object();
        for (const auto& [name, prop] : set.properties()) {
            json_obj[name] = value_to_json(prop);
        }
        return json_obj;
    }

private:
    /**
     * @brief Serialize just the value of a property
     */
    static auto value_to_json(const property& prop) -> nlohmann::json {
        if (prop.is_scalar()) {
            return visit_scalar(prop.value(), [](const auto* ptr) -> nlohmann::json {
                return to_string(*ptr);
            });
        }
        if (prop.is_optional()) {
            return visit_optional(prop.value(), [](const auto* ptr) -> nlohmann::json {
                return ptr->has_value() ? nlohmann::json(to_string(ptr->value())) : nullptr;
            });
        }
        if (prop.is_list() && !prop.is_struct_list()) {
            return visit_list(prop.value(), [](const auto* ptr) -> nlohmann::json {
                auto arr = nlohmann::json::array();
                for (const auto& item : *ptr) {
                    arr.push_back(to_string(item));
                }
                return arr;
            });
        }
        if (prop.is_struct()) {
            return serialize_struct(prop);
        }
        if (prop.is_struct_list()) {
            return serialize_struct_list(prop);
        }
        return nullptr;
    }

    /**
     * @brief Serialize a struct property's nested fields
     */
    static auto serialize_struct(const property& prop) -> nlohmann::json {
        auto* nested = prop.nested();
        if (!nested) return nlohmann::json::object();

        auto json_obj = nlohmann::json::object();
        for (const auto& [field_name, field_prop] : nested->properties()) {
            json_obj[field_name] = value_to_json(field_prop);
        }
        return json_obj;
    }

    /**
     * @brief Serialize a struct-list property
     */
    static auto serialize_struct_list(const property& prop) -> nlohmann::json {
        auto json_arr = nlohmann::json::array();

        const auto& acc = get_struct_accessor(prop.value());
        auto count = acc.size(acc.data);

        for (std::size_t i = 0; i < count; ++i) {
            auto element_props = property_set{};
            // Use const accessor for read-only access, cast for register_fields which
            // creates pointers but doesn't modify data during serialization
            const void* element_ptr = acc.get_element_const(acc.data, i);
            acc.register_fields(element_props, const_cast<void*>(element_ptr));

            auto item_json = nlohmann::json::object();
            for (const auto& [field_name, field_prop] : element_props.properties()) {
                item_json[field_name] = value_to_json(field_prop);
            }
            json_arr.push_back(item_json);
        }

        return json_arr;
    }
};

} // namespace composite::properties
