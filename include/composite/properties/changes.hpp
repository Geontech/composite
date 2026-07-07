/*
 * Copyright (C) 2024-2026 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "reflect.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

/**
 * @file changes.hpp
 * @brief changes<T> — the scoped, compile-checked diff handed to config<T>::on_apply.
 *
 * A config<T> reaction receives `(const T& prev, const changes<T>& ch)`. The diff is
 * already scoped to the top-level fields that actually changed (the RFC-7396 struct
 * diff), so `ch.changed(&T::gain)` is true ONLY when gain changed — letting a
 * single-field write react narrowly instead of rebuilding everything. The member
 * pointer is compile-checked (`&Other::x` / a wrong type won't compile); a valid
 * member that was never listed in COMPOSITE_FIELDS throws at runtime (a runtime
 * member-pointer can't be static_assert'd).
 */
namespace composite {

template <reflect::reflected T>
class changes {
public:
    /// @param diff the scoped RFC-7396 struct diff; @param live the post-commit value
    /// (config_binding builds this AFTER the swap, so reading a field from it yields the
    /// exact new value — including nested reflected structs the diff only partially carries).
    changes(const reflect::json& diff, const T& live) : m_diff(&diff), m_live(&live) {}

    /// Did anything change at all?
    [[nodiscard]] auto any() const -> bool { return m_diff->is_object() && !m_diff->empty(); }

    /// Did this field change? `&T::field` is compile-checked.
    template <typename M>
    [[nodiscard]] auto changed(M T::* member) const -> bool {
        return m_diff->is_object() && m_diff->contains(std::string(name_of(member)));
    }

    /// The new value of a changed field (read from the committed struct, so it is exact
    /// for every field type incl. nested structs), or nullopt if it did not change.
    template <typename M>
    [[nodiscard]] auto new_value(M T::* member) const -> std::optional<M> {
        if (!changed(member)) { return std::nullopt; }
        return m_live->*member;
    }

    /// Names of the top-level fields that changed.
    [[nodiscard]] auto changed_fields() const -> std::vector<std::string> {
        std::vector<std::string> out;
        if (m_diff->is_object()) {
            for (auto it = m_diff->begin(); it != m_diff->end(); ++it) { out.push_back(it.key()); }
        }
        return out;
    }

    /// The raw RFC-7396 diff (escape hatch / advanced use).
    [[nodiscard]] auto raw() const -> const reflect::json& { return *m_diff; }

private:
    /// Map a pointer-to-member to its reflected field name. Disambiguates same-typed
    /// fields by the pointer value. Throws if the member is not a COMPOSITE_FIELDS field.
    template <typename M>
    static auto name_of(M T::* member) -> std::string_view {
        std::string_view found{};
        bool matched = false;
        std::apply([&](auto&&... f) {
            ([&] {
                using F = std::decay_t<decltype(f)>;
                if constexpr (std::is_same_v<typename F::member_type, M>) {
                    if (f.ptr == member) { found = f.name; matched = true; }
                }
            }(), ...);
        }, reflect::descriptor<T>::fields());
        if (!matched) {
            throw std::logic_error("changes<T>: pointer-to-member is not a reflected field of T");
        }
        return found;
    }

    const reflect::json* m_diff;
    const T* m_live;  ///< the committed (post-change) value; source of new_value()
};

} // namespace composite
