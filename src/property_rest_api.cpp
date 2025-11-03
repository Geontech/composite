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

#include "property_rest_api.hpp"
#include "property_serializer.hpp"

#include <format>
#include <spdlog/spdlog.h>

namespace composite::properties::rest {

auto property_handlers::get_property_from_component(
    component* comp,
    std::string_view property_name,
    std::string_view path
) -> std::pair<const property*, std::optional<httplib::Response>> {

    const auto& props = comp->properties();
    auto it = props.find(std::string{property_name});

    if (it == props.end()) {
        auto details = nlohmann::json::object();
        details["property"] = property_name;
        details["component"] = comp->id();

        return {
            nullptr,
            rest_helpers::error_response_obj(
                error_code::PROPERTY_NOT_FOUND,
                std::format("Property '{}' not found in component '{}'", property_name, comp->id()),
                path,
                details
            )
        };
    }

    return {&it->second, std::nullopt};
}

auto property_handlers::build_property_json(
    const property& prop,
    std::string_view name,
    std::string_view href
) -> nlohmann::json {
    auto json_obj = nlohmann::json::object();

    // Use property_serializer for consistent format
    property_serializer::to_json(json_obj, prop);

    // Add metadata
    json_obj["name"] = name;
    json_obj["href"] = href;

    return json_obj;
}

auto property_handlers::validate_runtime_configurable(
    const property& prop,
    std::string_view property_name,
    std::string_view path
) -> std::optional<httplib::Response> {

    if (prop.configurability() == config_type::INITIALIZE) {
        auto details = nlohmann::json::object();
        details["property"] = property_name;
        details["configurability"] = "initialize";

        return rest_helpers::error_response_obj(
            error_code::NOT_RUNTIME_CONFIGURABLE,
            std::format("Property '{}' can only be configured at initialization", property_name),
            path,
            details
        );
    }

    return std::nullopt;
}

auto property_handlers::update_property_value(
    component* comp,
    std::string_view property_name,
    const nlohmann::json& value_json,
    std::string_view path
) -> std::optional<httplib::Response> {

    try {
        // Convert JSON value to string for property system
        std::string value_str;
        if (value_json.is_string()) {
            value_str = value_json.get<std::string>();
        } else if (value_json.is_number()) {
            value_str = std::to_string(value_json.get<double>());
        } else if (value_json.is_boolean()) {
            value_str = value_json.get<bool>() ? "true" : "false";
        } else if (value_json.is_null()) {
            value_str = composite::properties::null_prop;
        } else {
            auto details = nlohmann::json::object();
            details["provided_type"] = value_json.type_name();

            return rest_helpers::error_response_obj(
                error_code::INVALID_TYPE,
                "Value must be a scalar (string, number, boolean, or null)",
                path,
                details
            );
        }

        // Set the property
        auto props = std::vector<std::pair<std::string, std::string>>{
            {std::string{property_name}, value_str}
        };

        comp->set_properties(props, config_type::RUNTIME);
        comp->property_change_handler();

        return std::nullopt; // Success

    } catch (const configurability_error& ex) {
        auto details = nlohmann::json::object();
        details["property"] = ex.prop;

        return rest_helpers::error_response_obj(
            error_code::NOT_RUNTIME_CONFIGURABLE,
            ex.what(),
            path,
            details
        );

    } catch (const key_error& ex) {
        auto details = nlohmann::json::object();
        details["property"] = ex.prop;

        return rest_helpers::error_response_obj(
            error_code::PROPERTY_NOT_FOUND,
            ex.what(),
            path,
            details
        );

    } catch (const type_error& ex) {
        auto details = nlohmann::json::object();
        details["property"] = ex.prop;
        details["type"] = ex.type;

        return rest_helpers::error_response_obj(
            error_code::INVALID_TYPE,
            ex.what(),
            path,
            details
        );

    } catch (const value_error& ex) {
        auto details = nlohmann::json::object();
        details["property"] = ex.prop;
        details["value"] = ex.value;

        return rest_helpers::error_response_obj(
            error_code::INVALID_VALUE,
            ex.what(),
            path,
            details
        );

    } catch (const std::exception& ex) {
        auto details = nlohmann::json::object();
        details["exception"] = ex.what();

        return rest_helpers::error_response_obj(
            error_code::VALIDATION_FAILED,
            std::format("Failed to update property '{}': {}", property_name, ex.what()),
            path,
            details
        );
    }
}

// ============================================================================
// Public Handler Implementations
// ============================================================================

auto property_handlers::get_properties_collection(
    component* comp,
    const httplib::Request& req,
    std::string_view base_path
) -> httplib::Response {

    // Extract query parameters
    std::optional<std::string> type_filter;
    std::optional<std::string> config_filter;
    bool include_schema = false;

    if (req.has_param("type")) {
        type_filter = req.get_param_value("type");
    }
    if (req.has_param("configurability")) {
        config_filter = req.get_param_value("configurability");
    }
    if (req.has_param("schema")) {
        auto schema_param = req.get_param_value("schema");
        include_schema = (schema_param == "true" || schema_param == "1");
    }

    auto response_obj = nlohmann::json::object();
    auto properties_obj = nlohmann::json::object();
    std::size_t count = 0;

    for (const auto& [name, prop] : comp->properties()) {
        // Apply filters
        if (!matches_filter(prop, type_filter, config_filter)) {
            continue;
        }

        auto href = std::format("{}/{}", base_path, name);

        if (include_schema) {
            // Include schema information
            auto prop_json = build_schema_json(prop, name);
            prop_json["href"] = href;
            properties_obj[name] = prop_json;
        } else {
            // Normal property response
            properties_obj[name] = build_property_json(prop, name, href);
        }

        count++;
    }

    response_obj["properties"] = properties_obj;
    response_obj["count"] = count;

    // Include filter information if filters were applied
    if (type_filter.has_value() || config_filter.has_value()) {
        auto filters_obj = nlohmann::json::object();
        if (type_filter.has_value()) {
            filters_obj["type"] = type_filter.value();
        }
        if (config_filter.has_value()) {
            filters_obj["configurability"] = config_filter.value();
        }
        response_obj["filters"] = filters_obj;
    }

    return rest_helpers::success_response(response_obj);
}

auto property_handlers::get_property(
    component* comp,
    std::string_view property_name,
    std::string_view path
) -> httplib::Response {

    // Get the property
    auto [prop, error] = get_property_from_component(comp, property_name, path);
    if (error) {
        return *error;
    }

    // Build response
    auto response = build_property_json(*prop, property_name, path);

    return rest_helpers::success_response(response);
}

auto property_handlers::put_property(
    component* comp,
    std::string_view property_name,
    const nlohmann::json& request_body,
    std::string_view path
) -> httplib::Response {

    // Verify property exists
    auto [prop, error] = get_property_from_component(comp, property_name, path);
    if (error) {
        return *error;
    }

    // Validate runtime configurability
    if (auto config_error = validate_runtime_configurable(*prop, property_name, path)) {
        return *config_error;
    }

    // Extract value from request body
    if (!request_body.contains("value")) {
        auto details = nlohmann::json::object();
        details["required_field"] = "value";

        return rest_helpers::error_response_obj(
            error_code::MISSING_FIELD,
            "Request body must contain 'value' field",
            path,
            details
        );
    }

    const auto& value_json = request_body["value"];

    // Update the property
    if (auto update_error = update_property_value(comp, property_name, value_json, path)) {
        return *update_error;
    }

    // Get updated property and return
    return get_property(comp, property_name, path);
}

auto property_handlers::patch_property(
    component* comp,
    std::string_view property_name,
    const nlohmann::json& request_body,
    std::string_view path
) -> httplib::Response {

    // PATCH is the same as PUT for simple properties
    // In the future, PATCH could support partial updates for structured properties
    return put_property(comp, property_name, request_body, path);
}

auto property_handlers::delete_property(
    component* comp,
    std::string_view property_name,
    std::string_view path
) -> httplib::Response {

    // Verify property exists
    auto [prop, error] = get_property_from_component(comp, property_name, path);
    if (error) {
        return *error;
    }

    // Validate runtime configurability
    if (auto config_error = validate_runtime_configurable(*prop, property_name, path)) {
        return *config_error;
    }

    // Reset to default by setting to null
    auto null_value = nlohmann::json(nullptr);
    if (auto update_error = update_property_value(comp, property_name, null_value, path)) {
        return *update_error;
    }

    // Build success response
    auto response = nlohmann::json::object();
    response["success"] = true;
    response["message"] = std::format("Property '{}' reset to default value", property_name);
    response["property"] = property_name;

    return rest_helpers::success_response(response);
}

// ============================================================================
// List Operation Helper Functions
// ============================================================================

auto property_handlers::validate_is_list(
    const property& prop,
    std::string_view property_name,
    std::string_view path
) -> std::optional<httplib::Response> {

    if (!prop.is_list()) {
        auto details = nlohmann::json::object();
        details["property"] = property_name;
        details["type"] = prop.type();

        return rest_helpers::error_response_obj(
            error_code::UNSUPPORTED_OPERATION,
            std::format("Property '{}' is not a list (type: {})", property_name, prop.type()),
            path,
            details
        );
    }

    return std::nullopt;
}

auto property_handlers::get_list_size(const property& prop) -> std::size_t {
    auto type = prop.type();

    // Handle structured lists
    if (type == "[]struct") {
        return prop.struct_list_size();
    }

    // Handle scalar lists
    if (type == "[]bool") {
        return std::any_cast<std::vector<bool>*>(prop.value())->size();
    } else if (type == "[]string") {
        return std::any_cast<std::vector<std::string>*>(prop.value())->size();
    } else if (type == "[]int16") {
        return std::any_cast<std::vector<int16_t>*>(prop.value())->size();
    } else if (type == "[]uint16") {
        return std::any_cast<std::vector<uint16_t>*>(prop.value())->size();
    } else if (type == "[]int32") {
        return std::any_cast<std::vector<int32_t>*>(prop.value())->size();
    } else if (type == "[]uint32") {
        return std::any_cast<std::vector<uint32_t>*>(prop.value())->size();
    } else if (type == "[]int64") {
        return std::any_cast<std::vector<int64_t>*>(prop.value())->size();
    } else if (type == "[]uint64") {
        return std::any_cast<std::vector<uint64_t>*>(prop.value())->size();
    } else if (type == "[]float") {
        return std::any_cast<std::vector<float>*>(prop.value())->size();
    } else if (type == "[]double") {
        return std::any_cast<std::vector<double>*>(prop.value())->size();
    }

    return 0;
}

auto property_handlers::validate_list_index(
    const property& prop,
    std::string_view property_name,
    std::size_t index,
    std::string_view path
) -> std::optional<httplib::Response> {

    auto size = get_list_size(prop);

    if (index >= size) {
        auto details = nlohmann::json::object();
        details["property"] = property_name;
        details["index"] = index;
        details["list_size"] = size;

        return rest_helpers::error_response_obj(
            error_code::INVALID_INDEX,
            std::format("Index {} out of bounds for list property '{}' (size: {})", index, property_name, size),
            path,
            details
        );
    }

    return std::nullopt;
}

// ============================================================================
// List Operation Handler Implementations
// ============================================================================

auto property_handlers::get_list_items(
    component* comp,
    std::string_view property_name,
    std::string_view path,
    std::string_view base_path
) -> httplib::Response {

    // Get the property
    auto [prop, error] = get_property_from_component(comp, property_name, path);
    if (error) {
        return *error;
    }

    // Validate it's a list
    if (auto list_error = validate_is_list(*prop, property_name, path)) {
        return *list_error;
    }

    // Build response with all items
    auto response_obj = nlohmann::json::object();
    response_obj["name"] = property_name;
    response_obj["type"] = prop->type();

    auto items_array = nlohmann::json::array();
    auto size = get_list_size(*prop);

    for (std::size_t i = 0; i < size; ++i) {
        auto item_obj = nlohmann::json::object();
        item_obj["index"] = i;
        item_obj["href"] = std::format("{}/{}", base_path, i);

        // Get value - serialize list item to get its value
        // For scalar lists, we need to access the vector directly
        try {
            auto type = prop->type();
            if (type == "[]bool") {
                auto& vec = *std::any_cast<std::vector<bool>*>(prop->value());
                item_obj["value"] = vec[i] ? "true" : "false";
            } else if (type == "[]string") {
                auto& vec = *std::any_cast<std::vector<std::string>*>(prop->value());
                item_obj["value"] = vec[i];
            } else if (type == "[]int16") {
                auto& vec = *std::any_cast<std::vector<int16_t>*>(prop->value());
                item_obj["value"] = std::to_string(vec[i]);
            } else if (type == "[]uint16") {
                auto& vec = *std::any_cast<std::vector<uint16_t>*>(prop->value());
                item_obj["value"] = std::to_string(vec[i]);
            } else if (type == "[]int32") {
                auto& vec = *std::any_cast<std::vector<int32_t>*>(prop->value());
                item_obj["value"] = std::to_string(vec[i]);
            } else if (type == "[]uint32") {
                auto& vec = *std::any_cast<std::vector<uint32_t>*>(prop->value());
                item_obj["value"] = std::to_string(vec[i]);
            } else if (type == "[]int64") {
                auto& vec = *std::any_cast<std::vector<int64_t>*>(prop->value());
                item_obj["value"] = std::to_string(vec[i]);
            } else if (type == "[]uint64") {
                auto& vec = *std::any_cast<std::vector<uint64_t>*>(prop->value());
                item_obj["value"] = std::to_string(vec[i]);
            } else if (type == "[]float") {
                auto& vec = *std::any_cast<std::vector<float>*>(prop->value());
                item_obj["value"] = std::to_string(vec[i]);
            } else if (type == "[]double") {
                auto& vec = *std::any_cast<std::vector<double>*>(prop->value());
                item_obj["value"] = std::to_string(vec[i]);
            } else if (type == "[]struct") {
                // For struct lists, serialize the struct
                auto* item_ptr = prop->struct_getter(i);
                if (item_ptr) {
                    auto bound = property_set{};
                    prop->struct_registration(bound, item_ptr);
                    auto struct_json = nlohmann::json::object();
                    property_serializer::to_json(struct_json, bound);
                    item_obj["value"] = struct_json;
                } else {
                    item_obj["value"] = nullptr;
                }
            } else {
                item_obj["value"] = nullptr;
            }
        } catch (const std::exception& ex) {
            item_obj["value"] = nullptr;
        }

        items_array.push_back(item_obj);
    }

    response_obj["items"] = items_array;
    response_obj["count"] = size;

    return rest_helpers::success_response(response_obj);
}

auto property_handlers::get_list_item(
    component* comp,
    std::string_view property_name,
    std::size_t index,
    std::string_view path
) -> httplib::Response {

    // Get the property
    auto [prop, error] = get_property_from_component(comp, property_name, path);
    if (error) {
        return *error;
    }

    // Validate it's a list
    if (auto list_error = validate_is_list(*prop, property_name, path)) {
        return *list_error;
    }

    // Validate index
    if (auto index_error = validate_list_index(*prop, property_name, index, path)) {
        return *index_error;
    }

    // Get value by accessing the vector directly based on type
    auto response_obj = nlohmann::json::object();
    response_obj["index"] = index;
    response_obj["href"] = path;

    try {
        auto type = prop->type();
        if (type == "[]bool") {
            auto& vec = *std::any_cast<std::vector<bool>*>(prop->value());
            response_obj["value"] = vec[index] ? "true" : "false";
        } else if (type == "[]string") {
            auto& vec = *std::any_cast<std::vector<std::string>*>(prop->value());
            response_obj["value"] = vec[index];
        } else if (type == "[]int16") {
            auto& vec = *std::any_cast<std::vector<int16_t>*>(prop->value());
            response_obj["value"] = std::to_string(vec[index]);
        } else if (type == "[]uint16") {
            auto& vec = *std::any_cast<std::vector<uint16_t>*>(prop->value());
            response_obj["value"] = std::to_string(vec[index]);
        } else if (type == "[]int32") {
            auto& vec = *std::any_cast<std::vector<int32_t>*>(prop->value());
            response_obj["value"] = std::to_string(vec[index]);
        } else if (type == "[]uint32") {
            auto& vec = *std::any_cast<std::vector<uint32_t>*>(prop->value());
            response_obj["value"] = std::to_string(vec[index]);
        } else if (type == "[]int64") {
            auto& vec = *std::any_cast<std::vector<int64_t>*>(prop->value());
            response_obj["value"] = std::to_string(vec[index]);
        } else if (type == "[]uint64") {
            auto& vec = *std::any_cast<std::vector<uint64_t>*>(prop->value());
            response_obj["value"] = std::to_string(vec[index]);
        } else if (type == "[]float") {
            auto& vec = *std::any_cast<std::vector<float>*>(prop->value());
            response_obj["value"] = std::to_string(vec[index]);
        } else if (type == "[]double") {
            auto& vec = *std::any_cast<std::vector<double>*>(prop->value());
            response_obj["value"] = std::to_string(vec[index]);
        } else if (type == "[]struct") {
            auto* item_ptr = prop->struct_getter(index);
            if (item_ptr) {
                auto bound = property_set{};
                prop->struct_registration(bound, item_ptr);
                auto struct_json = nlohmann::json::object();
                property_serializer::to_json(struct_json, bound);
                response_obj["value"] = struct_json;
            } else {
                response_obj["value"] = nullptr;
            }
        } else {
            response_obj["value"] = nullptr;
        }
    } catch (const std::exception& ex) {
        auto details = nlohmann::json::object();
        details["exception"] = ex.what();

        return rest_helpers::error_response_obj(
            error_code::INVALID_INDEX,
            std::format("Failed to get item at index {}: {}", index, ex.what()),
            path,
            details
        );
    }

    return rest_helpers::success_response(response_obj);
}

auto property_handlers::post_list_item(
    component* comp,
    std::string_view property_name,
    const nlohmann::json& request_body,
    std::string_view path,
    std::string_view base_path
) -> httplib::Response {

    // Get the property
    auto [prop, error] = get_property_from_component(comp, property_name, path);
    if (error) {
        return *error;
    }

    // Validate it's a list
    if (auto list_error = validate_is_list(*prop, property_name, path)) {
        return *list_error;
    }

    // Validate runtime configurability
    if (auto config_error = validate_runtime_configurable(*prop, property_name, path)) {
        return *config_error;
    }

    // Extract value from request body
    if (!request_body.contains("value")) {
        auto details = nlohmann::json::object();
        details["required_field"] = "value";

        return rest_helpers::error_response_obj(
            error_code::MISSING_FIELD,
            "Request body must contain 'value' field",
            path,
            details
        );
    }

    const auto& value_json = request_body["value"];

    // Use property path notation for append (foo[])
    auto prop_path = std::format("{}[]", property_name);
    if (auto update_error = update_property_value(comp, prop_path, value_json, path)) {
        return *update_error;
    }

    // Get the new index (size - 1 after append)
    auto [updated_prop, get_error] = get_property_from_component(comp, property_name, path);
    if (get_error) {
        return *get_error;
    }

    auto new_index = get_list_size(*updated_prop) - 1;

    // Build success response
    auto response = nlohmann::json::object();
    response["success"] = true;
    response["index"] = new_index;
    response["href"] = std::format("{}/{}", base_path, new_index);

    // Get the appended value
    try {
        auto type = updated_prop->type();
        if (type == "[]bool") {
            auto& vec = *std::any_cast<std::vector<bool>*>(updated_prop->value());
            response["value"] = vec[new_index] ? "true" : "false";
        } else if (type == "[]string") {
            auto& vec = *std::any_cast<std::vector<std::string>*>(updated_prop->value());
            response["value"] = vec[new_index];
        } else if (type == "[]int16") {
            auto& vec = *std::any_cast<std::vector<int16_t>*>(updated_prop->value());
            response["value"] = std::to_string(vec[new_index]);
        } else if (type == "[]uint16") {
            auto& vec = *std::any_cast<std::vector<uint16_t>*>(updated_prop->value());
            response["value"] = std::to_string(vec[new_index]);
        } else if (type == "[]int32") {
            auto& vec = *std::any_cast<std::vector<int32_t>*>(updated_prop->value());
            response["value"] = std::to_string(vec[new_index]);
        } else if (type == "[]uint32") {
            auto& vec = *std::any_cast<std::vector<uint32_t>*>(updated_prop->value());
            response["value"] = std::to_string(vec[new_index]);
        } else if (type == "[]int64") {
            auto& vec = *std::any_cast<std::vector<int64_t>*>(updated_prop->value());
            response["value"] = std::to_string(vec[new_index]);
        } else if (type == "[]uint64") {
            auto& vec = *std::any_cast<std::vector<uint64_t>*>(updated_prop->value());
            response["value"] = std::to_string(vec[new_index]);
        } else if (type == "[]float") {
            auto& vec = *std::any_cast<std::vector<float>*>(updated_prop->value());
            response["value"] = std::to_string(vec[new_index]);
        } else if (type == "[]double") {
            auto& vec = *std::any_cast<std::vector<double>*>(updated_prop->value());
            response["value"] = std::to_string(vec[new_index]);
        } else if (type == "[]struct") {
            auto* item_ptr = updated_prop->struct_getter(new_index);
            if (item_ptr) {
                auto bound = property_set{};
                updated_prop->struct_registration(bound, item_ptr);
                auto struct_json = nlohmann::json::object();
                property_serializer::to_json(struct_json, bound);
                response["value"] = struct_json;
            } else {
                response["value"] = nullptr;
            }
        } else {
            response["value"] = nullptr;
        }
    } catch (const std::exception&) {
        response["value"] = nullptr;
    }

    auto res = httplib::Response{};
    res.set_content(response.dump(2), "application/json");
    res.status = httplib::Created_201;
    rest_helpers::set_cors_header(res);
    return res;
}

auto property_handlers::put_list_item(
    component* comp,
    std::string_view property_name,
    std::size_t index,
    const nlohmann::json& request_body,
    std::string_view path
) -> httplib::Response {

    // Get the property
    auto [prop, error] = get_property_from_component(comp, property_name, path);
    if (error) {
        return *error;
    }

    // Validate it's a list
    if (auto list_error = validate_is_list(*prop, property_name, path)) {
        return *list_error;
    }

    // Validate runtime configurability
    if (auto config_error = validate_runtime_configurable(*prop, property_name, path)) {
        return *config_error;
    }

    // Validate index
    if (auto index_error = validate_list_index(*prop, property_name, index, path)) {
        return *index_error;
    }

    // Extract value from request body
    if (!request_body.contains("value")) {
        auto details = nlohmann::json::object();
        details["required_field"] = "value";

        return rest_helpers::error_response_obj(
            error_code::MISSING_FIELD,
            "Request body must contain 'value' field",
            path,
            details
        );
    }

    const auto& value_json = request_body["value"];

    // Use property path notation for indexed update (foo[i])
    auto prop_path = std::format("{}[{}]", property_name, index);
    if (auto update_error = update_property_value(comp, prop_path, value_json, path)) {
        return *update_error;
    }

    // Return updated item
    return get_list_item(comp, property_name, index, path);
}

auto property_handlers::delete_list_item(
    component* comp,
    std::string_view property_name,
    std::size_t index,
    std::string_view path
) -> httplib::Response {

    // Get the property
    auto [prop, error] = get_property_from_component(comp, property_name, path);
    if (error) {
        return *error;
    }

    // Validate it's a list
    if (auto list_error = validate_is_list(*prop, property_name, path)) {
        return *list_error;
    }

    // Validate runtime configurability
    if (auto config_error = validate_runtime_configurable(*prop, property_name, path)) {
        return *config_error;
    }

    // Validate index
    if (auto index_error = validate_list_index(*prop, property_name, index, path)) {
        return *index_error;
    }

    // Get previous value before deletion
    std::string previous_value;
    try {
        auto type = prop->type();
        if (type == "[]bool") {
            auto& vec = *std::any_cast<std::vector<bool>*>(prop->value());
            previous_value = vec[index] ? "true" : "false";
        } else if (type == "[]string") {
            auto& vec = *std::any_cast<std::vector<std::string>*>(prop->value());
            previous_value = vec[index];
        } else if (type == "[]int16") {
            auto& vec = *std::any_cast<std::vector<int16_t>*>(prop->value());
            previous_value = std::to_string(vec[index]);
        } else if (type == "[]uint16") {
            auto& vec = *std::any_cast<std::vector<uint16_t>*>(prop->value());
            previous_value = std::to_string(vec[index]);
        } else if (type == "[]int32") {
            auto& vec = *std::any_cast<std::vector<int32_t>*>(prop->value());
            previous_value = std::to_string(vec[index]);
        } else if (type == "[]uint32") {
            auto& vec = *std::any_cast<std::vector<uint32_t>*>(prop->value());
            previous_value = std::to_string(vec[index]);
        } else if (type == "[]int64") {
            auto& vec = *std::any_cast<std::vector<int64_t>*>(prop->value());
            previous_value = std::to_string(vec[index]);
        } else if (type == "[]uint64") {
            auto& vec = *std::any_cast<std::vector<uint64_t>*>(prop->value());
            previous_value = std::to_string(vec[index]);
        } else if (type == "[]float") {
            auto& vec = *std::any_cast<std::vector<float>*>(prop->value());
            previous_value = std::to_string(vec[index]);
        } else if (type == "[]double") {
            auto& vec = *std::any_cast<std::vector<double>*>(prop->value());
            previous_value = std::to_string(vec[index]);
        } else if (type == "[]struct") {
            previous_value = "[struct]";
        } else {
            previous_value = "unknown";
        }
    } catch (const std::exception&) {
        previous_value = "unknown";
    }

    // Delete by setting to null using property path notation (foo[i] = null)
    auto prop_path = std::format("{}[{}]", property_name, index);
    auto null_value = nlohmann::json(nullptr);
    if (auto update_error = update_property_value(comp, prop_path, null_value, path)) {
        return *update_error;
    }

    // Build success response
    auto response = nlohmann::json::object();
    response["success"] = true;
    response["message"] = std::format("Item at index {} removed", index);
    response["previous_value"] = previous_value;

    return rest_helpers::success_response(response);
}

// ============================================================================
// Struct Operation Helper Functions
// ============================================================================

auto property_handlers::validate_is_structured(
    const property& prop,
    std::string_view property_name,
    std::string_view path
) -> std::optional<httplib::Response> {

    if (!prop.is_structured()) {
        auto details = nlohmann::json::object();
        details["property"] = property_name;
        details["type"] = prop.type();

        return rest_helpers::error_response_obj(
            error_code::UNSUPPORTED_OPERATION,
            std::format("Property '{}' is not a structured type (type: {})", property_name, prop.type()),
            path,
            details
        );
    }

    return std::nullopt;
}

auto property_handlers::build_field_json(
    const property& field_prop,
    std::string_view field_name,
    std::string_view href
) -> nlohmann::json {
    auto json_obj = nlohmann::json::object();

    // Serialize field value
    property_serializer::to_json(json_obj, field_prop);

    // Add field metadata
    json_obj["name"] = field_name;
    json_obj["href"] = href;

    return json_obj;
}

// ============================================================================
// Struct Operation Handler Implementations
// ============================================================================

auto property_handlers::get_struct_fields(
    component* comp,
    std::string_view property_name,
    std::string_view path,
    std::string_view base_path
) -> httplib::Response {

    // Get the property
    auto [prop, error] = get_property_from_component(comp, property_name, path);
    if (error) {
        return *error;
    }

    // Validate it's a structured property
    if (auto struct_error = validate_is_structured(*prop, property_name, path)) {
        return *struct_error;
    }

    // Build response with all fields
    auto response_obj = nlohmann::json::object();
    response_obj["name"] = property_name;
    response_obj["type"] = prop->type();

    auto fields_obj = nlohmann::json::object();
    const auto& struct_set = prop->structured();

    for (const auto& [field_name, field_prop] : struct_set.properties()) {
        auto href = std::format("{}/{}", base_path, field_name);
        fields_obj[field_name] = build_field_json(field_prop, field_name, href);
    }

    response_obj["fields"] = fields_obj;
    response_obj["count"] = struct_set.properties().size();

    return rest_helpers::success_response(response_obj);
}

auto property_handlers::get_struct_field(
    component* comp,
    std::string_view property_name,
    std::string_view field_name,
    std::string_view path
) -> httplib::Response {

    // Get the property
    auto [prop, error] = get_property_from_component(comp, property_name, path);
    if (error) {
        return *error;
    }

    // Validate it's a structured property
    if (auto struct_error = validate_is_structured(*prop, property_name, path)) {
        return *struct_error;
    }

    // Get the field from the structured property
    const auto& struct_set = prop->structured();
    const auto& fields = struct_set.properties();
    auto it = fields.find(std::string{field_name});

    if (it == fields.end()) {
        auto details = nlohmann::json::object();
        details["property"] = property_name;
        details["field"] = field_name;

        return rest_helpers::error_response_obj(
            error_code::PROPERTY_NOT_FOUND,
            std::format("Field '{}' not found in structured property '{}'", field_name, property_name),
            path,
            details
        );
    }

    // Build response
    auto response = build_field_json(it->second, field_name, path);

    return rest_helpers::success_response(response);
}

auto property_handlers::patch_struct_field(
    component* comp,
    std::string_view property_name,
    std::string_view field_name,
    const nlohmann::json& request_body,
    std::string_view path
) -> httplib::Response {

    // Get the property
    auto [prop, error] = get_property_from_component(comp, property_name, path);
    if (error) {
        return *error;
    }

    // Validate it's a structured property
    if (auto struct_error = validate_is_structured(*prop, property_name, path)) {
        return *struct_error;
    }

    // Validate runtime configurability
    if (auto config_error = validate_runtime_configurable(*prop, property_name, path)) {
        return *config_error;
    }

    // Extract value from request body
    if (!request_body.contains("value")) {
        auto details = nlohmann::json::object();
        details["required_field"] = "value";

        return rest_helpers::error_response_obj(
            error_code::MISSING_FIELD,
            "Request body must contain 'value' field",
            path,
            details
        );
    }

    const auto& value_json = request_body["value"];

    // Use property path notation for struct field update (foo.bar)
    auto prop_path = std::format("{}.{}", property_name, field_name);
    if (auto update_error = update_property_value(comp, prop_path, value_json, path)) {
        return *update_error;
    }

    // Return updated field
    return get_struct_field(comp, property_name, field_name, path);
}

auto property_handlers::get_struct_list_item_fields(
    component* comp,
    std::string_view property_name,
    std::size_t index,
    std::string_view path,
    std::string_view base_path
) -> httplib::Response {

    // Get the property
    auto [prop, error] = get_property_from_component(comp, property_name, path);
    if (error) {
        return *error;
    }

    // Validate it's a list
    if (auto list_error = validate_is_list(*prop, property_name, path)) {
        return *list_error;
    }

    // Validate index
    if (auto index_error = validate_list_index(*prop, property_name, index, path)) {
        return *index_error;
    }

    // Check if it's a struct list
    if (prop->type() != "[]struct") {
        auto details = nlohmann::json::object();
        details["property"] = property_name;
        details["type"] = prop->type();

        return rest_helpers::error_response_obj(
            error_code::UNSUPPORTED_OPERATION,
            std::format("Property '{}' is not a struct list (type: {})", property_name, prop->type()),
            path,
            details
        );
    }

    // Get the struct at the specified index
    auto* item_ptr = prop->struct_getter(index);
    if (!item_ptr) {
        auto details = nlohmann::json::object();
        details["index"] = index;

        return rest_helpers::error_response_obj(
            error_code::INVALID_INDEX,
            std::format("Failed to get struct at index {}", index),
            path,
            details
        );
    }

    // Bind the struct to a property_set
    auto bound = property_set{};
    prop->struct_registration(bound, item_ptr);

    // Build response with all fields
    auto response_obj = nlohmann::json::object();
    response_obj["name"] = property_name;
    response_obj["type"] = prop->type();
    response_obj["index"] = index;

    auto fields_obj = nlohmann::json::object();
    for (const auto& [field_name, field_prop] : bound.properties()) {
        auto href = std::format("{}/{}", base_path, field_name);
        fields_obj[field_name] = build_field_json(field_prop, field_name, href);
    }

    response_obj["fields"] = fields_obj;
    response_obj["count"] = bound.properties().size();

    return rest_helpers::success_response(response_obj);
}

auto property_handlers::patch_struct_list_item_field(
    component* comp,
    std::string_view property_name,
    std::size_t index,
    std::string_view field_name,
    const nlohmann::json& request_body,
    std::string_view path
) -> httplib::Response {

    // Get the property
    auto [prop, error] = get_property_from_component(comp, property_name, path);
    if (error) {
        return *error;
    }

    // Validate it's a list
    if (auto list_error = validate_is_list(*prop, property_name, path)) {
        return *list_error;
    }

    // Validate runtime configurability
    if (auto config_error = validate_runtime_configurable(*prop, property_name, path)) {
        return *config_error;
    }

    // Validate index
    if (auto index_error = validate_list_index(*prop, property_name, index, path)) {
        return *index_error;
    }

    // Check if it's a struct list
    if (prop->type() != "[]struct") {
        auto details = nlohmann::json::object();
        details["property"] = property_name;
        details["type"] = prop->type();

        return rest_helpers::error_response_obj(
            error_code::UNSUPPORTED_OPERATION,
            std::format("Property '{}' is not a struct list (type: {})", property_name, prop->type()),
            path,
            details
        );
    }

    // Extract value from request body
    if (!request_body.contains("value")) {
        auto details = nlohmann::json::object();
        details["required_field"] = "value";

        return rest_helpers::error_response_obj(
            error_code::MISSING_FIELD,
            "Request body must contain 'value' field",
            path,
            details
        );
    }

    const auto& value_json = request_body["value"];

    // Get the current struct to read all field values
    auto* item_ptr_before = prop->struct_getter(index);
    if (!item_ptr_before) {
        auto details = nlohmann::json::object();
        details["index"] = index;
        return rest_helpers::error_response_obj(
            error_code::INVALID_INDEX,
            std::format("Failed to get struct at index {}", index),
            path,
            details
        );
    }

    // Bind to read current values
    auto bound_before = property_set{};
    prop->struct_registration(bound_before, item_ptr_before);

    // Build a vector of all fields with the one field updated
    std::vector<std::pair<std::string, std::string>> field_pairs;
    for (const auto& [fname, fprop] : bound_before.properties()) {
        if (fname == field_name) {
            // Convert the new value to string
            std::string value_str;
            if (value_json.is_string()) {
                value_str = value_json.get<std::string>();
            } else if (value_json.is_number()) {
                value_str = std::to_string(value_json.get<double>());
            } else if (value_json.is_boolean()) {
                value_str = value_json.get<bool>() ? "true" : "false";
            } else if (value_json.is_null()) {
                value_str = composite::properties::null_prop;
            } else {
                auto details = nlohmann::json::object();
                details["provided_type"] = value_json.type_name();
                return rest_helpers::error_response_obj(
                    error_code::INVALID_TYPE,
                    "Value must be a scalar (string, number, boolean, or null)",
                    path,
                    details
                );
            }
            field_pairs.emplace_back(fname, value_str);
        } else {
            // Keep existing value - serialize it
            auto type = fprop.type();
            std::string current_value;

            // Get current value as string
            if (type == "string") {
                current_value = *std::any_cast<std::string*>(fprop.value());
            } else if (type == "bool") {
                current_value = *std::any_cast<bool*>(fprop.value()) ? "true" : "false";
            } else if (type == "int32") {
                current_value = std::to_string(*std::any_cast<int32_t*>(fprop.value()));
            } else if (type == "uint32") {
                current_value = std::to_string(*std::any_cast<uint32_t*>(fprop.value()));
            } else if (type == "int16") {
                current_value = std::to_string(*std::any_cast<int16_t*>(fprop.value()));
            } else if (type == "uint16") {
                current_value = std::to_string(*std::any_cast<uint16_t*>(fprop.value()));
            } else if (type == "int64") {
                current_value = std::to_string(*std::any_cast<int64_t*>(fprop.value()));
            } else if (type == "uint64") {
                current_value = std::to_string(*std::any_cast<uint64_t*>(fprop.value()));
            } else if (type == "float") {
                current_value = std::to_string(*std::any_cast<float*>(fprop.value()));
            } else if (type == "double") {
                current_value = std::to_string(*std::any_cast<double*>(fprop.value()));
            } else if (type.ends_with("?")) {
                // Handle optional - just use empty for now
                current_value = "";
            }

            field_pairs.emplace_back(fname, current_value);
        }
    }

    // Update using the proper syntax: connections[index] with vector of pairs
    auto prop_path = std::format("{}[{}]", property_name, index);
    try {
        comp->set_properties({{prop_path, field_pairs}}, config_type::RUNTIME);
        comp->property_change_handler();
    } catch (const std::exception& ex) {
        auto details = nlohmann::json::object();
        details["exception"] = ex.what();
        return rest_helpers::error_response_obj(
            error_code::VALIDATION_FAILED,
            std::format("Failed to update field '{}': {}", field_name, ex.what()),
            path,
            details
        );
    }

    // Get the updated struct at the specified index
    auto* item_ptr = prop->struct_getter(index);
    if (!item_ptr) {
        auto details = nlohmann::json::object();
        details["index"] = index;

        return rest_helpers::error_response_obj(
            error_code::INVALID_INDEX,
            std::format("Failed to get struct at index {} after update", index),
            path,
            details
        );
    }

    // Bind the struct to a property_set
    auto bound = property_set{};
    prop->struct_registration(bound, item_ptr);

    // Get the updated field
    const auto& fields = bound.properties();
    auto it = fields.find(std::string{field_name});

    if (it == fields.end()) {
        auto details = nlohmann::json::object();
        details["field"] = field_name;

        return rest_helpers::error_response_obj(
            error_code::PROPERTY_NOT_FOUND,
            std::format("Field '{}' not found after update", field_name),
            path,
            details
        );
    }

    // Build response
    auto response = build_field_json(it->second, field_name, path);

    return rest_helpers::success_response(response);
}

// ============================================================================
// Validation & Discovery Helper Functions
// ============================================================================

auto property_handlers::build_schema_json(
    const property& prop,
    std::string_view property_name
) -> nlohmann::json {
    auto schema_obj = nlohmann::json::object();

    schema_obj["name"] = property_name;
    schema_obj["type"] = prop.type();
    schema_obj["units"] = prop.units();
    schema_obj["configurability"] = (prop.configurability() == config_type::RUNTIME) ? "runtime" : "initialize";

    // Add description (if available in the future)
    // schema_obj["description"] = prop.description();

    // Add default value (not currently tracked in property class)
    // schema_obj["default"] = ...;

    // Add constraints (not currently tracked in property class)
    // This would be added in the future to support validation
    // schema_obj["constraints"] = {
    //     {"min", ...},
    //     {"max", ...},
    //     {"allowed_values", [...]}
    // };

    return schema_obj;
}

auto property_handlers::matches_filter(
    const property& prop,
    const std::optional<std::string>& type_filter,
    const std::optional<std::string>& config_filter
) -> bool {
    // Check type filter
    if (type_filter.has_value()) {
        if (prop.type() != type_filter.value()) {
            return false;
        }
    }

    // Check configurability filter
    if (config_filter.has_value()) {
        auto config_str = (prop.configurability() == config_type::RUNTIME) ? "runtime" : "initialize";
        if (config_str != config_filter.value()) {
            return false;
        }
    }

    return true;
}

// ============================================================================
// Validation & Discovery Handler Implementations
// ============================================================================

auto property_handlers::validate_property_value(
    component* comp,
    std::string_view property_name,
    const nlohmann::json& request_body,
    std::string_view path
) -> httplib::Response {

    // Get the property
    auto [prop, error] = get_property_from_component(comp, property_name, path);
    if (error) {
        return *error;
    }

    // Extract value from request body
    if (!request_body.contains("value")) {
        auto details = nlohmann::json::object();
        details["required_field"] = "value";

        return rest_helpers::error_response_obj(
            error_code::MISSING_FIELD,
            "Request body must contain 'value' field",
            path,
            details
        );
    }

    const auto& value_json = request_body["value"];

    // Try to validate the value by attempting to set it
    // We'll use a try-catch to detect validation errors without actually applying
    try {
        // Convert JSON value to string for property system
        std::string value_str;
        if (value_json.is_string()) {
            value_str = value_json.get<std::string>();
        } else if (value_json.is_number()) {
            value_str = std::to_string(value_json.get<double>());
        } else if (value_json.is_boolean()) {
            value_str = value_json.get<bool>() ? "true" : "false";
        } else if (value_json.is_null()) {
            value_str = composite::properties::null_prop;
        } else {
            // Invalid type
            auto response = nlohmann::json::object();
            response["valid"] = false;

            auto errors_array = nlohmann::json::array();
            auto error_obj = nlohmann::json::object();
            error_obj["code"] = "INVALID_TYPE";
            error_obj["message"] = "Value must be a scalar (string, number, boolean, or null)";
            error_obj["field"] = "value";

            auto details = nlohmann::json::object();
            details["provided_type"] = value_json.type_name();
            error_obj["details"] = details;

            errors_array.push_back(error_obj);
            response["errors"] = errors_array;

            return rest_helpers::success_response(response);
        }

        // For a real validation, we would need to:
        // 1. Check type compatibility
        // 2. Check constraints (min/max, allowed values, etc.)
        // 3. Check if change listeners would accept the value
        //
        // Currently, the property system doesn't expose a "dry-run" validation
        // so we'll return a basic validation response

        // Basic type check
        auto response = nlohmann::json::object();
        response["valid"] = true;
        response["value"] = value_json;

        return rest_helpers::success_response(response);

    } catch (const std::exception& ex) {
        // Validation failed
        auto response = nlohmann::json::object();
        response["valid"] = false;

        auto errors_array = nlohmann::json::array();
        auto error_obj = nlohmann::json::object();
        error_obj["code"] = "VALIDATION_FAILED";
        error_obj["message"] = ex.what();
        error_obj["field"] = "value";

        errors_array.push_back(error_obj);
        response["errors"] = errors_array;

        return rest_helpers::success_response(response);
    }
}

auto property_handlers::get_property_schema(
    component* comp,
    std::string_view property_name,
    std::string_view path
) -> httplib::Response {

    // Get the property
    auto [prop, error] = get_property_from_component(comp, property_name, path);
    if (error) {
        return *error;
    }

    // Build schema response
    auto response = build_schema_json(*prop, property_name);

    return rest_helpers::success_response(response);
}

} // namespace composite::properties::rest
