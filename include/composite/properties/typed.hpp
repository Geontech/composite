/*
 * Copyright (C) 2024-2026 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "reflect.hpp"
#include "types.hpp" // config_type

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

/**
 * @file typed.hpp
 * @brief Typed properties on a candidate -> validate -> swap -> diff -> notify
 *        commit path. There is NO rollback: validation runs on a candidate
 *        before the live value is touched, so a rejected change never mutates
 *        state. A two-phase prepare()/commit() lets a property_set apply several
 *        properties atomically (validate all, then commit all). JSON is the
 *        transport; the worker reads the live member directly (mutated only
 *        while the worker is parked).
 */
namespace composite::properties {

using reflect::json;

/// A validator/listener rejected or the value was illegal. The two-argument form
/// carries a human-readable reason (from validate(fn, "reason")) so the rejection
/// says more than just the property name (the ROS 2 SetParametersResult lesson).
class validation_error : public std::runtime_error {
public:
    std::string name;
    std::string reason; ///< optional explanation; empty if none was supplied
    explicit validation_error(std::string n)
        : std::runtime_error("property change rejected: " + n), name(std::move(n)) {}
    validation_error(std::string n, std::string why)
        : std::runtime_error("property change rejected: " + n + " (" + why + ")"), name(std::move(n)),
          reason(std::move(why)) {}
};

/// A RUNTIME apply touched an INITIALIZE-only property.
class config_violation : public std::runtime_error {
public:
    std::string name;
    explicit config_violation(std::string n)
        : std::runtime_error("property not runtime-configurable: " + n), name(std::move(n)) {}
};

/// A referenced property name is not registered.
class unknown_property : public std::runtime_error {
public:
    std::string name;
    explicit unknown_property(std::string n) : std::runtime_error("unknown property: " + n), name(std::move(n)) {}
};

/**
 * @brief Type-erased property interface held by a property_set.
 *
 * Two-phase commit: prepare() validates a candidate (throwing on reject without
 * mutating); commit() swaps it in and returns the change diff (null if no-op);
 * abort() discards a prepared candidate. A property_set prepares every property
 * in a batch before committing any, so a batch is all-or-nothing.
 */
class property_base {
public:
    virtual ~property_base() = default;
    [[nodiscard]] virtual auto name() const -> const std::string& = 0;
    [[nodiscard]] virtual auto configurability() const -> config_type = 0;
    [[nodiscard]] virtual auto type_name() const -> std::string = 0;
    [[nodiscard]] virtual auto encode() const -> json = 0;
    /// UI-grade schema for this property: name, type (incl. nested fields / array
    /// items / enum choices / integer range), configurability, default, and unit.
    [[nodiscard]] virtual auto describe() const -> json = 0;

    virtual auto prepare(const json& value, config_type ctx) -> void = 0;
    /// Swap in the prepared candidate and return the diff (null if no change).
    /// Mutation only — does NOT fire change listeners (see notify()), so a
    /// property_set can commit a whole batch before any listener observes it.
    virtual auto commit() -> json = 0;
    /// Fire change listeners for a committed diff (no-op if diff is null).
    virtual auto notify(const json& diff) -> void = 0;
    virtual auto abort() noexcept -> void = 0;

    /// Single-shot convenience: prepare + commit + notify. Returns true if changed.
    auto apply(const json& value, config_type ctx = config_type::INITIALIZE) -> bool {
        prepare(value, ctx);
        const json d = commit();
        notify(d);
        return !d.is_null();
    }
};

// ---------------------------------------------------------------------------
// typed_property<T> — scalar, enum, optional, vector, or reflected struct,
// bound to a component member (the member stays the source of truth).
// ---------------------------------------------------------------------------
template <typename T>
class typed_property final : public property_base {
public:
    using validator_fn = std::function<bool(const T& candidate)>;
    using listener_fn = std::function<void(const json& diff)>;

    typed_property(std::string name, T* ref, config_type cfg = config_type::INITIALIZE)
        : m_name(std::move(name)), m_ref(ref), m_config(cfg), m_default(ref != nullptr ? *ref : T{}) {}

    /// Attach a validator. The two-argument form supplies a human-readable reason
    /// surfaced in the rejection (REST 400 / log) instead of just the property name.
    auto validate(validator_fn fn) -> typed_property& {
        m_validators.push_back({std::move(fn), {}});
        return *this;
    }
    auto validate(validator_fn fn, std::string reason) -> typed_property& {
        m_validators.push_back({std::move(fn), std::move(reason)});
        return *this;
    }
    auto on_change(listener_fn fn) -> typed_property& {
        m_listeners.push_back(std::move(fn));
        return *this;
    }
    auto units(std::string u) -> typed_property& {
        m_units = std::move(u);
        return *this;
    }

    [[nodiscard]] auto name() const -> const std::string& override { return m_name; }
    [[nodiscard]] auto configurability() const -> config_type override { return m_config; }
    [[nodiscard]] auto unit() const -> const std::string& { return m_units; }
    [[nodiscard]] auto type_name() const -> std::string override { return type_name_for<T>(); }
    [[nodiscard]] auto encode() const -> json override { return reflect::encode(*m_ref); }
    [[nodiscard]] auto get() const -> const T& { return *m_ref; }

