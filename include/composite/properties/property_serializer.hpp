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

#include "composite/property_set.hpp"
#include <nlohmann/json.hpp>
#include <format>

namespace composite {

class property_serializer {
public:
    // Serialize a single property to JSON
    static auto to_json(nlohmann::json& json_obj, const property& prop) -> void {
        auto type = prop.type();
        json_obj["type"] = type;
        json_obj["units"] = prop.units();
        json_obj["configurability"] = (static_cast<int>(prop.configurability()) == 1) ? "runtime" : "initialize";

        // Handle lists of structs (special case)
        if (prop.is_list() && type == "[]struct") {
            nlohmann::json value_array = nlohmann::json::array();
            for (auto i = std::size_t{}; i < prop.struct_list_size(); ++i) {
                nlohmann::json item_obj;
                auto bound = property_set{};
                auto* item_ptr = prop.struct_getter(i);
                if (!item_ptr) {
                    continue;
                }
                prop.struct_registration(bound, item_ptr);
                to_json(item_obj, bound);
                value_array.push_back(item_obj);
            }
            json_obj["value"] = value_array;
            return;
        }

        // Handle lists of scalar types
        if (prop.is_list()) {
            serialize_list(json_obj, prop);
            return;
        }

        // Handle structured properties
        if (prop.is_structured()) {
            nlohmann::json nested;
            to_json(nested, prop.structured());
            json_obj["value"] = nested;
            return;
        }

        // Handle scalar and optional types
        serialize_scalar(json_obj, prop);
    }

    // Serialize a property_set to JSON
    static auto to_json(nlohmann::json& json_obj, const property_set& set) -> void {
        for (const auto& [name, prop] : set.properties()) {
            auto prop_obj = nlohmann::json::object();
            to_json(prop_obj, prop);
            json_obj[name] = prop_obj;
        }
    }

private:
    // Serialize list property
    static auto serialize_list(nlohmann::json& json_obj, const property& prop) -> void {
        auto type = prop.type();

        // Handle lists of scalar types
        if (type == "[]bool") {
            json_obj["value"] = *std::any_cast<std::vector<bool>*>(prop.value());
        } else if (type == "[]string") {
            json_obj["value"] = *std::any_cast<std::vector<std::string>*>(prop.value());
        } else if (type == "[]int16") {
            json_obj["value"] = *std::any_cast<std::vector<int16_t>*>(prop.value());
        } else if (type == "[]uint16") {
            json_obj["value"] = *std::any_cast<std::vector<uint16_t>*>(prop.value());
        } else if (type == "[]int32") {
            json_obj["value"] = *std::any_cast<std::vector<int32_t>*>(prop.value());
        } else if (type == "[]uint32") {
            json_obj["value"] = *std::any_cast<std::vector<uint32_t>*>(prop.value());
        } else if (type == "[]int64") {
            json_obj["value"] = *std::any_cast<std::vector<int64_t>*>(prop.value());
        } else if (type == "[]uint64") {
            json_obj["value"] = *std::any_cast<std::vector<uint64_t>*>(prop.value());
        } else if (type == "[]float") {
            json_obj["value"] = *std::any_cast<std::vector<float>*>(prop.value());
        } else if (type == "[]double") {
            json_obj["value"] = *std::any_cast<std::vector<double>*>(prop.value());
        }

        // Convert each element to string for consistency with original behavior
        for (auto& v : json_obj["value"]) {
            v = v.dump();
        }
    }

    // Serialize scalar/optional property
    static auto serialize_scalar(nlohmann::json& json_obj, const property& prop) -> void {
        auto type = prop.type();

        if (type == "bool") {
            json_obj["value"] = std::format("{}", *std::any_cast<bool*>(prop.value()));
        } else if (type == "bool?") {
            auto opt_val = *std::any_cast<std::optional<bool>*>(prop.value());
            if (opt_val.has_value()) {
                json_obj["value"] = std::format("{}", opt_val.value());
            } else {
                json_obj["value"] = nullptr;
            }
        } else if (type == "string") {
            json_obj["value"] = *std::any_cast<std::string*>(prop.value());
        } else if (type == "string?") {
            auto opt_val = *std::any_cast<std::optional<std::string>*>(prop.value());
            if (opt_val.has_value()) {
                json_obj["value"] = opt_val.value();
            } else {
                json_obj["value"] = nullptr;
            }
        } else if (type == "int16") {
            json_obj["value"] = std::to_string(*std::any_cast<int16_t*>(prop.value()));
        } else if (type == "int16?") {
            auto opt_val = *std::any_cast<std::optional<int16_t>*>(prop.value());
            if (opt_val.has_value()) {
                json_obj["value"] = std::to_string(opt_val.value());
            } else {
                json_obj["value"] = nullptr;
            }
        } else if (type == "uint16") {
            json_obj["value"] = std::to_string(*std::any_cast<uint16_t*>(prop.value()));
        } else if (type == "uint16?") {
            auto opt_val = *std::any_cast<std::optional<uint16_t>*>(prop.value());
            if (opt_val.has_value()) {
                json_obj["value"] = std::to_string(opt_val.value());
            } else {
                json_obj["value"] = nullptr;
            }
        } else if (type == "int32") {
            json_obj["value"] = std::to_string(*std::any_cast<int32_t*>(prop.value()));
        } else if (type == "int32?") {
            auto opt_val = *std::any_cast<std::optional<int32_t>*>(prop.value());
            if (opt_val.has_value()) {
                json_obj["value"] = std::to_string(opt_val.value());
            } else {
                json_obj["value"] = nullptr;
            }
        } else if (type == "uint32") {
            json_obj["value"] = std::to_string(*std::any_cast<uint32_t*>(prop.value()));
        } else if (type == "uint32?") {
            auto opt_val = *std::any_cast<std::optional<uint32_t>*>(prop.value());
            if (opt_val.has_value()) {
                json_obj["value"] = std::to_string(opt_val.value());
            } else {
                json_obj["value"] = nullptr;
            }
        } else if (type == "int64") {
            json_obj["value"] = std::to_string(*std::any_cast<int64_t*>(prop.value()));
        } else if (type == "int64?") {
            auto opt_val = *std::any_cast<std::optional<int64_t>*>(prop.value());
            if (opt_val.has_value()) {
                json_obj["value"] = std::to_string(opt_val.value());
            } else {
                json_obj["value"] = nullptr;
            }
        } else if (type == "uint64") {
            json_obj["value"] = std::to_string(*std::any_cast<uint64_t*>(prop.value()));
        } else if (type == "uint64?") {
            auto opt_val = *std::any_cast<std::optional<uint64_t>*>(prop.value());
            if (opt_val.has_value()) {
                json_obj["value"] = std::to_string(opt_val.value());
            } else {
                json_obj["value"] = nullptr;
            }
        } else if (type == "float") {
            json_obj["value"] = std::to_string(*std::any_cast<float*>(prop.value()));
        } else if (type == "float?") {
            auto opt_val = *std::any_cast<std::optional<float>*>(prop.value());
            if (opt_val.has_value()) {
                json_obj["value"] = std::to_string(opt_val.value());
            } else {
                json_obj["value"] = nullptr;
            }
        } else if (type == "double") {
            json_obj["value"] = std::to_string(*std::any_cast<double*>(prop.value()));
        } else if (type == "double?") {
            auto opt_val = *std::any_cast<std::optional<double>*>(prop.value());
            if (opt_val.has_value()) {
                json_obj["value"] = std::to_string(opt_val.value());
            } else {
                json_obj["value"] = nullptr;
            }
        }
    }
}; // class property_serializer

} // namespace composite
