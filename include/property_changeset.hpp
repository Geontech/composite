/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "composite/properties/json_convert.hpp"

#include <format>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace composite {

using properties::json_to_string;

/**
 * @brief Parses JSON property updates into path-based key-value pairs
 *
 * Flattens nested JSON into path notation compatible with the unified set() API:
 * - Scalar: {"rate": 48000} -> [("rate", "48000")]
 * - Nested: {"net": {"host": "localhost"}} -> [("net.host", "localhost")]
 * - Array: {"list": ["a", "b"]} -> [("list", "null"), ("list[]", "a"), ("list[]", "b")]
 * - Struct array: {"items": [{"x": 1}]} -> [("items", "null"), ("items[]", ""), ("items[0].x", "1")]
 */
class property_changeset {
public:
    using props_type = std::vector<std::pair<std::string, std::string>>;

    /**
     * @brief Parse JSON into a flat list of path-value pairs
     */
    static auto from_json(const nlohmann::json& properties) -> property_changeset {
        auto result = property_changeset{};

        for (const auto& [key, value] : properties.items()) {
            if (key == "enabled") {
                continue;  // Handled separately
            }
            result.parse_value("", key, value);
        }

        return result;
    }

    [[nodiscard]] auto has_updates() const noexcept -> bool {
        return !m_props.empty();
    }

    /**
     * @brief Get all property updates as path-value pairs
     *
     * This unified format works directly with component::set_properties()
     */
    [[nodiscard]] auto properties() const noexcept -> const props_type& {
        return m_props;
    }

    // Legacy accessors for backwards compatibility during transition
    [[nodiscard]] auto scalar_properties() const noexcept -> const props_type& {
        return m_props;
    }

private:
    props_type m_props;

    auto parse_value(const std::string& prefix, const std::string& key, const nlohmann::json& value) -> void {
        auto path = prefix.empty() ? key : std::format("{}.{}", prefix, key);

        if (value.is_null()) {
            m_props.emplace_back(path, std::string{properties::null_value});
        } else if (value.is_array()) {
            parse_array(path, value);
        } else if (value.is_object()) {
            parse_object(path, value);
        } else {
            m_props.emplace_back(path, json_to_string(value));
        }
    }

    auto parse_array(const std::string& path, const nlohmann::json& arr) -> void {
        // Clear existing array first
        m_props.emplace_back(path, std::string{properties::null_value});

        std::size_t index = 0;
        for (const auto& elem : arr) {
            if (elem.is_object()) {
                // Struct array: append element, then set its fields
                m_props.emplace_back(std::format("{}[]", path), "");
                for (const auto& [k, v] : elem.items()) {
                    auto field_path = std::format("{}[{}].{}", path, index, k);
                    if (v.is_object()) {
                        parse_nested_object(field_path, v);
                    } else {
                        m_props.emplace_back(field_path, json_to_string(v));
                    }
                }
                ++index;
            } else {
                // Scalar array
                m_props.emplace_back(std::format("{}[]", path), json_to_string(elem));
            }
        }
    }

    auto parse_object(const std::string& path, const nlohmann::json& obj) -> void {
        for (const auto& [k, v] : obj.items()) {
            parse_value(path, k, v);
        }
    }

    auto parse_nested_object(const std::string& prefix, const nlohmann::json& obj) -> void {
        for (const auto& [k, v] : obj.items()) {
            auto field_path = std::format("{}.{}", prefix, k);
            if (v.is_object()) {
                parse_nested_object(field_path, v);
            } else {
                m_props.emplace_back(field_path, json_to_string(v));
            }
        }
    }

}; // class property_changeset

} // namespace composite