    [[nodiscard]] auto describe() const -> json override {
        json o = reflect::type_schema<T>(); // type + nested fields / items / choices / int range
        o["name"] = m_name;
        o["configurability"] = (m_config == config_type::RUNTIME) ? "runtime" : "initialize";
        o["default"] = reflect::encode(m_default);
        if (!m_units.empty()) {
            o["unit"] = m_units;
        }
        return o;
    }

    auto prepare(const json& value, config_type ctx) -> void override {
        if (ctx == config_type::RUNTIME && m_config == config_type::INITIALIZE) {
            throw config_violation(m_name);
        }
        T candidate = *m_ref;
        if (value.is_null()) {
            candidate = m_default; // RFC-7396 null: reset to the value captured
                                   // at registration (re-runs member initializers;
                                   // T{} would lose them — the noop_thread_delay bug)
        } else if constexpr (reflect::is_reflected_v<T>) {
            // pass m_default so a nested-field null resets to the registered default
            // at that depth (not the zero value).
            reflect::merge(candidate, value, m_name, &m_default);
        } else {
            reflect::decode(value, candidate, m_name); // scalar/enum/optional/vector: strict replace
        }
        for (auto& v : m_validators) {
            if (!v.fn(candidate)) {
                if (v.reason.empty()) {
                    throw validation_error(m_name);
                }
                throw validation_error(m_name, v.reason);
            }
        }
        m_pending.emplace(std::move(candidate));
    }

    auto commit() -> json override {
        if (!m_pending.has_value()) {
            return json();
        }
        if (reflect::equal(*m_ref, *m_pending)) {
            m_pending.reset();
            return json();
        }
        const json d = make_diff(*m_ref, *m_pending);
        *m_ref = std::move(*m_pending);
        m_pending.reset();
        return d;
    }

    auto notify(const json& diff) -> void override {
        if (diff.is_null()) {
            return;
        }
        for (auto& l : m_listeners) {
            l(diff);
        }
    }

    auto abort() noexcept -> void override { m_pending.reset(); }

private:
    static auto make_diff(const T& before, const T& after) -> json {
        if constexpr (reflect::is_reflected_v<T>) {
            return reflect::diff(before, after);
        } else {
            return reflect::encode(after);
        }
    }
    template <typename U>
    static auto type_name_for() -> std::string {
        if constexpr (reflect::is_reflected_v<U>) {
            return "struct";
        } else if constexpr (std::is_enum_v<U>) {
            return "enum";
        } else if constexpr (reflect::is_optional<U>::value) {
            return "optional";
        } else if constexpr (reflect::is_vector<U>::value) {
            return "array";
        } else if constexpr (std::is_same_v<U, bool>) {
            return "bool";
        } else if constexpr (std::is_floating_point_v<U>) {
            return "number";
        } else if constexpr (std::is_integral_v<U>) {
            return "integer";
        } else if constexpr (std::is_same_v<U, std::string>) {
            return "string";
        } else {
            return "unknown";
        }
    }

    struct val_entry {
        validator_fn fn;
        std::string reason;
    };

    std::string m_name;
    T* m_ref;
    config_type m_config;
    std::string m_units;
    T m_default; ///< value captured at registration, used for null-reset and describe()
    std::vector<val_entry> m_validators;
    std::vector<listener_fn> m_listeners;
    std::optional<T> m_pending; ///< prepared candidate, awaiting commit
};

// ---------------------------------------------------------------------------
// keyed_collection<E> — runtime-editable, stably-keyed set of reflected structs
// (a channelizer's per-channel configs), backed by std::map. The map key is the
// element's stable identity. Per-element and whole-list (cross-element)
// validators run on a candidate before commit. apply()/prepare() are 7396 over
// the map: key -> null erases; key -> object upserts (merging into the element).
// ---------------------------------------------------------------------------
template <typename E>
class keyed_collection final : public property_base {
public:
    using map_type = std::map<std::string, E>;
    using elem_validator = std::function<bool(const std::string& key, const E& candidate)>;
    using list_validator = std::function<bool(const map_type& candidate)>;
    using listener_fn = std::function<void(const json& diff)>;

    static_assert(reflect::is_reflected_v<E>, "keyed_collection element must be COMPOSITE_STRUCT-reflected");

    keyed_collection(std::string name, map_type* ref, config_type cfg = config_type::INITIALIZE)
        : m_name(std::move(name)), m_ref(ref), m_config(cfg) {}

    auto validate_element(elem_validator fn) -> keyed_collection& {
        m_elem_validators.push_back({std::move(fn), {}});
        return *this;
    }
    auto validate_element(elem_validator fn, std::string reason) -> keyed_collection& {
        m_elem_validators.push_back({std::move(fn), std::move(reason)});
        return *this;
    }
    auto validate_list(list_validator fn) -> keyed_collection& {
        m_list_validators.push_back({std::move(fn), {}});
        return *this;
    }
    auto validate_list(list_validator fn, std::string reason) -> keyed_collection& {
        m_list_validators.push_back({std::move(fn), std::move(reason)});
        return *this;
    }
    auto on_change(listener_fn fn) -> keyed_collection& {
        m_listeners.push_back(std::move(fn));
        return *this;
    }

