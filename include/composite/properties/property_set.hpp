/*
 * Copyright (C) 2024-2026 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "config.hpp" // composite::config<T>, config_binding_base/config_binding<T>
#include "typed.hpp"

#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace composite::properties {

/**
 * @brief Per-component registry of typed properties and keyed collections.
 *
 * Everything is JSON in / JSON out. `add()`/`add_keyed()` register a property
 * bound to a component member and return a reference for fluent
 * validate()/on_change(). `apply()` of a JSON object is an **atomic batch**:
 * every named property is prepared (validated against a candidate) before any
 * is committed, so a batch is all-or-nothing and a rejection mutates nothing.
 * `encode()` is the full state; `describe()` is the schema for introspection.
 *
 * This replaces the previous string-routed engine (the `property`,
 * `property_path`, `property_visitor`, `json_convert`, `serialization`,
 * `property_changeset`, and `property_handlers` machinery), and the
 * mutate-then-rollback / `_no_listener` / struct-list-cache complexity with it.
 */
class property_set {
public:
    /// Register a scalar/enum/optional/vector/reflected-struct property.
    template <typename T>
    auto add(std::string_view name, T& ref, config_type cfg = config_type::INITIALIZE) -> typed_property<T>& {
        auto prop = std::make_unique<typed_property<T>>(std::string{name}, &ref, cfg);
        auto& ref_out = *prop;
        insert(std::string{name}, std::move(prop));
        return ref_out;
    }

    /// Register a keyed collection (std::map<std::string, E>, E reflected).
    template <typename E>
    auto add_keyed(std::string_view name, std::map<std::string, E>& ref, config_type cfg = config_type::INITIALIZE)
        -> keyed_collection<E>& {
        auto prop = std::make_unique<keyed_collection<E>>(std::string{name}, &ref, cfg);
        auto& ref_out = *prop;
        insert(std::string{name}, std::move(prop));
        return ref_out;
    }

    /// Register a config<T>: the struct's top-level fields become top-level
    /// properties (PATCH {"field": v} works, encode() shows them at top level), but
    /// the WHOLE struct is the validate/commit unit. Coexists with add()/add_keyed()
    /// in the same set. @p default_cfg is the baseline configurability for fields
    /// that do not carry the `runtime` attribute.
    template <reflect::reflected T>
    auto add_config(::composite::config<T>& cfg, config_type default_cfg = config_type::INITIALIZE)
        -> ::composite::config<T>& {
        auto binding = std::make_unique<config_binding<T>>(cfg, default_cfg);
        auto* raw = binding.get();
        for (const auto& f : raw->field_names()) { // all-or-nothing collision check
            if (m_props.find(f) != m_props.end() || m_field_owner.find(f) != m_field_owner.end()) {
                throw std::logic_error("composite: duplicate property/field registration: " + f);
            }
        }
        for (const auto& f : raw->field_names()) {
            m_field_owner.emplace(f, raw);
            m_order.push_back(f); // field name at the wire top level
        }
        m_bindings.push_back(std::move(binding));
        return cfg;
    }

    [[nodiscard]] auto find(std::string_view name) -> property_base* {
        auto it = m_props.find(name);
        return it == m_props.end() ? nullptr : it->second.get();
    }
    [[nodiscard]] auto find(std::string_view name) const -> const property_base* {
        auto it = m_props.find(name);
        return it == m_props.end() ? nullptr : it->second.get();
    }
    [[nodiscard]] auto contains(std::string_view name) const -> bool {
        return m_props.find(name) != m_props.end() || m_field_owner.find(name) != m_field_owner.end();
    }
    /// The config_binding owning a top-level field name, or nullptr if @p name is a
    /// plain property / unknown.
    [[nodiscard]] auto find_field_owner(std::string_view name) -> config_binding_base* {
        auto it = m_field_owner.find(name);
        return it == m_field_owner.end() ? nullptr : it->second;
    }
    [[nodiscard]] auto names() const -> const std::vector<std::string>& { return m_order; }
    [[nodiscard]] auto empty() const -> bool { return m_order.empty(); }

