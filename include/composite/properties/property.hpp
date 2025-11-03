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
#include "property_operations.hpp"

#include <any>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

namespace composite {

// Forward declaration
class property_set;

class property {
public:
    using change_func_type = std::function<bool()>;
    using indexed_change_func_type = std::function<bool(std::size_t)>;
    using any_change_listener = std::variant<std::monostate, change_func_type, indexed_change_func_type>;

    property(std::string_view type, std::any value) :
      m_type(type),
      m_value(value) {}

    // Type information
    auto type() const noexcept -> std::string { return m_type; }
    auto type(std::string_view value) -> void { m_type = value; }
    auto is_optional() const noexcept -> bool { return m_type.ends_with('?'); }
    auto is_list() const noexcept -> bool { return m_type.starts_with("[]"); }
    auto is_structured() const noexcept -> bool { return m_struct != nullptr; }

    // Value access
    auto value() const -> const std::any& { return m_value; }
    auto value() -> std::any& { return m_value; }
    auto value(std::any value) -> void { m_value = value; }

    // Metadata
    auto units() const noexcept -> std::string { return m_units; }
    auto units(std::string_view u) -> property& {
        m_units = std::string{u};
        return *this;
    }

    auto configurability() const noexcept -> properties::config_type { return m_configurability; }
    auto configurability(properties::config_type value) -> property& {
        m_configurability = value;
        return *this;
    }

    // Change listeners
    auto change_listener() const noexcept -> any_change_listener { return m_change_func; }

    auto change_listener(change_func_type value) -> void {
        if (this->is_list()) {
            throw std::runtime_error("list properties must use indexed change listener");
        }
        m_change_func = value;
    }

    auto change_listener(indexed_change_func_type value) -> void {
        if (!this->is_list()) {
            throw std::runtime_error("non-list properties must use non-indexed change listener");
        }
        m_change_func = value;
    }

    // Structured property access
    auto structured() -> property_set& { return *m_struct; }
    auto structured() const -> const property_set& { return *m_struct; }
    auto structured(std::shared_ptr<property_set> s) -> void { m_struct = std::move(s); }

    // Struct operations (delegated to helper)
    auto struct_registration(detail::struct_operations::registration_func func) -> void {
        m_struct_ops.set_registration_func(std::move(func));
    }
    auto struct_registration(property_set& set, void* ptr) const -> void {
        m_struct_ops.register_fields(set, ptr);
    }

    auto struct_reset(detail::struct_operations::reset_func func) -> void {
        m_struct_ops.set_reset_func(std::move(func));
    }
    auto struct_reset() -> void {
        m_struct_ops.reset(m_value);
    }

    auto struct_emplace_back(detail::struct_operations::emplace_back_func func) -> void {
        m_struct_ops.set_emplace_back_func(std::move(func));
    }
    auto struct_emplace_back() -> std::optional<std::size_t> {
        return m_struct_ops.emplace_back(m_value);
    }

    auto struct_erase(detail::struct_operations::erase_func func) -> void {
        m_struct_ops.set_erase_func(std::move(func));
    }
    auto struct_erase(std::size_t index) -> void {
        m_struct_ops.erase(m_value, index);
    }

    auto struct_getter(detail::struct_operations::getter_func func) -> void {
        m_struct_ops.set_getter_func(std::move(func));
    }
    auto struct_getter(std::size_t index) const -> void* {
        return m_struct_ops.get_element(m_value, index);
    }

    auto struct_list_size(detail::struct_operations::list_size_func func) -> void {
        m_struct_ops.set_list_size_func(std::move(func));
    }
    auto struct_list_size() const -> std::size_t {
        return m_struct_ops.list_size(m_value);
    }

private:
    std::string m_type;
    std::any m_value;
    std::string m_units;
    properties::config_type m_configurability{};
    any_change_listener m_change_func;
    std::shared_ptr<property_set> m_struct;
    mutable detail::struct_operations m_struct_ops;

}; // class property

} // namespace composite
