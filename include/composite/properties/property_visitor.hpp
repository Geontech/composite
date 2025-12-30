/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "types.hpp"
#include "property_traits.hpp"
#include "errors.hpp"

#include <string>
#include <string_view>

namespace composite::properties {

// ============================================================================
// Visitor for Setting Scalar Values from String
// ============================================================================

/**
 * @brief Visitor that sets a scalar value from a string
 */
struct set_scalar_visitor {
    std::string_view value;
    std::string_view prop_name;

    template<typename T>
    auto operator()(T* ptr) const -> void {
        *ptr = from_string<T>(value, prop_name);
    }
};

/**
 * @brief Visitor that resets a scalar value to its default
 */
struct reset_scalar_visitor {
    template<typename T>
    auto operator()(T* ptr) const -> void {
        *ptr = T{};
    }
};

// ============================================================================
// Visitor for Setting Optional Values from String
// ============================================================================

/**
 * @brief Visitor that sets an optional value from a string (or resets if null)
 */
struct set_optional_visitor {
    std::string_view value;
    std::string_view prop_name;
    bool is_null;

    template<typename T>
    auto operator()(std::optional<T>* ptr) const -> void {
        if (is_null) {
            ptr->reset();
        } else {
            *ptr = from_string<T>(value, prop_name);
        }
    }
};

// ============================================================================
// Visitor for Getting Scalar Values as String
// ============================================================================

/**
 * @brief Visitor that converts a scalar value to string
 */
struct get_string_visitor {
    template<typename T>
    auto operator()(const T* ptr) const -> std::string {
        return to_string(*ptr);
    }
};

// ============================================================================
// Visitor for Getting Optional Values as String
// ============================================================================

/**
 * @brief Visitor that converts an optional value to string (or empty if nullopt)
 */
struct get_optional_string_visitor {
    template<typename T>
    auto operator()(const std::optional<T>* ptr) const -> std::optional<std::string> {
        if (ptr->has_value()) {
            return to_string(ptr->value());
        }
        return std::nullopt;
    }
};

// ============================================================================
// Visitor for List Operations
// ============================================================================

/**
 * @brief Visitor that gets the size of a list
 */
struct list_size_visitor {
    template<typename T>
    auto operator()(const std::vector<T>* ptr) const -> std::size_t {
        return ptr->size();
    }
};

/**
 * @brief Visitor that sets a list item from a string
 */
struct set_list_item_visitor {
    std::size_t index;
    std::string_view value;
    std::string_view prop_name;

    template<typename T>
    auto operator()(std::vector<T>* ptr) const -> void {
        if (index >= ptr->size()) {
            throw index_error(prop_name, index, ptr->size());
        }
        (*ptr)[index] = from_string<T>(value, prop_name);
    }
};

/**
 * @brief Visitor that gets a list item as a string
 */
struct get_list_item_visitor {
    std::size_t index;
    std::string_view prop_name;

    template<typename T>
    auto operator()(const std::vector<T>* ptr) const -> std::string {
        if (index >= ptr->size()) {
            throw index_error(prop_name, index, ptr->size());
        }
        return to_string((*ptr)[index]);
    }
};

/**
 * @brief Visitor that appends an item to a list from a string
 */
struct append_list_item_visitor {
    std::string_view value;
    std::string_view prop_name;

    template<typename T>
    auto operator()(std::vector<T>* ptr) const -> std::size_t {
        ptr->push_back(from_string<T>(value, prop_name));
        return ptr->size() - 1;
    }
};

/**
 * @brief Visitor that erases an item from a list
 */
struct erase_list_item_visitor {
    std::size_t index;
    std::string_view prop_name;

    template<typename T>
    auto operator()(std::vector<T>* ptr) const -> void {
        if (index >= ptr->size()) {
            throw index_error(prop_name, index, ptr->size());
        }
        ptr->erase(ptr->begin() + static_cast<std::ptrdiff_t>(index));
    }
};

/**
 * @brief Visitor that inserts an item into a list from a string
 */
struct insert_list_item_visitor {
    std::size_t index;
    std::string_view value;
    std::string_view prop_name;

    template<typename T>
    auto operator()(std::vector<T>* ptr) const -> void {
        if (index > ptr->size()) {
            throw index_error(prop_name, index, ptr->size());
        }
        ptr->insert(ptr->begin() + static_cast<std::ptrdiff_t>(index),
                    from_string<T>(value, prop_name));
    }
};

/**
 * @brief Visitor that clears a list
 */
struct clear_list_visitor {
    template<typename T>
    auto operator()(std::vector<T>* ptr) const -> void {
        ptr->clear();
    }
};

// ============================================================================
// Helper Functions for Visiting Property Values
// ============================================================================

/**
 * @brief Visit a scalar property value
 */
template<typename Visitor>
auto visit_scalar(property_value& val, Visitor&& vis) -> decltype(auto) {
    return std::visit(std::forward<Visitor>(vis), std::get<scalar_types>(val));
}

template<typename Visitor>
auto visit_scalar(const property_value& val, Visitor&& vis) -> decltype(auto) {
    return std::visit(std::forward<Visitor>(vis), std::get<scalar_types>(val));
}

/**
 * @brief Visit an optional property value
 */
template<typename Visitor>
auto visit_optional(property_value& val, Visitor&& vis) -> decltype(auto) {
    return std::visit(std::forward<Visitor>(vis), std::get<optional_types>(val));
}

template<typename Visitor>
auto visit_optional(const property_value& val, Visitor&& vis) -> decltype(auto) {
    return std::visit(std::forward<Visitor>(vis), std::get<optional_types>(val));
}

/**
 * @brief Visit a list property value
 */
template<typename Visitor>
auto visit_list(property_value& val, Visitor&& vis) -> decltype(auto) {
    return std::visit(std::forward<Visitor>(vis), std::get<list_types>(val));
}

template<typename Visitor>
auto visit_list(const property_value& val, Visitor&& vis) -> decltype(auto) {
    return std::visit(std::forward<Visitor>(vis), std::get<list_types>(val));
}

/**
 * @brief Get the struct_accessor from a property value
 */
inline auto get_struct_accessor(property_value& val) -> struct_accessor& {
    return std::get<struct_accessor>(val);
}

inline auto get_struct_accessor(const property_value& val) -> const struct_accessor& {
    return std::get<struct_accessor>(val);
}

} // namespace composite::properties
