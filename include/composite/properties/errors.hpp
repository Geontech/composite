/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <format>
#include <stdexcept>
#include <string>
#include <string_view>

namespace composite::properties {

/**
 * @brief Base class for all property-related errors
 */
class property_error : public std::runtime_error {
public:
    explicit property_error(const std::string& msg) : std::runtime_error(msg) {}
};

/**
 * @brief Thrown when a property name is not found
 */
class key_error : public property_error {
public:
    key_error(std::string_view property_name) :
        property_error(std::format("unknown property '{}'", property_name)),
        name{property_name} {}

    std::string name;
};

/**
 * @brief Thrown when a type mismatch occurs during property access
 */
class type_error : public property_error {
public:
    type_error(std::string_view property_name, std::string_view expected_type) :
        property_error(std::format("type mismatch for '{}': expected {}", property_name, expected_type)),
        name{property_name},
        expected{expected_type} {}

    std::string name;
    std::string expected;
};

/**
 * @brief Thrown when a value cannot be parsed or is out of range
 */
class value_error : public property_error {
public:
    value_error(std::string_view property_name, std::string_view value, std::string_view reason = "") :
        property_error(reason.empty()
            ? std::format("invalid value '{}' for '{}'", value, property_name)
            : std::format("invalid value '{}' for '{}': {}", value, property_name, reason)
        ),
        name{property_name},
        bad_value{value} {}

    std::string name;
    std::string bad_value;
};

/**
 * @brief Thrown when attempting to modify a non-runtime-configurable property
 */
class config_error : public property_error {
public:
    explicit config_error(std::string_view property_name) :
        property_error(std::format("property '{}' is not runtime configurable", property_name)),
        name{property_name} {}

    std::string name;
};

/**
 * @brief Thrown when a list index is out of bounds
 */
class index_error : public property_error {
public:
    index_error(std::string_view property_name, std::size_t index, std::size_t size) :
        property_error(std::format("index {} out of bounds for '{}' (size {})", index, property_name, size)),
        name{property_name},
        requested_index{index},
        list_size{size} {}

    std::string name;
    std::size_t requested_index;
    std::size_t list_size;
};

/**
 * @brief Thrown when a change listener rejects a property update
 */
class listener_rejected : public property_error {
public:
    explicit listener_rejected(std::string_view property_name) :
        property_error(std::format("change listener rejected update to '{}'", property_name)),
        name{property_name} {}

    std::string name;
};

} // namespace composite::properties
