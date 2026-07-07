/*
 * Copyright (C) 2024-2026 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace composite::property_handlers {

// ============================================================================
// REST response helpers (CORS + JSON body + status)
// ============================================================================

inline auto set_cors(httplib::Response& res) -> void {
    res.set_header("Access-Control-Allow-Origin", "*");
}

inline auto json_ok(httplib::Response& res, const nlohmann::json& data) -> void {
    set_cors(res);
    res.set_content(data.dump(2), "application/json");
    res.status = httplib::OK_200;
}

inline auto json_created(httplib::Response& res, const nlohmann::json& data) -> void {
    set_cors(res);
    res.set_content(data.dump(2), "application/json");
    res.status = httplib::Created_201;
}

inline auto error(httplib::Response& res, std::string_view message, int status) -> void {
    set_cors(res);
    res.set_content(nlohmann::json{{"error", message}}.dump(), "application/json");
    res.status = status;
}

/// Extract "value" from a request body if wrapped ({"value": ...}), else use the body as-is.
inline auto extract_value(const nlohmann::json& body) -> nlohmann::json {
    return body.contains("value") ? body["value"] : body;
}

} // namespace composite::property_handlers
