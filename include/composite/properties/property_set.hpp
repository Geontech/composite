/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "property.hpp"
#include "property_path.hpp"
#include "property_visitor.hpp"

#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace composite::properties {

/**
 * @brief Container for properties with registration and access APIs
 *
 * The property_set provides a simplified, type-safe interface for managing properties.
 * Properties are registered using the add_property() method which auto-detects the property
 * type (scalar, optional, list, struct, or list-of-struct) based on the member type.
 *
 * For struct types, users must specialize property_traits<T> to define how fields are registered.
 */
class property_set {
public:
    using property_map = std::map<std::string, property, std::less<>>;

    // ========================================================================
    // Registration API - Auto-detects property type via overloading
    // ========================================================================

    /**
     * @brief Register a scalar property
     */
    template<typename T>
    requires is_scalar_type_v<T>
    auto add(std::string_view name,
             T& ref,
             config_type config = config_type::INITIALIZE,
             std::string_view units = "") -> property& {
        auto [it, _] = m_props.try_emplace(std::string{name}, &ref);
        return it->second.configurability(config).units(units);
    }

    /**
     * @brief Register an optional scalar property
     */
    template<typename T>
    requires is_scalar_type_v<T>
    auto add(std::string_view name,
             std::optional<T>& ref,
             config_type config = config_type::INITIALIZE,
             std::string_view units = "") -> property& {
        auto [it, _] = m_props.try_emplace(std::string{name}, &ref);
        return it->second.configurability(config).units(units);
    }

    /**
     * @brief Register a list property (vector of scalars)
     */
    template<typename T>
    requires is_scalar_type_v<T>
    auto add(std::string_view name,
             std::vector<T>& ref,
             config_type config = config_type::INITIALIZE,
             std::string_view units = "") -> property& {
        auto [it, _] = m_props.try_emplace(std::string{name}, &ref);
        return it->second.configurability(config).units(units);
    }

    /**
     * @brief Register a struct property (requires property_traits<T> specialization)
     */
    template<typename T>
    requires has_property_traits_v<T>
    auto add(std::string_view name,
             T& ref,
             config_type config = config_type::INITIALIZE,
             std::string_view /*units*/ = "") -> property& {
        struct_accessor acc;
        acc.data = &ref;
        acc.register_fields = [](property_set& ps, void* p) {
            property_traits<T>::register_fields(ps, *static_cast<T*>(p));
        };
        acc.reset = [](void* p) { *static_cast<T*>(p) = T{}; };
        acc.snapshot = [](const void* p) -> std::any {
            return *static_cast<const T*>(p);
        };
        acc.restore = [](void* p, const std::any& snap) {
            *static_cast<T*>(p) = std::any_cast<const T&>(snap);
        };

        auto [it, _] = m_props.try_emplace(std::string{name}, properties::property{std::move(acc), false});
        auto& prop = it->second;
        prop.configurability(config);
        prop.set_type_name(struct_type_name_v<T>);

        // Create nested property_set and register fields
        auto nested = std::make_unique<property_set>();
        property_traits<T>::register_fields(*nested, ref);
        prop.set_nested(std::move(nested));

        return prop;
    }

