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

#include "property_metadata.hpp"

#include <any>
#include <optional>
#include <vector>

namespace composite::properties {

template <typename Visitor>
auto visit_by_type_name(std::string_view type, Visitor&& visitor) -> decltype(auto) {
    if (type == "bool") { return visitor.template operator()<bool>(); }
    if (type == "bool?") { return visitor.template operator()<std::optional<bool>>(); }
    if (type == "string") { return visitor.template operator()<std::string>(); }
    if (type == "string?") { return visitor.template operator()<std::optional<std::string>>(); }
    if (type == "int16") { return visitor.template operator()<int16_t>(); }
    if (type == "int16?") { return visitor.template operator()<std::optional<int16_t>>(); }
    if (type == "uint16") { return visitor.template operator()<uint16_t>(); }
    if (type == "uint16?") { return visitor.template operator()<std::optional<uint16_t>>(); }
    if (type == "int32") { return visitor.template operator()<int32_t>(); }
    if (type == "int32?") { return visitor.template operator()<std::optional<int32_t>>(); }
    if (type == "uint32") { return visitor.template operator()<uint32_t>(); }
    if (type == "uint32?") { return visitor.template operator()<std::optional<uint32_t>>(); }
    if (type == "int64") { return visitor.template operator()<int64_t>(); }
    if (type == "int64?") { return visitor.template operator()<std::optional<int64_t>>(); }
    if (type == "uint64") { return visitor.template operator()<uint64_t>(); }
    if (type == "uint64?") { return visitor.template operator()<std::optional<uint64_t>>(); }
    if (type == "float") { return visitor.template operator()<float>(); }
    if (type == "float?") { return visitor.template operator()<std::optional<float>>(); }
    if (type == "double") { return visitor.template operator()<double>(); }
    if (type == "double?") { return visitor.template operator()<std::optional<double>>(); }

    throw type_error("", "", type);
}

// Visit base types (without [] or ? modifiers)
template <typename Visitor>
auto visit_base_type(std::string_view type, Visitor&& visitor) -> decltype(auto) {
    if (type == "bool") { return visitor.template operator()<bool>(); }
    if (type == "string") { return visitor.template operator()<std::string>(); }
    if (type == "int16") { return visitor.template operator()<int16_t>(); }
    if (type == "uint16") { return visitor.template operator()<uint16_t>(); }
    if (type == "int32") { return visitor.template operator()<int32_t>(); }
    if (type == "uint32") { return visitor.template operator()<uint32_t>(); }
    if (type == "int64") { return visitor.template operator()<int64_t>(); }
    if (type == "uint64") { return visitor.template operator()<uint64_t>(); }
    if (type == "float") { return visitor.template operator()<float>(); }
    if (type == "double") { return visitor.template operator()<double>(); }

    throw type_error("", "", type);
}

// Visit list types
template <typename Visitor>
auto visit_list_type(std::string_view type, Visitor&& visitor) -> decltype(auto) {
    if (type == "[]bool") { return visitor.template operator()<std::vector<bool>>(); }
    if (type == "[]string") { return visitor.template operator()<std::vector<std::string>>(); }
    if (type == "[]int16") { return visitor.template operator()<std::vector<int16_t>>(); }
    if (type == "[]uint16") { return visitor.template operator()<std::vector<uint16_t>>(); }
    if (type == "[]int32") { return visitor.template operator()<std::vector<int32_t>>(); }
    if (type == "[]uint32") { return visitor.template operator()<std::vector<uint32_t>>(); }
    if (type == "[]int64") { return visitor.template operator()<std::vector<int64_t>>(); }
    if (type == "[]uint64") { return visitor.template operator()<std::vector<uint64_t>>(); }
    if (type == "[]float") { return visitor.template operator()<std::vector<float>>(); }
    if (type == "[]double") { return visitor.template operator()<std::vector<double>>(); }

    throw type_error("", "", type);
}

} // namespace composite::properties
