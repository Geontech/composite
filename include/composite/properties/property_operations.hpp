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

#include <any>
#include <functional>
#include <optional>

namespace composite {

// Forward declaration
class property_set;

namespace detail {

class struct_operations {
public:
    using reset_func = std::function<void(std::any&)>;
    using emplace_back_func = std::function<std::size_t(std::any&)>;
    using erase_func = std::function<void(std::any&, std::size_t)>;
    using registration_func = std::function<void(property_set&, void*)>;
    using getter_func = std::function<void*(const std::any&, std::size_t)>;
    using list_size_func = std::function<std::size_t(const std::any&)>;

    auto reset(std::any& value) -> void {
        if (m_reset_func) { m_reset_func(value); }
    }

    auto emplace_back(std::any& value) -> std::optional<std::size_t> {
        return m_emplace_back_func ? std::optional{m_emplace_back_func(value)} : std::nullopt;
    }

    auto erase(std::any& value, std::size_t index) -> void {
        if (m_erase_func) { m_erase_func(value, index); }
    }

    auto get_element(const std::any& value, std::size_t index) const -> void* {
        return m_getter_func ? m_getter_func(value, index) : nullptr;
    }

    auto list_size(const std::any& value) const -> std::size_t {
        return m_list_size_func ? m_list_size_func(value) : 0;
    }

    auto register_fields(property_set& ps, void* ptr) const -> void {
        if (m_registration_func) { m_registration_func(ps, ptr); }
    }

    auto set_reset_func(reset_func func) -> void { m_reset_func = std::move(func); }
    auto set_emplace_back_func(emplace_back_func func) -> void { m_emplace_back_func = std::move(func); }
    auto set_erase_func(erase_func func) -> void { m_erase_func = std::move(func); }
    auto set_registration_func(registration_func func) -> void { m_registration_func = std::move(func); }
    auto set_getter_func(getter_func func) -> void { m_getter_func = std::move(func); }
    auto set_list_size_func(list_size_func func) -> void { m_list_size_func = std::move(func); }

private:
    reset_func m_reset_func;
    emplace_back_func m_emplace_back_func;
    erase_func m_erase_func;
    registration_func m_registration_func;
    getter_func m_getter_func;
    list_size_func m_list_size_func;
};

} // namespace detail
} // namespace composite