    /**
     * @brief Register a list-of-struct property (requires property_traits<T> specialization)
     */
    template<typename T>
    requires has_property_traits_v<typename std::remove_cvref_t<T>::value_type>
    auto add(std::string_view name,
             T& ref,
             config_type config = config_type::INITIALIZE,
             std::string_view /*units*/ = "") -> property&
    requires is_vector_v<std::remove_cvref_t<T>> {
        using Elem = typename std::remove_cvref_t<T>::value_type;

        struct_accessor acc;
        acc.data = &ref;
        acc.register_fields = [](property_set& ps, void* p) {
            property_traits<Elem>::register_fields(ps, *static_cast<Elem*>(p));
        };
        acc.reset = [](void* p) { static_cast<T*>(p)->clear(); };
        acc.snapshot = [](const void* p) -> std::any {
            return *static_cast<const T*>(p);
        };
        acc.restore = [](void* p, const std::any& snap) {
            *static_cast<T*>(p) = std::any_cast<const T&>(snap);
        };
        acc.size = [](const void* p) { return static_cast<const T*>(p)->size(); };
        acc.get_element = [](void* p, std::size_t i) -> void* {
            return &(*static_cast<T*>(p))[i];
        };
        acc.get_element_const = [](const void* p, std::size_t i) -> const void* {
            return &(*static_cast<const T*>(p))[i];
        };
        acc.emplace_back = [](void* p) -> std::size_t {
            auto* vec = static_cast<T*>(p);
            vec->emplace_back();
            return vec->size() - 1;
        };
        acc.erase = [](void* p, std::size_t i) {
            auto* vec = static_cast<T*>(p);
            vec->erase(vec->begin() + static_cast<std::ptrdiff_t>(i));
        };

        auto [it, _] = m_props.try_emplace(std::string{name}, properties::property{std::move(acc), true});
        auto& prop = it->second;
        prop.configurability(config);
        prop.set_type_name(std::string{"[]"} + std::string{struct_type_name_v<Elem>});

        // Create schema property_set (for introspection)
        auto schema = std::make_unique<property_set>();
        Elem schema_instance{};
        property_traits<Elem>::register_fields(*schema, schema_instance);
        prop.set_nested(std::move(schema));

        return prop;
    }

    // ========================================================================
    // Unified Set Interface
    // ========================================================================

    /**
     * @brief Set a property value from a string
     * @param path Property path (e.g., "name", "struct.field", "list[0]", "list[]")
     * @param value String representation of the value
     * @param config Configurability check (INITIALIZE allows all, RUNTIME only runtime-configurable)
     * @throws key_error if property not found
     * @throws config_error if property not runtime-configurable
     * @throws value_error if value conversion fails
     * @throws listener_rejected if change listener rejects the update
     */
    auto set(std::string_view path_str,
             std::string_view value,
             config_type config = config_type::INITIALIZE) -> void {
        auto path = property_path::parse(path_str);
        set_by_path(path, value, config);
    }

    /**
     * @brief Set multiple properties atomically with rollback on failure
     *
     * If any property update fails, all previous changes in this batch are reverted.
     * This provides transactional semantics for batch property updates.
     *
     * @note List deletions (setting to null) are not supported in atomic batches
     *       because full rollback would require serializing and deserializing
     *       complex types. Use single-property updates for deletions.
     *
     * @throws key_error if property not found (and allow_unknown is false)
     * @throws config_error if property not runtime-configurable
     * @throws value_error if value conversion fails, or if deletion attempted in batch
     * @throws listener_rejected if change listener rejects the update
     */
    auto set_batch(std::span<const std::pair<std::string, std::string>> values,
                   config_type config = config_type::INITIALIZE,
                   bool allow_unknown = false) -> void {
        // Track changes for potential rollback
        struct rollback_entry {
            std::string path;
            std::string old_value;
            bool was_append{false};
            std::size_t append_index{0};
            std::string list_name;  // For appends, the list property name
        };
        std::vector<rollback_entry> rollback_stack;

        auto do_rollback = [&]() {
            // Restore in reverse order
            for (auto it = rollback_stack.rbegin(); it != rollback_stack.rend(); ++it) {
                try {
                    if (it->was_append) {
                        // Remove the appended element
                        erase_no_listener(it->list_name, it->append_index);
                    } else {
                        // Restore previous value (bypass listeners during rollback)
                        auto path = property_path::parse(it->path);
                        set_by_path_no_listener(path, it->old_value);
                    }
                } catch (...) {
                    // Best effort rollback - log but don't throw
                }
            }
        };

        try {
            for (const auto& [path_str, value] : values) {
                auto path = property_path::parse(path_str);
                const auto& head = path.head();

                // Check if this is a deletion (null value for list item)
                // Deletions are only disallowed in multi-item batches (for rollback reasons)
                bool is_null = is_null_value(value);
                if (is_null && head.has_index() && values.size() > 1) {
                    throw value_error(head.name, value,
                        "list deletions not supported in atomic batch - use single update");
                }

                // Check if property exists (for allow_unknown handling)
                auto* prop = find(head.name);
                if (!prop) {
                    if (allow_unknown) { continue; }
                    throw key_error(head.name);
                }

                // Check configurability before any changes
                if (config == config_type::RUNTIME && prop->configurability() == config_type::INITIALIZE) {
                    throw config_error(head.name);
                }

                // Capture current state for rollback
                rollback_entry entry;
                entry.path = path_str;

                if (head.is_append) {
                    // This is an append - track for removal on rollback
                    entry.was_append = true;
                    entry.list_name = std::string{head.name};
                    // We'll set append_index after the operation
                } else {
                    // Save current value
                    entry.old_value = get_value_as_string(path);
                }

                // Apply the change
                if (head.is_append) {
                    // For appends, we need to know the new index
                    std::size_t new_index = 0;
                    if (prop->is_struct_list()) {
                        auto& acc = get_struct_accessor(prop->value());
                        new_index = acc.emplace_back(acc.data);
                        invalidate_struct_list_cache(head.name);
                    } else if (prop->is_list()) {
                        new_index = visit_list(prop->value(), append_list_item_visitor{value, head.name});
                    }
                    entry.append_index = new_index;
                    rollback_stack.push_back(std::move(entry));

                    // Notify listener (may throw) - appends are SET operations
                    if (!prop->notify_change(new_index, change_type::SET)) {
                        throw listener_rejected(head.name);
                    }
                } else {
                    rollback_stack.push_back(std::move(entry));
                    // Use the standard set path
                    set_by_path(path, value, config);
                }
            }
        } catch (...) {
            do_rollback();
            throw;
        }
    }

