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

#include "composite/component.hpp"
#include "composite/property_set.hpp"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace composite::properties::rest {

enum class error_code {
    PROPERTY_NOT_FOUND,
    COMPONENT_NOT_FOUND,
    INVALID_TYPE,
    OUT_OF_RANGE,
    NOT_RUNTIME_CONFIGURABLE,
    INVALID_INDEX,
    INVALID_VALUE,
    PARSE_ERROR,
    VALIDATION_FAILED,
    MISSING_FIELD,
    UNSUPPORTED_OPERATION
};

struct error_response {
    error_code code;
    std::string message;
    nlohmann::json details;
    std::string path;

    auto to_json() const -> nlohmann::json {
        auto error_obj = nlohmann::json::object();
        error_obj["code"] = error_code_to_string(code);
        error_obj["message"] = message;
        if (!details.empty()) {
            error_obj["details"] = details;
        }
        error_obj["path"] = path;
        return nlohmann::json{{"error", error_obj}};
    }

    static auto error_code_to_string(error_code code) -> std::string {
        switch (code) {
            case error_code::PROPERTY_NOT_FOUND: return "PROPERTY_NOT_FOUND";
            case error_code::COMPONENT_NOT_FOUND: return "COMPONENT_NOT_FOUND";
            case error_code::INVALID_TYPE: return "INVALID_TYPE";
            case error_code::OUT_OF_RANGE: return "OUT_OF_RANGE";
            case error_code::NOT_RUNTIME_CONFIGURABLE: return "NOT_RUNTIME_CONFIGURABLE";
            case error_code::INVALID_INDEX: return "INVALID_INDEX";
            case error_code::INVALID_VALUE: return "INVALID_VALUE";
            case error_code::PARSE_ERROR: return "PARSE_ERROR";
            case error_code::VALIDATION_FAILED: return "VALIDATION_FAILED";
            case error_code::MISSING_FIELD: return "MISSING_FIELD";
            case error_code::UNSUPPORTED_OPERATION: return "UNSUPPORTED_OPERATION";
        }
        return "UNKNOWN_ERROR";
    }

    static auto http_status(error_code code) -> int {
        switch (code) {
            case error_code::PROPERTY_NOT_FOUND:
            case error_code::COMPONENT_NOT_FOUND:
                return httplib::NotFound_404;
            case error_code::INVALID_TYPE:
            case error_code::OUT_OF_RANGE:
            case error_code::INVALID_INDEX:
            case error_code::INVALID_VALUE:
            case error_code::PARSE_ERROR:
            case error_code::VALIDATION_FAILED:
            case error_code::MISSING_FIELD:
                return httplib::BadRequest_400;
            case error_code::NOT_RUNTIME_CONFIGURABLE:
                return httplib::Forbidden_403;
            case error_code::UNSUPPORTED_OPERATION:
                return httplib::MethodNotAllowed_405;
        }
        return httplib::InternalServerError_500;
    }
}; // struct error_response

// ============================================================================
// Request/Response Helpers
// ============================================================================

class rest_helpers {
public:
    // Build success response with data
    static auto success_response(const nlohmann::json& data) -> httplib::Response {
        auto res = httplib::Response{};
        res.set_content(data.dump(2), "application/json");
        res.status = httplib::OK_200;
        set_cors_header(res);
        return res;
    }

    // Build error response
    static auto error_response_obj(
        error_code code,
        std::string_view message,
        std::string_view path,
        const nlohmann::json& details = nlohmann::json::object()
    ) -> httplib::Response {
        auto err = error_response{code, std::string{message}, details, std::string{path}};
        auto res = httplib::Response{};
        res.set_content(err.to_json().dump(2), "application/json");
        res.status = error_response::http_status(code);
        set_cors_header(res);
        return res;
    }

    // Parse JSON body
    static auto parse_json_body(const std::string& body, std::string_view path)
        -> std::pair<nlohmann::json, std::optional<httplib::Response>> {
        try {
            auto json = nlohmann::json::parse(body);
            return {json, std::nullopt};
        } catch (const std::exception& ex) {
            auto err_details = nlohmann::json::object();
            err_details["parse_error"] = ex.what();
            return {
                nlohmann::json{},
                error_response_obj(error_code::PARSE_ERROR, "Invalid JSON in request body", path, err_details)
            };
        }
    }

    // Set CORS headers
    static auto set_cors_header(httplib::Response& res) -> void {
        res.set_header("Access-Control-Allow-Origin", "*");
    }

}; // class rest_helpers

// ============================================================================
// Property REST Handlers
// ============================================================================

class property_handlers {
public:
    // GET /app/components/:id/properties
    static auto get_properties_collection(
        component* comp,
        const httplib::Request& req,
        std::string_view base_path
    ) -> httplib::Response;

    // GET /app/components/:id/properties/:name
    static auto get_property(
        component* comp,
        std::string_view property_name,
        std::string_view path
    ) -> httplib::Response;

    // PUT /app/components/:id/properties/:name
    static auto put_property(
        component* comp,
        std::string_view property_name,
        const nlohmann::json& request_body,
        std::string_view path
    ) -> httplib::Response;

    // PATCH /app/components/:id/properties/:name
    static auto patch_property(
        component* comp,
        std::string_view property_name,
        const nlohmann::json& request_body,
        std::string_view path
    ) -> httplib::Response;

