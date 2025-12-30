/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "common.hpp"
#include "composite/core/component.hpp"
#include "composite/properties/serialization.hpp"

#include <format>

namespace composite::property_handlers {

// ============================================================================
// GET /components/:id/properties/:name/items
// ============================================================================

inline auto get_list_items(
    component* comp,
    std::string_view name,
    std::string_view base_href,
    httplib::Response& res
) -> void {
    const auto& props = comp->properties();
    auto it = props.find(std::string{name});

    if (it == props.end()) {
        return error(res, std::format("property '{}' not found", name), 404);
    }

    const auto& prop = it->second;
    if (!prop.is_list() && !prop.is_struct_list()) {
        return error(res, std::format("property '{}' is not a list", name), 400);
    }

    auto prop_json = properties::property_serializer::to_json(prop, name);
    auto response = nlohmann::json::object();
    response["name"] = name;
    response["type"] = prop.type_name();

    auto items = nlohmann::json::array();
    if (prop_json.contains("value") && prop_json["value"].is_array()) {
        std::size_t idx = 0;
        for (const auto& val : prop_json["value"]) {
            items.push_back({
                {"index", idx},
                {"href", std::format("{}/{}", base_href, idx)},
                {"value", val}
            });
            ++idx;
        }
    }

    response["items"] = items;
    response["count"] = items.size();
    json_ok(res, response);
}

// ============================================================================
// GET /components/:id/properties/:name/items/:index
// ============================================================================

inline auto get_list_item(
    component* comp,
    std::string_view name,
    std::size_t index,
    httplib::Response& res
) -> void {
    const auto& props = comp->properties();
    auto it = props.find(std::string{name});

    if (it == props.end()) {
        return error(res, std::format("property '{}' not found", name), 404);
    }

    const auto& prop = it->second;
    if (!prop.is_list() && !prop.is_struct_list()) {
        return error(res, std::format("property '{}' is not a list", name), 400);
    }

    auto prop_json = properties::property_serializer::to_json(prop, name);
    if (!prop_json.contains("value") || !prop_json["value"].is_array()) {
        return error(res, "failed to serialize list", 500);
    }

    const auto& arr = prop_json["value"];
    if (index >= arr.size()) {
        return error(res, std::format("index {} out of bounds (size {})", index, arr.size()), 400);
    }

    json_ok(res, {{"index", index}, {"value", arr[index]}});
}

// ============================================================================
// POST /components/:id/properties/:name/items
// ============================================================================

inline auto post_list_item(
    component* comp,
    std::string_view name,
    const nlohmann::json& body,
    httplib::Response& res
) -> void {
    handle_exceptions(res, [&] {
        auto value = extract_value(body);
        auto path = std::format("{}[]", name);

        if (value.is_object()) {
            std::vector<std::pair<std::string, std::string>> fields;
            fields.reserve(value.size());
            for (const auto& [field, val] : value.items()) {
                fields.emplace_back(field, json_to_string(val));
            }
            auto index = comp->append_struct_list(name, fields, properties::config_type::RUNTIME);
            (void)index;
        } else {
            comp->set_properties({{path, json_to_string(value)}}, properties::config_type::RUNTIME);
        }

        comp->property_change_handler();
        json_created(res, {{"success", std::format("item appended to '{}'", name)}});
    });
}

// ============================================================================
// PUT /components/:id/properties/:name/items/:index
// ============================================================================

inline auto put_list_item(
    component* comp,
    std::string_view name,
    std::size_t index,
    const nlohmann::json& body,
    httplib::Response& res
) -> void {
    handle_exceptions(res, [&] {
        auto value = extract_value(body);
        auto path = std::format("{}[{}]", name, index);

        if (value.is_object()) {
            std::vector<std::pair<std::string, std::string>> fields;
            fields.reserve(value.size());
            for (const auto& [field, val] : value.items()) {
                fields.emplace_back(field, json_to_string(val));
            }
            comp->update_struct_list_element(name, index, fields, properties::config_type::RUNTIME);
        } else {
            comp->set_properties({{path, json_to_string(value)}}, properties::config_type::RUNTIME);
        }

        comp->property_change_handler();
        get_list_item(comp, name, index, res);
    });
}

// ============================================================================
// DELETE /components/:id/properties/:name/items/:index
// ============================================================================

inline auto delete_list_item(
    component* comp,
    std::string_view name,
    std::size_t index,
    httplib::Response& res
) -> void {
    handle_exceptions(res, [&] {
        auto path = std::format("{}[{}]", name, index);
        comp->set_properties(
            {{path, std::string{properties::null_value}}},
            properties::config_type::RUNTIME
        );
        comp->property_change_handler();
        json_ok(res, {{"success", std::format("item {} removed from '{}'", index, name)}});
    });
}

} // namespace composite::property_handlers