    // ========================================================================
    // Typed Get Interface
    // ========================================================================

    /**
     * @brief Get a scalar property value
     * @throws key_error if property not found
     * @throws type_error if type mismatch
     */
    template<typename T>
    requires is_scalar_type_v<T>
    [[nodiscard]] auto get(std::string_view name) const -> T {
        auto* prop = find(name);
        if (!prop) { throw key_error(name); }

        if (!prop->is_scalar()) {
            throw type_error(name, type_name_v<T>);
        }

        return *std::get<T*>(std::get<scalar_types>(prop->value()));
    }

    /**
     * @brief Get an optional property value
     */
    template<typename T>
    requires is_scalar_type_v<T>
    [[nodiscard]] auto get_optional(std::string_view name) const -> std::optional<T> {
        auto* prop = find(name);
        if (!prop) { throw key_error(name); }

        if (!prop->is_optional()) {
            throw type_error(name, std::string{type_name_v<T>} + "?");
        }

        return *std::get<std::optional<T>*>(std::get<optional_types>(prop->value()));
    }

    /**
     * @brief Get a property value as a string
     */
    [[nodiscard]] auto get_string(std::string_view name) const -> std::string {
        auto* prop = find(name);
        if (!prop) { throw key_error(name); }

        if (prop->is_scalar()) {
            return visit_scalar(prop->value(), get_string_visitor{});
        } else if (prop->is_optional()) {
            auto result = visit_optional(prop->value(), get_optional_string_visitor{});
            return result.value_or("");
        }

        throw type_error(name, "scalar or optional");
    }

    // ========================================================================
    // List Operations
    // ========================================================================

    [[nodiscard]] auto list_size(std::string_view name) const -> std::size_t {
        auto* prop = find(name);
        if (!prop || !prop->is_list()) { throw key_error(name); }

        if (prop->is_struct_list()) {
            auto& acc = get_struct_accessor(prop->value());
            return acc.size(acc.data);
        }

        return visit_list(prop->value(), list_size_visitor{});
    }

