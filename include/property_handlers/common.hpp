/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "composite/properties/errors.hpp"
#include "composite/properties/json_convert.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <string>
#include <functional>

namespace composite::property_handlers {

// Re-export json_to_string for convenience
using properties::json_to_string;

// ============================================================================
// Response Helpers
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

/// Extract "value" from body if present, otherwise use body as-is
inline auto extract_value(const nlohmann::json& body) -> nlohmann::json {
    return body.contains("value") ? body["value"] : body;
}

// ============================================================================
// Exception-to-HTTP Handler Wrapper
// ============================================================================

/**
 * @brief Execute a handler function and map property exceptions to HTTP responses
 *
 * Usage:
 *   handle_exceptions(res, [&] {
 *       // handler code that may throw property exceptions
 *   });
 */
template<typename Func>
inline auto handle_exceptions(httplib::Response& res, Func&& func) -> void {
    try {
        func();
    } catch (const properties::config_error& ex) {
        error(res, ex.what(), 403);
    } catch (const properties::key_error& ex) {
        error(res, ex.what(), 404);
    } catch (const properties::index_error& ex) {
        error(res, ex.what(), 400);
    } catch (const properties::value_error& ex) {
        error(res, ex.what(), 400);
    } catch (const properties::listener_rejected& ex) {
        error(res, ex.what(), 400);
    } catch (const std::exception& ex) {
        error(res, ex.what(), 500);
    }
}

} // namespace composite::property_handlers
