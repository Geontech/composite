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
// GET /components/:id/properties
// ============================================================================

inline auto get_properties(component* comp, httplib::Response& res) -> void {
    json_ok(res, properties::property_serializer::to_json(comp->property_set()));
}

// ============================================================================
// GET /components/:id/properties/:name
// ============================================================================

inline auto get_property(
    component* comp,
    std::string_view name,
    httplib::Response& res
) -> void {
    const auto& props = comp->properties();
    auto it = props.find(std::string{name});

    if (it == props.end()) {
        return error(res, std::format("property '{}' not found", name), 404);
    }

    json_ok(res, properties::property_serializer::to_json(it->second, name));
}

// ============================================================================
// PUT /components/:id/properties/:name
// ============================================================================

inline auto put_property(
    component* comp,
    std::string_view name,
    const nlohmann::json& body,
    httplib::Response& res
) -> void {
    handle_exceptions(res, [&] {
        auto value = extract_value(body);
        comp->set_properties(
            {{std::string{name}, json_to_string(value)}},
            properties::config_type::RUNTIME
        );
        comp->property_change_handler();
        get_property(comp, name, res);
    });
}

// ============================================================================
// PATCH /components/:id/properties/:name
// ============================================================================

inline auto patch_property(
    component* comp,
    std::string_view name,
    const nlohmann::json& body,
    httplib::Response& res
) -> void {
    put_property(comp, name, body, res);
}

// ============================================================================
// DELETE /components/:id/properties/:name
// ============================================================================

inline auto delete_property(
    component* comp,
    std::string_view name,
    httplib::Response& res
) -> void {
    handle_exceptions(res, [&] {
        comp->set_properties(
            {{std::string{name}, std::string{properties::null_value}}},
            properties::config_type::RUNTIME
        );
        comp->property_change_handler();
        json_ok(res, {{"success", std::format("property '{}' reset", name)}});
    });
}

} // namespace composite::property_handlers