    auto append(std::string_view name, std::string_view value) -> std::size_t {
        auto* prop = find(name);
        if (!prop || !prop->is_list()) { throw key_error(name); }

        if (prop->is_struct_list()) {
            auto& acc = get_struct_accessor(prop->value());
            auto index = acc.emplace_back(acc.data);
            if (!prop->notify_change(index, change_type::SET)) {
                acc.erase(acc.data, index);
                throw listener_rejected(name);
            }
            invalidate_struct_list_cache(name);
            return index;
        }

        auto index = visit_list(prop->value(), append_list_item_visitor{value, name});
        if (!prop->notify_change(index, change_type::SET)) {
            visit_list(prop->value(), erase_list_item_visitor{index, name});
            throw listener_rejected(name);
        }
        return index;
    }

    auto append_struct_list(std::string_view name,
                            std::span<const std::pair<std::string, std::string>> fields,
                            config_type config = config_type::INITIALIZE) -> std::size_t {
        auto* prop = find(name);
        if (!prop || !prop->is_struct_list()) { throw key_error(name); }

        if (config == config_type::RUNTIME && prop->configurability() == config_type::INITIALIZE) {
            throw config_error(name);
        }

        auto& acc = get_struct_accessor(prop->value());
        auto index = acc.emplace_back(acc.data);
        invalidate_struct_list_cache(name);

        try {
            auto* element_props = get_struct_list_element_set(*prop, name, index);
            for (const auto& [field_path, field_value] : fields) {
                auto path = property_path::parse(field_path);
                element_props->set_by_path(path, field_value, config);
            }
        } catch (...) {
            acc.erase(acc.data, index);
            invalidate_struct_list_cache(name);
            throw;
        }

        if (!prop->notify_change(index, change_type::SET)) {
            acc.erase(acc.data, index);
            invalidate_struct_list_cache(name);
            throw listener_rejected(name);
        }

        return index;
    }

    auto update_struct_list_element(std::string_view name,
                                    std::size_t index,
                                    std::span<const std::pair<std::string, std::string>> fields,
                                    config_type config = config_type::INITIALIZE) -> void {
        auto* prop = find(name);
        if (!prop || !prop->is_struct_list()) { throw key_error(name); }

        if (config == config_type::RUNTIME && prop->configurability() == config_type::INITIALIZE) {
            throw config_error(name);
        }

        auto& acc = get_struct_accessor(prop->value());
        if (index >= acc.size(acc.data)) {
            throw index_error(name, index, acc.size(acc.data));
        }

        auto snapshot = acc.snapshot(acc.data);
        try {
            auto* element_props = get_struct_list_element_set(*prop, name, index);
            for (const auto& [field_path, field_value] : fields) {
                auto path = property_path::parse(field_path);
                element_props->set_by_path(path, field_value, config);
            }
        } catch (...) {
            acc.restore(acc.data, snapshot);
            throw;
        }

        if (!prop->notify_change(index, change_type::MODIFY)) {
            acc.restore(acc.data, snapshot);
            throw listener_rejected(name);
        }
    }

    auto erase(std::string_view name, std::size_t index) -> void {
        auto* prop = find(name);
        if (!prop || !prop->is_list()) { throw key_error(name); }

        if (prop->is_struct_list()) {
            auto& acc = get_struct_accessor(prop->value());
            if (index >= acc.size(acc.data)) {
                throw index_error(name, index, acc.size(acc.data));
            }
            auto snapshot = acc.snapshot(acc.data);
            acc.erase(acc.data, index);
            if (!prop->notify_change(index, change_type::RESET)) {
                acc.restore(acc.data, snapshot);
                throw listener_rejected(name);
            }
            invalidate_struct_list_cache(name);
            return;
        }

        auto old_value = visit_list(prop->value(), get_list_item_visitor{index, name});
        visit_list(prop->value(), erase_list_item_visitor{index, name});
        if (!prop->notify_change(index, change_type::RESET)) {
            visit_list(prop->value(), insert_list_item_visitor{index, old_value, name});
            throw listener_rejected(name);
        }
    }

    // ========================================================================
    // Property Access
    // ========================================================================