    // DELETE /app/components/:id/properties/:name
    static auto delete_property(
        component* comp,
        std::string_view property_name,
        std::string_view path
    ) -> httplib::Response;

    // ========================================================================
    // List Property Operations
    // ========================================================================

    // GET /app/components/:id/properties/:name/items
    static auto get_list_items(
        component* comp,
        std::string_view property_name,
        std::string_view path,
        std::string_view base_path
    ) -> httplib::Response;

    // GET /app/components/:id/properties/:name/items/:index
    static auto get_list_item(
        component* comp,
        std::string_view property_name,
        std::size_t index,
        std::string_view path
    ) -> httplib::Response;

    // POST /app/components/:id/properties/:name/items
    static auto post_list_item(
        component* comp,
        std::string_view property_name,
        const nlohmann::json& request_body,
        std::string_view path,
        std::string_view base_path
    ) -> httplib::Response;

    // PUT /app/components/:id/properties/:name/items/:index
    static auto put_list_item(
        component* comp,
        std::string_view property_name,
        std::size_t index,
        const nlohmann::json& request_body,
        std::string_view path
    ) -> httplib::Response;

    // DELETE /app/components/:id/properties/:name/items/:index
    static auto delete_list_item(
        component* comp,
        std::string_view property_name,
        std::size_t index,
        std::string_view path
    ) -> httplib::Response;

    // ========================================================================
    // Struct Property Operations
    // ========================================================================

    // GET /app/components/:id/properties/:name/fields
    static auto get_struct_fields(
        component* comp,
        std::string_view property_name,
        std::string_view path,
        std::string_view base_path
    ) -> httplib::Response;

    // GET /app/components/:id/properties/:name/fields/:field
    static auto get_struct_field(
        component* comp,
        std::string_view property_name,
        std::string_view field_name,
        std::string_view path
    ) -> httplib::Response;

    // PATCH /app/components/:id/properties/:name/fields/:field
    static auto patch_struct_field(
        component* comp,
        std::string_view property_name,
        std::string_view field_name,
        const nlohmann::json& request_body,
        std::string_view path
    ) -> httplib::Response;

    // GET /app/components/:id/properties/:name/items/:index/fields
    static auto get_struct_list_item_fields(
        component* comp,
        std::string_view property_name,
        std::size_t index,
        std::string_view path,
        std::string_view base_path
    ) -> httplib::Response;

    // PATCH /app/components/:id/properties/:name/items/:index/fields/:field
    static auto patch_struct_list_item_field(
        component* comp,
        std::string_view property_name,
        std::size_t index,
        std::string_view field_name,
        const nlohmann::json& request_body,
        std::string_view path
    ) -> httplib::Response;

    // ========================================================================
    // Validation & Discovery Operations
    // ========================================================================

    // POST /app/components/:id/properties/:name/validate
    static auto validate_property_value(
        component* comp,
        std::string_view property_name,
        const nlohmann::json& request_body,
        std::string_view path
    ) -> httplib::Response;

    // GET /app/components/:id/properties/:name/schema
    static auto get_property_schema(
        component* comp,
        std::string_view property_name,
        std::string_view path
    ) -> httplib::Response;

private:
    // Helper: Get property from component
    static auto get_property_from_component(
        component* comp,
        std::string_view property_name,
        std::string_view path
    ) -> std::pair<const property*, std::optional<httplib::Response>>;

    // Helper: Build property JSON response
    static auto build_property_json(
        const property& prop,
        std::string_view name,
        std::string_view href
    ) -> nlohmann::json;

    // Helper: Validate runtime configurability
    static auto validate_runtime_configurable(
        const property& prop,
        std::string_view property_name,
        std::string_view path
    ) -> std::optional<httplib::Response>;

    // Helper: Update property value
    static auto update_property_value(
        component* comp,
        std::string_view property_name,
        const nlohmann::json& value_json,
        std::string_view path
    ) -> std::optional<httplib::Response>;

    // Helper: Validate property is a list
    static auto validate_is_list(
        const property& prop,
        std::string_view property_name,
        std::string_view path
    ) -> std::optional<httplib::Response>;

    // Helper: Get list size
    static auto get_list_size(const property& prop) -> std::size_t;

    // Helper: Validate list index
    static auto validate_list_index(
        const property& prop,
        std::string_view property_name,
        std::size_t index,
        std::string_view path
    ) -> std::optional<httplib::Response>;

    // Helper: Validate property is structured
    static auto validate_is_structured(
        const property& prop,
        std::string_view property_name,
        std::string_view path
    ) -> std::optional<httplib::Response>;

    // Helper: Build field JSON response
    static auto build_field_json(
        const property& field_prop,
        std::string_view field_name,
        std::string_view href
    ) -> nlohmann::json;

    // Helper: Build schema JSON response
    static auto build_schema_json(
        const property& prop,
        std::string_view property_name
    ) -> nlohmann::json;

    // Helper: Check if property matches filter criteria
    static auto matches_filter(
        const property& prop,
        const std::optional<std::string>& type_filter,
        const std::optional<std::string>& config_filter
    ) -> bool;

}; // class property_handlers

} // namespace composite::properties::rest
