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
// GET /components/:id/properties/:name/fields
// ============================================================================

inline auto get_struct_fields(
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
    if (!prop.is_struct()) {
        return error(res, std::format("property '{}' is not a struct", name), 400);
    }

    auto* nested = prop.nested();
    if (!nested) {
        return error(res, "struct has no nested properties", 500);
    }

    auto response = nlohmann::json::object();
    response["name"] = name;
    response["type"] = prop.type_name();

    auto fields = nlohmann::json::object();
    for (const auto& [field_name, field_prop] : nested->properties()) {
        auto field_json = properties::property_serializer::to_json(field_prop, field_name);
        fields[field_name] = {
            {"href", std::format("{}/{}", base_href, field_name)},
            {"type", field_prop.type_name()},
            {"value", field_json.contains("value") ? field_json["value"] : nullptr}
        };
    }

    response["fields"] = fields;
    response["count"] = nested->properties().size();
    json_ok(res, response);
}

// ============================================================================
// GET /components/:id/properties/:name/fields/:field
// ============================================================================

inline auto get_struct_field(
    component* comp,
    std::string_view name,
    std::string_view field,
    httplib::Response& res
) -> void {
    const auto& props = comp->properties();
    auto it = props.find(std::string{name});

    if (it == props.end()) {
        return error(res, std::format("property '{}' not found", name), 404);
    }

    const auto& prop = it->second;
    if (!prop.is_struct()) {
        return error(res, std::format("property '{}' is not a struct", name), 400);
    }

    auto* nested = prop.nested();
    if (!nested) {
        return error(res, "struct has no nested properties", 500);
    }

    const auto& field_props = nested->properties();
    auto field_it = field_props.find(std::string{field});

    if (field_it == field_props.end()) {
        return error(res, std::format("field '{}' not found in '{}'", field, name), 404);
    }

    json_ok(res, properties::property_serializer::to_json(field_it->second, field));
}

// ============================================================================
// PATCH /components/:id/properties/:name/fields/:field
// ============================================================================

inline auto patch_struct_field(
    component* comp,
    std::string_view name,
    std::string_view field,
    const nlohmann::json& body,
    httplib::Response& res
) -> void {
    handle_exceptions(res, [&] {
        auto value = extract_value(body);
        auto path = std::format("{}.{}", name, field);
        comp->set_properties({{path, json_to_string(value)}}, properties::config_type::RUNTIME);
        comp->property_change_handler();
        get_struct_field(comp, name, field, res);
    });
}

// ============================================================================
// GET /components/:id/properties/:name/items/:index/fields
// ============================================================================

inline auto get_struct_list_item_fields(
    component* comp,
    std::string_view name,
    std::size_t index,
    std::string_view base_href,
    httplib::Response& res
) -> void {
    const auto& props = comp->properties();
    auto it = props.find(std::string{name});

    if (it == props.end()) {
        return error(res, std::format("property '{}' not found", name), 404);
    }

    const auto& prop = it->second;
    if (!prop.is_struct_list()) {
        return error(res, std::format("property '{}' is not a struct list", name), 400);
    }

    auto prop_json = properties::property_serializer::to_json(prop, name);
    if (!prop_json.contains("value") || !prop_json["value"].is_array()) {
        return error(res, "failed to serialize struct list", 500);
    }

    const auto& arr = prop_json["value"];
    if (index >= arr.size()) {
        return error(res, std::format("index {} out of bounds (size {})", index, arr.size()), 400);
    }

    const auto& item = arr[index];
    if (!item.is_object()) {
        return error(res, "struct list item is not an object", 500);
    }

    auto response = nlohmann::json::object();
    response["name"] = name;
    response["index"] = index;
    response["type"] = "struct";

    auto fields = nlohmann::json::object();
    for (const auto& [field_name, field_value] : item.items()) {
        fields[field_name] = {
            {"href", std::format("{}/{}", base_href, field_name)},
            {"value", field_value}
        };
    }

    response["fields"] = fields;
    response["count"] = item.size();
    json_ok(res, response);
}

// ============================================================================
// PATCH /components/:id/properties/:name/items/:index/fields/:field
// ============================================================================

inline auto patch_struct_list_item_field(
    component* comp,
    std::string_view name,
    std::size_t index,
    std::string_view field,
    const nlohmann::json& body,
    httplib::Response& res
) -> void {
    handle_exceptions(res, [&] {
        auto value = extract_value(body);
        auto path = std::format("{}[{}].{}", name, index, field);
        comp->set_properties({{path, json_to_string(value)}}, properties::config_type::RUNTIME);
        comp->property_change_handler();
        json_ok(res, {{"field", field}, {"index", index}, {"value", value}});
    });
}

} // namespace composite::property_handlers