    [[nodiscard]]
    auto properties() const noexcept -> const property_map& {
        return m_props;
    }

    [[nodiscard]]
    auto find(std::string_view name) -> properties::property* {
        auto it = m_props.find(name);
        return it != m_props.end() ? &it->second : nullptr;
    }

    [[nodiscard]]
    auto find(std::string_view name) const -> const properties::property* {
        auto it = m_props.find(name);
        return it != m_props.end() ? &it->second : nullptr;
    }

    // ========================================================================
    // Change Listener Registration
    // ========================================================================

    auto add_change_listener(std::string_view name, properties::property::change_listener_fn fn) -> void {
        if (auto* prop = find(name)) {
            prop->change_listener(std::move(fn));
        }
    }

    auto add_change_listener(std::string_view name, properties::property::indexed_change_listener_fn fn) -> void {
        if (auto* prop = find(name)) {
            prop->change_listener(std::move(fn));
        }
    }

    /// Add a contextual change listener that receives change type information
    auto add_change_listener(std::string_view name, properties::property::contextual_change_listener_fn fn) -> void {
        if (auto* prop = find(name)) {
            prop->change_listener(std::move(fn));
        }
    }

    /// Add a contextual indexed change listener that receives index and change type information
    auto add_change_listener(std::string_view name, properties::property::contextual_indexed_change_listener_fn fn) -> void {
        if (auto* prop = find(name)) {
            prop->change_listener(std::move(fn));
        }
    }

private:
    property_map m_props;
    mutable std::map<std::pair<std::string, std::size_t>, std::unique_ptr<property_set>> m_struct_list_cache;

    auto invalidate_struct_list_cache(std::string_view name) -> void {
        for (auto it = m_struct_list_cache.begin(); it != m_struct_list_cache.end(); ) {
            if (it->first.first == name) {
                it = m_struct_list_cache.erase(it);
            } else {
                ++it;
            }
        }
    }

    auto get_struct_list_element_set(const properties::property& prop,
                                     std::string_view name,
                                     std::size_t index) const -> property_set* {
        auto key = std::make_pair(std::string{name}, index);
        auto it = m_struct_list_cache.find(key);
        if (it != m_struct_list_cache.end()) {
            return it->second.get();
        }

        auto element_props = std::make_unique<property_set>();
        auto& acc = get_struct_accessor(prop.value());
        void* element_ptr = acc.get_element(const_cast<void*>(acc.data), index);
        acc.register_fields(*element_props, element_ptr);

        auto* ptr = element_props.get();
        m_struct_list_cache.emplace(std::move(key), std::move(element_props));
        return ptr;
    }

    // ========================================================================
    // Path-Based Set Implementation
    // ========================================================================

    auto set_by_path(const property_path& path, std::string_view value, config_type config) -> void {
        const auto& head = path.head();
        auto* prop = find(head.name);
        if (!prop) { throw key_error(head.name); }

        // Check configurability
        if (config == config_type::RUNTIME && prop->configurability() == config_type::INITIALIZE) {
            throw config_error(head.name);
        }

        bool is_null = is_null_value(value);

        // Handle nested path (struct or struct-list field access)
        if (path.has_tail()) {
            set_nested_path(*prop, head, path.tail(), value, config);
            return;
        }

        // Handle indexed access or append
        if (head.has_index() || head.is_append) {
            set_list_element(*prop, head, value, is_null);
            return;
        }

        // Handle simple property set
        set_simple_property(*prop, head.name, value, is_null);
    }

