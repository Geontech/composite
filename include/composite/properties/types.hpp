/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <any>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace composite::properties {

// Forward declaration
class property_set;

/**
 * @brief Configurability of a property
 */
enum class config_type {
    INITIALIZE, ///< Can only be set during initialization
    RUNTIME     ///< Can be modified at runtime
};

// Convenience aliases
inline constexpr auto initialize = config_type::INITIALIZE;
inline constexpr auto runtime = config_type::RUNTIME;

/**
 * @brief Type of change that occurred to a property
 *
 * Passed to contextual change listeners to indicate what kind of change happened.
 */
enum class change_type {
    SET,    ///< Value was set (for optional: from nullopt to value)
    MODIFY, ///< Value was modified (from one value to another)
    RESET   ///< Value was reset (for optional: to nullopt; for scalars: to default)
};

// Convenience aliases
inline constexpr auto set_change = change_type::SET;
inline constexpr auto modify_change = change_type::MODIFY;
inline constexpr auto reset_change = change_type::RESET;

/**
 * @brief Variant holding pointers to scalar types
 */
using scalar_types =
    std::variant<bool*, int16_t*, uint16_t*, int32_t*, uint32_t*, int64_t*, uint64_t*, float*, double*, std::string*>;

/**
 * @brief Variant holding pointers to optional scalar types
 */
using optional_types =
    std::variant<std::optional<bool>*, std::optional<int16_t>*, std::optional<uint16_t>*, std::optional<int32_t>*,
                 std::optional<uint32_t>*, std::optional<int64_t>*, std::optional<uint64_t>*, std::optional<float>*,
                 std::optional<double>*, std::optional<std::string>*>;

/**
 * @brief Variant holding pointers to vector types (lists of scalars)
 */
using list_types =
    std::variant<std::vector<bool>*, std::vector<int16_t>*, std::vector<uint16_t>*, std::vector<int32_t>*,
                 std::vector<uint32_t>*, std::vector<int64_t>*, std::vector<uint64_t>*, std::vector<float>*,
                 std::vector<double>*, std::vector<std::string>*>;

/**
 * @brief Type-erased accessor for struct and list-of-struct properties
 *
 * This replaces the previous 6-function-pointer struct_operations pattern
 * with a cleaner interface that handles both single structs and vectors of structs.
 */
struct struct_accessor {
    void* data{nullptr};

    /// Register the fields of this struct type into a property_set
    std::function<void(property_set&, void*)> register_fields;

    /// Reset the struct to its default-constructed state
    std::function<void(void*)> reset;

    /// Snapshot the struct or list for rollback
    std::function<std::any(const void*)> snapshot;

    /// Restore the struct or list from a snapshot
    std::function<void(void*, const std::any&)> restore;

    // The following are only used for list-of-struct:

    /// Get the number of elements (empty for single struct)
    std::function<std::size_t(const void*)> size;

    /// Get mutable pointer to element at index (empty for single struct)
    std::function<void*(void*, std::size_t)> get_element;

    /// Get const pointer to element at index (empty for single struct)
    std::function<const void*(const void*, std::size_t)> get_element_const;

    /// Emplace a default element at the back, return its index (empty for single struct)
    std::function<std::size_t(void*)> emplace_back;

    /// Erase element at index (empty for single struct)
    std::function<void(void*, std::size_t)> erase;

    /// Check if this is a list-of-struct (vs single struct)
    [[nodiscard]] auto is_list() const noexcept -> bool { return static_cast<bool>(size); }
};

/**
 * @brief Unified property value type
 *
 * All property values are stored as one of these variants:
 * - scalar_types: Direct pointer to a scalar member
 * - optional_types: Pointer to std::optional<scalar>
 * - list_types: Pointer to std::vector<scalar>
 * - struct_accessor: Type-erased accessor for struct or list-of-struct
 */
using property_value = std::variant<scalar_types, optional_types, list_types, struct_accessor>;

/**
 * @brief Sentinel value for null/reset operations
 */
inline constexpr std::string_view null_value = "\0__NULL__\0";

/**
 * @brief Check if a string value represents null
 */
[[nodiscard]] inline auto is_null_value(std::string_view sv) noexcept -> bool {
    return sv == null_value || sv == "null" || sv == "~";
}

} // namespace composite::properties