    /**
     * @brief Apply a JSON object `{name: value}` as an atomic batch.
     * @return aggregate diff `{name: diff}` of properties that actually changed.
     * @throws unknown_property / config_violation / validation_error — and on any
     *         throw, every prepared candidate is aborted (nothing is committed).
     */
    auto apply(const json& obj, config_type ctx = config_type::INITIALIZE, bool allow_unknown = false) -> json {
        if (!obj.is_object()) {
            throw validation_error("property_set apply expects a JSON object");
        }
        // Grouping pre-pass: a plain property key prepares individually; a config
        // FIELD key is collected under its owning binding into ONE struct sub-patch,
        // so the whole struct validates + commits once. When no config<T> is
        // registered, m_field_owner is empty and this is byte-for-byte the old path.
        std::vector<std::pair<property_base*, const json*>> plain;
        std::map<config_binding_base*, json> grouped;
        for (const auto& [key, value] : obj.items()) {
            if (auto* p = find(key)) {
                plain.emplace_back(p, &value);
            } else if (auto* b = find_field_owner(key)) {
                grouped[b][key] = value;
            } else if (!allow_unknown) {
                throw unknown_property(key);
            }
        }

        // Phase 1: prepare all (plain individually; each touched binding ONCE with its
        // sub-patch). Bindings are prepared in REGISTRATION order (m_bindings), not the
        // pointer order of `grouped`, so on_apply reactions later fire deterministically
        // across multiple config<T> (commit/notify inherit this order).
        std::vector<property_base*> prepared;
        try {
            for (auto& [p, v] : plain) {
                p->prepare(*v, ctx);
                prepared.push_back(p);
            }
            for (const auto& bptr : m_bindings) {
                auto git = grouped.find(bptr.get());
                if (git == grouped.end()) {
                    continue;
                }
                bptr->prepare(git->second, ctx);
                prepared.push_back(bptr.get());
            }
        } catch (...) {
            for (auto* p : prepared) {
                p->abort();
            }
            throw;
        }

        // Phase 2: commit every prepared candidate (swap values only, NO listeners).
        std::vector<std::pair<property_base*, json>> committed;
        for (auto* p : prepared) {
            json d = p->commit();
            if (!d.is_null()) {
                committed.emplace_back(p, std::move(d));
            }
        }

        // Phase 3: the batch is fully live. Notify plain props in REGISTRATION order
        // (m_order) for determinism, then bindings; a binding's diff is field-flat
        // ({gain: subdiff, ...}) so it MERGES into the aggregate (keeping the wire
        // shape: fields at top level). A throwing listener is non-fatal (the values
        // are already committed) and routed to the error sink.
        json diffs = json::object();
        std::map<std::string, std::pair<property_base*, const json*>> plain_committed;
        std::vector<std::pair<config_binding_base*, const json*>> binding_committed;
        for (auto& [p, d] : committed) {
            if (auto* b = dynamic_cast<config_binding_base*>(p)) {
                binding_committed.emplace_back(b, &d);
            } else {
                plain_committed.emplace(p->name(), std::pair{p, &d});
            }
        }
        for (const auto& name : m_order) {
            auto it = plain_committed.find(name);
            if (it == plain_committed.end()) {
                continue;
            } // field names + unchanged props
            diffs[name] = *it->second.second;
            notify_guarded(it->second.first, name, *it->second.second);
        }
        for (auto& [b, dptr] : binding_committed) {
            diffs.update(*dptr); // field-flat merge into the aggregate diff
            notify_guarded(b, b->name(), *dptr);
        }
        return diffs;
    }

    /// Run any staged config<T> on_apply reactions. Called at the worker loop-top
    /// (worker thread, before process()) and inline by the writer when there is no live
    /// worker. No-op when no config<T> is registered (the common case) — a bare loop over
    /// an empty vector. Each binding's run_pending() is itself a no-op when nothing is staged.
    auto run_pending_reactions() -> void {
        for (const auto& b : m_bindings) {
            b->run_pending();
        }
    }

    /// Optional sink for exceptions thrown by post-commit on_change listeners. Set
    /// by the owning component so a listener failure is logged as a warning rather
    /// than silently swallowed (the batch is already live; see Phase 3 above).
    /// Receives (property name, what()).
    auto set_listener_error_handler(std::function<void(const std::string&, const char*)> fn) -> void {
        m_listener_error_sink = std::move(fn);
    }