    auto set_simple_property(properties::property& prop,
                             std::string_view name,
                             std::string_view value,
                             bool is_null) -> void {
        if (prop.is_scalar()) {
            // Save old value for rollback if listener rejects
            auto old_value = visit_scalar(prop.value(), get_string_visitor{});

            // Determine change type: RESET if setting to null/default, otherwise MODIFY
            auto ctx = is_null ? change_type::RESET : change_type::MODIFY;

            if (is_null) {
                visit_scalar(prop.value(), reset_scalar_visitor{});
            } else {
                visit_scalar(prop.value(), set_scalar_visitor{value, name});
            }

            // If listener rejects, restore old value before throwing
            if (!prop.notify_change(ctx)) {
                visit_scalar(prop.value(), set_scalar_visitor{old_value, name});
                throw listener_rejected(name);
            }
        } else if (prop.is_optional()) {
            // Save old value for rollback if listener rejects
            auto old_value = visit_optional(prop.value(), get_optional_string_visitor{});

            // Determine change type for optionals:
            // - SET: was nullopt, now has value
            // - MODIFY: had value, now different value
            // - RESET: had value, now nullopt
            change_type ctx;
            if (is_null) {
                ctx = change_type::RESET;
            } else if (!old_value.has_value()) {
                ctx = change_type::SET;
            } else {
                ctx = change_type::MODIFY;
            }

            visit_optional(prop.value(), set_optional_visitor{value, name, is_null});

            // If listener rejects, restore old value before throwing
            if (!prop.notify_change(ctx)) {
                if (old_value.has_value()) {
                    visit_optional(prop.value(), set_optional_visitor{*old_value, name, false});
                } else {
                    visit_optional(prop.value(), set_optional_visitor{"", name, true});
                }
                throw listener_rejected(name);
            }
        } else if (prop.is_struct()) {
            if (is_null) {
                auto& acc = get_struct_accessor(prop.value());
                auto snapshot = acc.snapshot(acc.data);
                acc.reset(acc.data);
                if (!prop.notify_change(change_type::RESET)) {
                    acc.restore(acc.data, snapshot);
                    throw listener_rejected(name);
                }
            } else {
                throw value_error(name, value, "structs can only be reset with null");
            }
        } else if (prop.is_struct_list()) {
            if (is_null) {
                auto& acc = get_struct_accessor(prop.value());
                auto snapshot = acc.snapshot(acc.data);
                acc.reset(acc.data);  // Clears the vector
                if (!prop.notify_change(change_type::RESET)) {
                    acc.restore(acc.data, snapshot);
                    throw listener_rejected(name);
                }
                invalidate_struct_list_cache(name);
            } else {
                throw value_error(name, value, "struct lists can only be reset with null");
            }
        } else {
            throw type_error(name, "unknown property type");
        }
    }

    auto set_list_element(properties::property& prop,
                          const path_segment& seg,
                          std::string_view value,
                          bool is_null) -> void {
        if (!prop.is_list()) { throw type_error(seg.name, "list"); }

        if (prop.is_struct_list()) {
            set_struct_list_element(prop, seg, value, is_null);
            return;
        }

        // Scalar list
        if (seg.is_append) {
            auto index = visit_list(prop.value(), append_list_item_visitor{value, seg.name});
            if (!prop.notify_change(index, change_type::SET)) {
                visit_list(prop.value(), erase_list_item_visitor{index, seg.name});
                throw listener_rejected(seg.name);
            }
        } else if (seg.index.has_value()) {
            if (is_null) {
                auto old_value = visit_list(prop.value(), get_list_item_visitor{*seg.index, seg.name});
                visit_list(prop.value(), erase_list_item_visitor{*seg.index, seg.name});
                if (!prop.notify_change(*seg.index, change_type::RESET)) {
                    visit_list(prop.value(), insert_list_item_visitor{*seg.index, old_value, seg.name});
                    throw listener_rejected(seg.name);
                }
            } else {
                // Save old value for rollback if listener rejects
                auto old_value = visit_list(prop.value(), get_list_item_visitor{*seg.index, seg.name});

                visit_list(prop.value(), set_list_item_visitor{*seg.index, value, seg.name});

                // If listener rejects, restore old value before throwing
                if (!prop.notify_change(*seg.index, change_type::MODIFY)) {
                    visit_list(prop.value(), set_list_item_visitor{*seg.index, old_value, seg.name});
                    throw listener_rejected(seg.name);
                }
            }
        }
    }

