/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "types.hpp"
#include "property_traits.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace composite::properties {

// Forward declaration
class property_set;

/**
 * @brief A single property with metadata and change notification
 *
 * Properties store a pointer to the actual data via the property_value variant.
 * They also hold metadata (units, configurability) and optional change listeners.
 */
class property {
public:
    /// Change listener signature for scalar/struct properties
    using change_listener_fn = std::function<bool()>;

    /// Change listener signature for list properties (receives modified index)
    using indexed_change_listener_fn = std::function<bool(std::size_t)>;

    /// Contextual change listener signature (receives change type)
    using contextual_change_listener_fn = std::function<bool(change_type)>;

    /// Contextual indexed change listener signature (receives index and change type)
    using contextual_indexed_change_listener_fn = std::function<bool(std::size_t, change_type)>;

    // ========================================================================
    // Constructors for different property types
    // ========================================================================

    /// Construct from a scalar pointer
    template<typename T>
    requires is_scalar_type_v<T>
    explicit property(T* ptr)
        : m_value{scalar_types{ptr}}
        , m_type_name{type_name_v<T>} {}

    /// Construct from an optional pointer
    template<typename T>
    requires is_scalar_type_v<T>
    explicit property(std::optional<T>* ptr)
        : m_value{optional_types{ptr}}
        , m_type_name{std::string{type_name_v<T>} + "?"} {}

    /// Construct from a vector pointer (list of scalars)
    template<typename T>
    requires is_scalar_type_v<T>
    explicit property(std::vector<T>* ptr)
        : m_value{list_types{ptr}}
        , m_type_name{std::string{"[]"} + type_name_v<T>} {}

    /// Construct from a struct accessor
    explicit property(struct_accessor acc, bool is_list = false)
        : m_value{std::move(acc)}
        , m_type_name{is_list ? "[]struct" : "struct"} {}

    // ========================================================================
    // Fluent Metadata Setters
    // ========================================================================

    /// Set the units string (e.g., "Hz", "ms", "dB")
    auto units(std::string_view u) -> property& {
        m_units = u;
        return *this;
    }

    /// Set the configurability (INITIALIZE or RUNTIME)
    auto configurability(config_type c) -> property& {
        m_config = c;
        return *this;
    }

    /// Set a change listener for scalar/struct properties
    auto change_listener(change_listener_fn fn) -> property& {
        m_on_change = std::move(fn);
        return *this;
    }

    /// Set an indexed change listener for list properties
    auto change_listener(indexed_change_listener_fn fn) -> property& {
        m_on_index_change = std::move(fn);
        return *this;
    }

    /// Set a contextual change listener for scalar/struct properties (receives change type)
    auto change_listener(contextual_change_listener_fn fn) -> property& {
        m_on_change_ctx = std::move(fn);
        return *this;
    }

    /// Set a contextual indexed change listener for list properties (receives index and change type)
    auto change_listener(contextual_indexed_change_listener_fn fn) -> property& {
        m_on_index_change_ctx = std::move(fn);
        return *this;
    }

    // ========================================================================
    // Metadata Getters
    // ========================================================================

    [[nodiscard]] auto units() const noexcept -> std::string_view {
        return m_units;
    }

    [[nodiscard]] auto configurability() const noexcept -> config_type {
        return m_config;
    }

    [[nodiscard]] auto type_name() const noexcept -> std::string_view {
        return m_type_name;
    }

    auto set_type_name(std::string_view type_name) -> void {
        m_type_name = type_name;
    }

    // ========================================================================
    // Type Queries
    // ========================================================================

    [[nodiscard]] auto is_scalar() const noexcept -> bool {
        return std::holds_alternative<scalar_types>(m_value);
    }

    [[nodiscard]] auto is_optional() const noexcept -> bool {
        return std::holds_alternative<optional_types>(m_value);
    }

    [[nodiscard]] auto is_list() const noexcept -> bool {
        if (std::holds_alternative<list_types>(m_value)) return true;
        if (auto* acc = std::get_if<struct_accessor>(&m_value)) {
            return acc->is_list();
        }
        return false;
    }

    [[nodiscard]] auto is_struct() const noexcept -> bool {
        if (auto* acc = std::get_if<struct_accessor>(&m_value)) {
            return !acc->is_list();
        }
        return false;
    }

    [[nodiscard]] auto is_struct_list() const noexcept -> bool {
        if (auto* acc = std::get_if<struct_accessor>(&m_value)) {
            return acc->is_list();
        }
        return false;
    }

    // ========================================================================
    // Value Access
    // ========================================================================

    [[nodiscard]] auto value() const noexcept -> const property_value& {
        return m_value;
    }

    [[nodiscard]] auto value() noexcept -> property_value& {
        return m_value;
    }

    // ========================================================================
    // Nested Property Set (for struct properties)
    // ========================================================================

    /// Get the nested property_set for struct properties
    [[nodiscard]] auto nested() const -> const property_set* {
        return m_nested.get();
    }

    [[nodiscard]] auto nested() -> property_set* {
        return m_nested.get();
    }

    /// Set the nested property_set
    auto set_nested(std::unique_ptr<property_set> ps) -> void {
        m_nested = std::move(ps);
    }

    // ========================================================================
    // Change Notification
    // ========================================================================

    /// Invoke change listener (for scalar/struct properties) without context
    /// @returns true if accepted (or no listener), false if rejected
    [[nodiscard]] auto notify_change() const -> bool {
        if (m_on_change) {
            return m_on_change();
        }
        return true;
    }

    /// Invoke change listener with context (for scalar/struct properties)
    /// Calls contextual listener if set, otherwise falls back to simple listener
    /// @returns true if accepted (or no listener), false if rejected
    [[nodiscard]] auto notify_change(change_type ctx) const -> bool {
        if (m_on_change_ctx) {
            return m_on_change_ctx(ctx);
        }
        if (m_on_change) {
            return m_on_change();
        }
        return true;
    }

    /// Invoke indexed change listener (for list properties) without context
    /// @returns true if accepted (or no listener), false if rejected
    [[nodiscard]] auto notify_change(std::size_t index) const -> bool {
        if (m_on_index_change) {
            return m_on_index_change(index);
        }
        return true;
    }

    /// Invoke indexed change listener with context (for list properties)
    /// Calls contextual listener if set, otherwise falls back to simple listener
    /// @returns true if accepted (or no listener), false if rejected
    [[nodiscard]] auto notify_change(std::size_t index, change_type ctx) const -> bool {
        if (m_on_index_change_ctx) {
            return m_on_index_change_ctx(index, ctx);
        }
        if (m_on_index_change) {
            return m_on_index_change(index);
        }
        return true;
    }

    /// Check if a change listener is set
    [[nodiscard]] auto has_change_listener() const noexcept -> bool {
        return static_cast<bool>(m_on_change) || static_cast<bool>(m_on_index_change) ||
               static_cast<bool>(m_on_change_ctx) || static_cast<bool>(m_on_index_change_ctx);
    }

private:
    property_value m_value;
    std::string m_type_name;
    std::string m_units;
    config_type m_config{config_type::INITIALIZE};

    change_listener_fn m_on_change;
    indexed_change_listener_fn m_on_index_change;
    contextual_change_listener_fn m_on_change_ctx;
    contextual_indexed_change_listener_fn m_on_index_change_ctx;

    /// Nested property_set for struct properties
    std::unique_ptr<property_set> m_nested;
};

} // namespace composite::properties
