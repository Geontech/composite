/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "types.hpp"
#include "errors.hpp"

#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace composite::properties {

// ============================================================================
// Type Classification Traits
// ============================================================================

/// Detect std::optional<T>
template<typename T>
struct is_optional : std::false_type {};

template<typename T>
struct is_optional<std::optional<T>> : std::true_type {};

template<typename T>
inline constexpr bool is_optional_v = is_optional<T>::value;

/// Extract inner type from std::optional<T>
template<typename T>
struct optional_inner { using type = T; };

template<typename T>
struct optional_inner<std::optional<T>> { using type = T; };

template<typename T>
using optional_inner_t = typename optional_inner<T>::type;

/// Detect std::vector<T>
template<typename T>
struct is_vector : std::false_type {};

template<typename T>
struct is_vector<std::vector<T>> : std::true_type {};

template<typename T>
inline constexpr bool is_vector_v = is_vector<T>::value;

/// Extract element type from std::vector<T>
template<typename T>
struct vector_element { using type = T; };

template<typename T>
struct vector_element<std::vector<T>> { using type = T; };

template<typename T>
using vector_element_t = typename vector_element<T>::type;

// ============================================================================
// property_traits - User Specializes for Struct Types
// ============================================================================

/**
 * @brief Trait for registering struct fields as properties
 *
 * Users must specialize this template for any struct type they want to use
 * as a property. The specialization must define a static register_fields function.
 *
 * Example:
 * @code
 * struct NetworkConfig {
 *     std::string host{"localhost"};
 *     uint16_t port{8080};
 * };
 *
 * template<>
 * struct composite::properties::property_traits<NetworkConfig> {
 *     static void register_fields(property_set& ps, NetworkConfig& c) {
 *         ps.add_property("host", c.host, runtime);
 *         ps.add_property("port", c.port, runtime);
 *     }
 * };
 * @endcode
 */
template<typename T, typename = void>
struct property_traits;  // Primary template - undefined for non-struct types

/// SFINAE helper to detect if property_traits<T> is specialized
template<typename T, typename = void>
struct has_property_traits : std::false_type {};

template<typename T>
struct has_property_traits<T, std::void_t<decltype(&property_traits<T>::register_fields)>>
    : std::true_type {};

template<typename T>
inline constexpr bool has_property_traits_v = has_property_traits<T>::value;

// ============================================================================
// Optional Struct Type Name
// ============================================================================

template<typename T, typename = void>
struct struct_type_name {
    static constexpr std::string_view value = "struct";
};

template<typename T>
struct struct_type_name<T, std::void_t<decltype(property_traits<T>::type_name)>> {
    static constexpr std::string_view value = std::string_view{property_traits<T>::type_name};
};

template<typename T>
inline constexpr std::string_view struct_type_name_v = struct_type_name<T>::value;

// ============================================================================
// Scalar Type Detection
// ============================================================================

/// Check if T is one of the supported scalar types
template<typename T>
struct is_scalar_type : std::bool_constant<
    std::is_same_v<T, bool> ||
    std::is_same_v<T, int16_t> ||
    std::is_same_v<T, uint16_t> ||
    std::is_same_v<T, int32_t> ||
    std::is_same_v<T, uint32_t> ||
    std::is_same_v<T, int64_t> ||
    std::is_same_v<T, uint64_t> ||
    std::is_same_v<T, float> ||
    std::is_same_v<T, double> ||
    std::is_same_v<T, std::string>
> {};

template<typename T>
inline constexpr bool is_scalar_type_v = is_scalar_type<T>::value;

// ============================================================================
// Type Name Strings (for serialization/debugging)
// ============================================================================

template<typename T>
struct type_name_impl { static constexpr const char* value = "unknown"; };

template<> struct type_name_impl<bool> { static constexpr const char* value = "bool"; };
template<> struct type_name_impl<int16_t> { static constexpr const char* value = "int16"; };
template<> struct type_name_impl<uint16_t> { static constexpr const char* value = "uint16"; };
template<> struct type_name_impl<int32_t> { static constexpr const char* value = "int32"; };
template<> struct type_name_impl<uint32_t> { static constexpr const char* value = "uint32"; };
template<> struct type_name_impl<int64_t> { static constexpr const char* value = "int64"; };
template<> struct type_name_impl<uint64_t> { static constexpr const char* value = "uint64"; };
template<> struct type_name_impl<float> { static constexpr const char* value = "float"; };
template<> struct type_name_impl<double> { static constexpr const char* value = "double"; };
template<> struct type_name_impl<std::string> { static constexpr const char* value = "string"; };

template<typename T>
inline constexpr const char* type_name_v = type_name_impl<T>::value;

// ============================================================================
// String Conversion - from_string
// ============================================================================

/**
 * @brief Convert a string to a typed value
 * @throws value_error if conversion fails
 */
template<typename T>
auto from_string(std::string_view sv, std::string_view prop_name = "") -> T;

// Bool specialization
template<>
inline auto from_string<bool>(std::string_view sv, std::string_view) -> bool {
    if (sv == "true" || sv == "1" || sv == "yes" || sv == "on") return true;
    if (sv == "false" || sv == "0" || sv == "no" || sv == "off") return false;
    throw value_error("", sv, "expected boolean");
}

// String specialization (passthrough)
template<>
inline auto from_string<std::string>(std::string_view sv, std::string_view) -> std::string {
    return std::string{sv};
}

// Integer types using std::from_chars
template<typename T>
requires std::is_integral_v<T> && (!std::is_same_v<T, bool>)
inline auto from_string(std::string_view sv, std::string_view prop_name) -> T {
    T result{};
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), result);
    if (ec == std::errc{} && ptr == sv.data() + sv.size()) {
        return result;
    }
    throw value_error(prop_name, sv, "invalid integer");
}

// Floating point types
template<>
inline auto from_string<float>(std::string_view sv, std::string_view prop_name) -> float {
    try {
        std::size_t pos{};
        float result = std::stof(std::string{sv}, &pos);
        if (pos == sv.size()) return result;
    } catch (...) {}
    throw value_error(prop_name, sv, "invalid float");
}

template<>
inline auto from_string<double>(std::string_view sv, std::string_view prop_name) -> double {
    try {
        std::size_t pos{};
        double result = std::stod(std::string{sv}, &pos);
        if (pos == sv.size()) return result;
    } catch (...) {}
    throw value_error(prop_name, sv, "invalid double");
}

// ============================================================================
// String Conversion - to_string
// ============================================================================

/**
 * @brief Convert a typed value to a string
 */
template<typename T>
auto to_string(const T& value) -> std::string;

// Bool specialization
template<>
inline auto to_string<bool>(const bool& value) -> std::string {
    return value ? "true" : "false";
}

// String specialization (passthrough)
template<>
inline auto to_string<std::string>(const std::string& value) -> std::string {
    return value;
}

// Numeric types
template<typename T>
requires std::is_arithmetic_v<T> && (!std::is_same_v<T, bool>)
inline auto to_string(const T& value) -> std::string {
    return std::to_string(value);
}

} // namespace composite::properties