    auto set_struct_list_element(properties::property& prop,
                                 const path_segment& seg,
                                 std::string_view /*value*/,
                                 bool is_null) -> void {
        auto& acc = get_struct_accessor(prop.value());

        if (seg.is_append) {
            auto index = acc.emplace_back(acc.data);
            if (!prop.notify_change(index, change_type::SET)) {
                acc.erase(acc.data, index);
                throw listener_rejected(seg.name);
            }
            invalidate_struct_list_cache(seg.name);
        } else if (seg.index.has_value()) {
            auto index = *seg.index;
            if (index >= acc.size(acc.data)) {
                throw index_error(seg.name, index, acc.size(acc.data));
            }
            if (is_null) {
                auto snapshot = acc.snapshot(acc.data);
                acc.erase(acc.data, index);
                if (!prop.notify_change(index, change_type::RESET)) {
                    acc.restore(acc.data, snapshot);
                    throw listener_rejected(seg.name);
                }
                invalidate_struct_list_cache(seg.name);
            }
            // Note: Setting a struct-list element to a non-null value requires field-by-field update
        }
    }

    auto set_nested_path(properties::property& prop,
                         const path_segment& head,
                         const property_path& tail,
                         std::string_view value,
                         config_type config) -> void {
        auto old_value = get_nested_value_as_string(prop, head, tail);

        if (prop.is_struct()) {
            // Navigate into the nested property_set
            auto* nested = prop.nested();
            if (!nested) { throw type_error(head.name, "struct with nested properties"); }
            nested->set_by_path(tail, value, config);
            // Nested field changes are always MODIFY on the parent struct
            if (!prop.notify_change(change_type::MODIFY)) {
                set_nested_path_no_listener(prop, head, tail, old_value);
                throw listener_rejected(head.name);
            }
        } else if (prop.is_struct_list()) {
            if (!head.index.has_value()) {
                throw value_error(head.name, "", "index required for struct list field access");
            }

            auto& acc = get_struct_accessor(prop.value());
            auto index = *head.index;
            if (index >= acc.size(acc.data)) {
                throw index_error(head.name, index, acc.size(acc.data));
            }

            auto* element_props = get_struct_list_element_set(prop, head.name, index);
            element_props->set_by_path(tail, value, config);
            // Nested field changes are always MODIFY on the list element
            if (!prop.notify_change(index, change_type::MODIFY)) {
                element_props->set_by_path_no_listener(tail, old_value);
                throw listener_rejected(head.name);
            }
        } else {
            throw type_error(head.name, "struct or struct list");
        }
    }

    // ========================================================================
    // Helper Methods for Atomic Batch Updates
    // ========================================================================

    /**
     * @brief Get the current value of a property path as a string (for rollback)
     */
    [[nodiscard]]
    auto get_value_as_string(const property_path& path) const -> std::string {
        const auto& head = path.head();
        auto* prop = find(head.name);
        if (!prop) { throw key_error(head.name); }

        // Handle nested path (struct or struct-list field access)
        if (path.has_tail()) {
            return get_nested_value_as_string(*prop, head, path.tail());
        }

        // Handle indexed access
        if (head.has_index()) {
            if (prop->is_struct_list()) {
                // For struct lists, we can't easily serialize a single item to string
                // Return empty string - we'll handle struct list items specially
                return "";
            }
            if (prop->is_list()) {
                return visit_list(prop->value(), get_list_item_visitor{*head.index, head.name});
            }
            throw type_error(head.name, "list");
        }

        // Handle simple property
        if (prop->is_scalar()) {
            return visit_scalar(prop->value(), get_string_visitor{});
        }
        if (prop->is_optional()) {
            auto result = visit_optional(prop->value(), get_optional_string_visitor{});
            return result.value_or(std::string{null_value});
        }

        // Structs and struct lists can't be serialized to a simple string
        return "";
    }

