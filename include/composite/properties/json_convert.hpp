/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "types.hpp"

#include <nlohmann/json.hpp>
#include <string>

namespace composite::properties {

/**
 * @brief Convert a JSON value to its string representation for the property system
 *
 * Used for setting property values from JSON input (REST API, config files, etc.)
 */
inline auto json_to_string(const nlohmann::json& value) -> std::string {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number_integer()) return std::to_string(value.get<int64_t>());
    if (value.is_number_float()) return std::to_string(value.get<double>());
    if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
    if (value.is_null()) return std::string{null_value};
    return value.dump();
}

} // namespace composite::properties
