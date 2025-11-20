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

#include "composite/properties/property_set.hpp"

#include <format>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace composite {

class property_changeset {
public:
    using scalar_props_type = std::vector<std::pair<std::string, std::string>>;
    using list_props_type = std::vector<std::pair<std::string, std::vector<std::string>>>;
    using struct_props_type = std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>;

    // Parse JSON into a changeset
    static auto from_json(const nlohmann::json& properties) -> property_changeset {
        auto result = property_changeset{};

        for (const auto& [key, value] : properties.items()) {
            if (key == "enabled") {
                // Skip enabled flag - handled separately
                continue;
            } else if (value.is_array()) {
                parse_array_property(result, key, value);
            } else if (value.is_object()) {
                parse_object_property(result, key, value);
            } else {
                parse_scalar_property(result, key, value);
            }
        }

        return result;
    }

    // Check if changeset has any updates
    auto has_updates() const -> bool {
        return !m_scalar_props.empty() || !m_list_props.empty() || !m_struct_props.empty();
    }

    // Get scalar properties
    auto scalar_properties() const -> const scalar_props_type& { return m_scalar_props; }

    // Get list properties
    auto list_properties() const -> const list_props_type& { return m_list_props; }

    // Get struct properties
    auto struct_properties() const -> const struct_props_type& { return m_struct_props; }

    // Add scalar property
    auto add_scalar(std::string_view key, std::string_view value) -> void {
        m_scalar_props.emplace_back(key, value);
    }

    // Add list property
    auto add_list(std::string_view key, std::vector<std::string> values) -> void {
        m_list_props.emplace_back(key, std::move(values));
    }

    // Add struct property
    auto add_struct(std::string_view key, std::vector<std::pair<std::string, std::string>> fields) -> void {
        m_struct_props.emplace_back(key, std::move(fields));
    }

private:
    scalar_props_type m_scalar_props;
    list_props_type m_list_props;
    struct_props_type m_struct_props;

    static auto parse_array_property(property_changeset& changeset, const std::string& key, const nlohmann::json& value) -> void {
        auto obj_scalar_props = std::vector<std::string>{};

        for (const auto& [k, v] : value.items()) {
            if (v.is_object()) {
                // Struct list element
                auto obj_props = std::vector<std::pair<std::string, std::string>>{};
                for (const auto& [ki, vi] : v.items()) {
                    obj_props.emplace_back(ki, vi);
                }
                changeset.m_scalar_props.emplace_back(key, properties::null_prop);
                changeset.m_struct_props.emplace_back(std::format("{}[]", key), std::move(obj_props));
            } else {
                // Scalar list element
                obj_scalar_props.emplace_back(v.get<std::string>());
            }
        }

        if (!obj_scalar_props.empty()) {
            changeset.m_list_props.emplace_back(key, std::move(obj_scalar_props));
        }
    }

    static auto parse_object_property(property_changeset& changeset, const std::string& key, const nlohmann::json& value) -> void {
        auto obj_props = std::vector<std::pair<std::string, std::string>>{};
        parse_object_fields(obj_props, "", value);
        changeset.m_struct_props.emplace_back(key, std::move(obj_props));
    }

    // Recursively flatten nested objects with dot notation
    static auto parse_object_fields(std::vector<std::pair<std::string, std::string>>& props,
                                    const std::string& prefix,
                                    const nlohmann::json& obj) -> void {
        for (const auto& [k, v] : obj.items()) {
            std::string field_key = prefix.empty() ? k : std::format("{}.{}", prefix, k);

            if (v.is_object()) {
                // Recursively flatten nested objects
                parse_object_fields(props, field_key, v);
            } else {
                // Use common scalar conversion logic
                props.emplace_back(field_key, json_to_string(v));
            }
        }
    }

    static auto parse_scalar_property(property_changeset& changeset, const std::string& key, const nlohmann::json& value) -> void {
        changeset.m_scalar_props.emplace_back(key, json_to_string(value));
    }

    static auto json_to_string(const nlohmann::json& value) -> std::string {
        if (value.is_null() || value.empty()) {
            return std::string{properties::null_prop};
        }
        return value.get<std::string>();
    }

}; // class property_changeset

} // namespace composite