    [[nodiscard]]
    auto get_nested_value_as_string(const properties::property& prop,
                                    const path_segment& head,
                                    const property_path& tail) const -> std::string {
        if (prop.is_struct()) {
            auto* nested = prop.nested();
            if (!nested) { throw type_error(head.name, "struct with nested properties"); }
            return nested->get_value_as_string(tail);
        }

        if (prop.is_struct_list()) {
            if (!head.index.has_value()) {
                throw value_error(head.name, "", "index required for struct list field access");
            }

            auto& acc = get_struct_accessor(prop.value());
            auto index = *head.index;
            if (index >= acc.size(acc.data)) {
                throw index_error(head.name, index, acc.size(acc.data));
            }

            auto* element_props = get_struct_list_element_set(prop, head.name, index);
            return element_props->get_value_as_string(tail);
        }

        throw type_error(head.name, "struct or struct list");
    }

    /**
     * @brief Set a property value without triggering change listeners (for rollback)
     */
    auto set_by_path_no_listener(const property_path& path, std::string_view value) -> void {
        const auto& head = path.head();
        auto* prop = find(head.name);
        if (!prop) { throw key_error(head.name); }

        bool is_null = is_null_value(value);

        // Handle nested path (struct or struct-list field access)
        if (path.has_tail()) {
            set_nested_path_no_listener(*prop, head, path.tail(), value);
            return;
        }

        // Handle indexed access
        if (head.has_index()) {
            if (prop->is_struct_list()) {
                // Struct list element - already handled by rollback tracking
                return;
            }
            if (prop->is_list()) {
                if (is_null) {
                    visit_list(prop->value(), erase_list_item_visitor{*head.index, head.name});
                } else {
                    visit_list(prop->value(), set_list_item_visitor{*head.index, value, head.name});
                }
                return;
            }
            throw type_error(head.name, "list");
        }

        // Handle simple property set (no listener notification)
        if (prop->is_scalar()) {
            if (is_null) {
                visit_scalar(prop->value(), reset_scalar_visitor{});
            } else {
                visit_scalar(prop->value(), set_scalar_visitor{value, head.name});
            }
        } else if (prop->is_optional()) {
            visit_optional(prop->value(), set_optional_visitor{value, head.name, is_null});
        } else if (prop->is_struct()) {
            if (is_null) {
                auto& acc = get_struct_accessor(prop->value());
                acc.reset(acc.data);
            }
        } else if (prop->is_struct_list()) {
            if (is_null) {
                auto& acc = get_struct_accessor(prop->value());
                acc.reset(acc.data);
                invalidate_struct_list_cache(head.name);
            }
        }
    }

    auto erase_no_listener(std::string_view name, std::size_t index) -> void {
        auto* prop = find(name);
        if (!prop || !prop->is_list()) { throw key_error(name); }

        if (prop->is_struct_list()) {
            auto& acc = get_struct_accessor(prop->value());
            if (index >= acc.size(acc.data)) {
                throw index_error(name, index, acc.size(acc.data));
            }
            acc.erase(acc.data, index);
            invalidate_struct_list_cache(name);
            return;
        }

        visit_list(prop->value(), erase_list_item_visitor{index, name});
    }

    auto set_nested_path_no_listener(properties::property& prop,
                                     const path_segment& head,
                                     const property_path& tail,
                                     std::string_view value) -> void {
        if (prop.is_struct()) {
            auto* nested = prop.nested();
            if (!nested) { throw type_error(head.name, "struct with nested properties"); }
            nested->set_by_path_no_listener(tail, value);
        } else if (prop.is_struct_list()) {
            if (!head.index.has_value()) {
                throw value_error(head.name, "", "index required for struct list field access");
            }

            auto& acc = get_struct_accessor(prop.value());
            auto index = *head.index;
            if (index >= acc.size(acc.data)) {
                throw index_error(head.name, index, acc.size(acc.data));
            }

            auto* element_props = get_struct_list_element_set(prop, head.name, index);
            element_props->set_by_path_no_listener(tail, value);
        } else {
            throw type_error(head.name, "struct or struct list");
        }
    }
};

} // namespace composite::properties