    /// Apply a single named property/field value. Delegated to the batch path so a
    /// single config-field write routes through the grouping pre-pass (whole-struct
    /// commit); for a plain property this is a one-key batch (identical result).
    auto apply(std::string_view name, const json& value, config_type ctx = config_type::INITIALIZE) -> bool {
        if (!contains(name)) {
            throw unknown_property(std::string{name});
        }
        return !apply(json{{std::string{name}, value}}, ctx).empty();
    }

    /// Typed read of a property's (or config field's) current value.
    template <typename T>
    [[nodiscard]] auto get(std::string_view name) const -> T {
        if (const auto* p = find(name)) {
            const auto* tp = dynamic_cast<const typed_property<T>*>(p);
            if (tp == nullptr) {
                throw validation_error(std::string{name} + " (type mismatch)");
            }
            return tp->get();
        }
        if (auto it = m_field_owner.find(name); it != m_field_owner.end()) {
            const json v = it->second->encode_all().at(std::string{name});
            T out{};
            reflect::decode(v, out, std::string{name});
            return out;
        }
        throw unknown_property(std::string{name});
    }

    /// Full current state as a JSON object `{name: value}` (registration order).
    /// Config fields appear at the top level alongside plain properties.
    [[nodiscard]] auto encode() const -> json {
        json o = json::object();
        std::map<const config_binding_base*, json> binding_cache; // encode each struct once
        for (const auto& n : m_order) {
            if (auto pit = m_props.find(n); pit != m_props.end()) {
                o[n] = pit->second->encode();
            } else if (auto fit = m_field_owner.find(n); fit != m_field_owner.end()) {
                const auto* b = fit->second;
                auto cit = binding_cache.find(b);
                if (cit == binding_cache.end()) {
                    cit = binding_cache.emplace(b, b->encode_all()).first;
                }
                o[n] = cit->second.at(n);
            }
        }
        return o;
    }

    /// Schema for introspection (registration order). Each entry is a property's (or
    /// config field's) full describe(): name, type (nested fields / array items / enum
    /// choices / integer range), configurability, default, unit, and attribute hints.
    [[nodiscard]] auto describe() const -> json {
        json arr = json::array();
        for (const auto& n : m_order) {
            if (auto pit = m_props.find(n); pit != m_props.end()) {
                arr.push_back(pit->second->describe());
            } else if (auto fit = m_field_owner.find(n); fit != m_field_owner.end()) {
                arr.push_back(fit->second->describe_field(n));
            }
        }
        return arr;
    }

private:
    /// notify() a property, routing a throwing listener to the error sink (the batch
    /// is already committed and live; never turn that into a 400).
    auto notify_guarded(property_base* p, const std::string& name, const json& diff) -> void {
        try {
            p->notify(diff);
        } catch (const std::exception& ex) {
            if (m_listener_error_sink) {
                m_listener_error_sink(name, ex.what());
            }
        } catch (...) {
            if (m_listener_error_sink) {
                m_listener_error_sink(name, "unknown exception");
            }
        }
    }

    auto insert(std::string name, std::unique_ptr<property_base> prop) -> void {
        // Reject a name already taken by a config<T> field too (both directions of the
        // plain-property / config-field collision are construction-time errors).
        if (m_field_owner.find(name) != m_field_owner.end()) {
            throw std::logic_error("composite: property name collides with a config field: " + name);
        }
        auto [it, inserted] = m_props.try_emplace(name, std::move(prop));
        if (!inserted) {
            // Duplicate registration is a construction-time programming error.
            // Throw rather than drop `prop` (which would leave add()'s returned
            // reference dangling at the freed object).
            throw std::logic_error("composite: duplicate property registration: " + name);
        }
        m_order.push_back(std::move(name));
    }

    std::map<std::string, std::unique_ptr<property_base>, std::less<>> m_props;
    std::vector<std::string> m_order; ///< registration order for stable encode/describe
    std::function<void(const std::string&, const char*)> m_listener_error_sink;
    // config<T> support: each top-level field name routes to its owning binding;
    // m_bindings owns the binding objects (one per registered config<T>).
    std::map<std::string, config_binding_base*, std::less<>> m_field_owner;
    std::vector<std::unique_ptr<config_binding_base>> m_bindings;
};

} // namespace composite::properties