    [[nodiscard]] auto name() const -> const std::string& override { return m_name; }
    [[nodiscard]] auto configurability() const -> config_type override { return m_config; }
    [[nodiscard]] auto type_name() const -> std::string override { return "keyed_collection"; }
    [[nodiscard]] auto size() const -> std::size_t { return m_ref->size(); }

    [[nodiscard]] auto encode() const -> json override {
        json o = json::object();
        for (const auto& [k, e] : *m_ref) {
            o[k] = reflect::encode(e);
        }
        return o;
    }

    [[nodiscard]] auto describe() const -> json override {
        return json{
            {"name", m_name},
            {"type", "keyed_collection"},
            {"configurability", (m_config == config_type::RUNTIME) ? "runtime" : "initialize"},
            {"element", reflect::type_schema<E>()}, // schema of each map value
        };
    }

    auto prepare(const json& patch, config_type ctx) -> void override {
        if (ctx == config_type::RUNTIME && m_config == config_type::INITIALIZE) {
            throw config_violation(m_name);
        }
        map_type candidate = *m_ref;
        std::vector<std::string> touched;
        if (patch.is_null()) {
            candidate.clear(); // RFC-7396 null: clear the whole collection
        } else if (patch.is_object()) {
            for (const auto& [k, v] : patch.items()) {
                if (v.is_null()) {
                    candidate.erase(k);
                } else {
                    reflect::merge(candidate[k], v, m_name + "/" + k);
                } // path-precise errors
                touched.push_back(k);
            }
        } else {
            throw validation_error(m_name + " (expected an object of key -> value/null, or null)");
        }
        for (const auto& k : touched) {
            auto it = candidate.find(k);
            if (it == candidate.end()) {
                continue;
            } // erased
            for (auto& ev : m_elem_validators) {
                if (!ev.fn(k, it->second)) {
                    if (ev.reason.empty()) {
                        throw validation_error(m_name + "/" + k);
                    }
                    throw validation_error(m_name + "/" + k, ev.reason);
                }
            }
        }
        for (auto& lv : m_list_validators) {
            if (!lv.fn(candidate)) {
                if (lv.reason.empty()) {
                    throw validation_error(m_name + " (list invariant)");
                }
                throw validation_error(m_name + " (list invariant)", lv.reason);
            }
        }
        m_pending.emplace(std::move(candidate));
    }

    auto commit() -> json override {
        if (!m_pending.has_value()) {
            return json();
        }
        const json d = collection_diff(*m_ref, *m_pending);
        if (d.empty()) {
            m_pending.reset();
            return json();
        }
        // Node-stable apply: overwrite surviving elements in place and erase/insert
        // only what changed, so a held reference to an unchanged element stays valid
        // across the commit (the whole-map move would invalidate every address).
        apply_node_stable(*m_ref, *m_pending);
        m_pending.reset();
        return d;
    }

    auto notify(const json& diff) -> void override {
        if (diff.is_null()) {
            return;
        }
        for (auto& l : m_listeners) {
            l(diff);
        }
    }

    auto abort() noexcept -> void override { m_pending.reset(); }

private:
    /// Make @p live equal @p cand while preserving the node (address) of every key
    /// present in both — erase keys not in cand, overwrite survivors in place,
    /// insert genuinely new keys.
    static auto apply_node_stable(map_type& live, const map_type& cand) -> void {
        for (auto it = live.begin(); it != live.end();) {
            if (cand.find(it->first) == cand.end()) {
                it = live.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto& [k, v] : cand) {
            auto it = live.find(k);
            if (it == live.end()) {
                live.emplace(k, v);
            } else {
                it->second = v;
            } // overwrite in place — address preserved
        }
    }

    static auto collection_diff(const map_type& a, const map_type& b) -> json {
        json d = json::object();
        for (const auto& [k, e] : b) {
            auto it = a.find(k);
            if (it == a.end()) {
                d[k] = reflect::encode(e);
            } else if (!reflect::equal(it->second, e)) {
                d[k] = reflect::diff(it->second, e);
            }
        }
        for (const auto& [k, e] : a) {
            (void)e;
            if (b.find(k) == b.end()) {
                d[k] = nullptr;
            }
        }
        return d;
    }

    std::string m_name;
    map_type* m_ref;
    config_type m_config;
    struct elem_entry {
        elem_validator fn;
        std::string reason;
    };
    struct list_entry {
        list_validator fn;
        std::string reason;
    };

    std::vector<elem_entry> m_elem_validators;
    std::vector<list_entry> m_list_validators;
    std::vector<listener_fn> m_listeners;
    std::optional<map_type> m_pending;
};

} // namespace composite::properties
