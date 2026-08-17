/*
 * Copyright (C) 2024-2026 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "changes.hpp"
#include "typed.hpp" // property_base, typed_property, config_type, the error types

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

/**
 * @file config.hpp
 * @brief composite::config<T> — one reflected config struct as the unit of
 *        configuration.
 *
 * A component declares a reflected struct (COMPOSITE_FIELDS) and holds a
 * `config<T>` member. add_config() projects the struct's top-level fields onto the
 * property-set namespace (so the wire contract is unchanged: PATCH {"gain":2.5}
 * still works), but the WHOLE struct is the validate/commit unit — a single-field
 * write validates every field's constraint plus cross-field invariants, commits
 * once, and the reaction `on_apply(prev, changes<T>)` sees a scoped diff. The worker
 * reads `cfg->field` with zero overhead (operator-> is a plain member read).
 *
 * config<T> is a thin facade over ONE typed_property<T> (the existing candidate ->
 * validate -> swap -> diff engine, which already handles reflected T); config_binding
 * wraps it as a property_base and adds per-field configurability + the field-name
 * projection. See PLAN.md M3 Phase 1.
 */
namespace composite {

namespace properties {
template <reflect::reflected T>
class config_binding;
}

/**
 * @brief Framework-owned holder of a reflected config struct.
 *
 * Non-copyable/movable: a config_binding holds a pointer into m_value, so the object
 * must stay put once mounted (it is a component member, like the bound members it
 * replaces).
 */
template <reflect::reflected T>
class config {
public:
    using value_type = T;
    using reaction_fn = std::function<void(const T& prev, const changes<T>& ch)>;

    config() = default;
    explicit config(T initial) : m_value(std::move(initial)) {}

    config(const config&) = delete;
    config(config&&) = delete;
    auto operator=(const config&) -> config& = delete;
    auto operator=(config&&) -> config& = delete;

    // HOT PATH — byte-identical to a plain member read (inlines to &m_value, same as
    // std::optional::operator->). The worker reads this only while NOT parked; the
    // cold-path writer swaps m_value only while the worker is parked.
    [[nodiscard]] constexpr auto operator->() const noexcept -> const T* { return &m_value; }
    [[nodiscard]] constexpr auto operator*() const noexcept -> const T& { return m_value; }
    [[nodiscard]] constexpr auto get() const noexcept -> const T& { return m_value; }

    /// Reaction run after a committed change. The signature is FIXED across Phase 1
    /// (writer-under-park) and Phase 2 (worker loop-top) — flipping needs no change here.
    auto on_apply(reaction_fn fn) -> config& {
        m_on_apply = std::move(fn);
        return *this;
    }

    /// Whole-struct (cross-field) invariant. The two-arg form supplies a reason.
    auto validate(std::function<bool(const T&)> fn, std::string why = {}) -> config& {
        m_validators.push_back({std::move(fn), std::move(why)});
        return *this;
    }

private:
    template <reflect::reflected>
    friend class properties::config_binding;
    struct val_entry {
        std::function<bool(const T&)> fn;
        std::string reason;
    };

    T m_value{}; ///< the live struct; cfg->field reads this
    reaction_fn m_on_apply;
    std::vector<val_entry> m_validators;
};

namespace properties {

// ---- JSON-Schema-flavored field-entry helpers (shared by describe + 2020-12) ----
namespace detail {

inline auto trim_double(double d) -> std::string {
    std::string s = std::to_string(d);
    if (s.find('.') == std::string::npos) {
        return s;
    }
    auto last = s.find_last_not_of('0');
    if (s[last] == '.') {
        ++last;
    } // keep one trailing zero: "1." -> "1.0"
    return s.substr(0, last + 1);
}

} // namespace detail

/**
 * @brief Abstract binding interface used by property_set for field projection.
 *
 * Derives from property_base so a binding slots into the existing
 * prepare/commit/notify batch loop — but adds the field-level projection methods.
 * property_base itself is UNCHANGED (typed_property's vtable/ABI is untouched).
 */
class config_binding_base : public property_base {
public:
    /// Top-level field names this binding contributes to the property-set namespace.
    [[nodiscard]] virtual auto field_names() const -> const std::vector<std::string>& = 0;
    /// Full struct state as {field: value} (top-level fields).
    [[nodiscard]] virtual auto encode_all() const -> json = 0;
    /// describe() entry for a single field (same shape as typed_property::describe()).
    [[nodiscard]] virtual auto describe_field(const std::string& field) const -> json = 0;
    /// Run a staged on_apply reaction, if any: invoked at the worker loop-top, or
    /// inline by the writer when there is no live worker. Idempotent / safe to call when
    /// nothing is pending.
    virtual auto run_pending() -> void = 0;
};

/**
 * @brief property_base wrapper over a single typed_property<T> bound to config<T>.
 */
template <reflect::reflected T>
class config_binding final : public config_binding_base {
public:
    config_binding(composite::config<T>& cfg, config_type default_cfg)
        : m_cfg(&cfg), m_inner("config", &cfg.m_value, config_type::RUNTIME), // inner permissive; per-field gated below
          m_default_cfg(default_cfg) {
        build_field_table();
        install_attr_validators();
        for (const auto& v : cfg.m_validators) { // cross-field invariants
            if (v.reason.empty()) {
                m_inner.validate(v.fn);
            } else {
                m_inner.validate(v.fn, v.reason);
            }
        }
        build_describe(); // precompute per-field schema entries (cold path, once)
    }

    // ---- property_base ----
    [[nodiscard]] auto name() const -> const std::string& override { return m_name; }
    [[nodiscard]] auto configurability() const -> config_type override { return m_default_cfg; }
    [[nodiscard]] auto type_name() const -> std::string override { return "config_struct"; }
    [[nodiscard]] auto encode() const -> json override { return m_inner.encode(); }
    [[nodiscard]] auto describe() const -> json override {
        // whole-struct entry list (one per field), for completeness; property_set
        // emits per-field entries via describe_field().
        json arr = json::array();
        for (const auto& n : m_field_names) {
            arr.push_back(m_field_describe.at(n));
        }
        return arr;
    }

    auto prepare(const json& patch, config_type ctx) -> void override {
        enforce_field_configurability(patch, ctx);
        m_inner.prepare(patch, config_type::RUNTIME); // merge whole struct, run ALL validators
    }

    auto commit() -> json override {
        if (!m_cfg->m_on_apply) { // no reaction -> skip the prev snapshot entirely
            return m_inner.commit();
        }
        m_prev = m_cfg->m_value; // snapshot BEFORE the inner swap (becomes on_apply's `prev`)
        return m_inner.commit(); // engine swaps m_cfg->m_value, returns the scoped struct diff
    }

    auto notify(const json& diff) -> void override {
        m_inner.notify(diff); // fire any typed_property on_change listeners (none by default)
        // Do NOT run the reaction here (this runs under the park on the writer's
        // thread). Stage it for the worker to run at its next loop-top — same thread as
        // process(), which is what makes a config-driven derived-state rebuild safe.
        stage_reaction(diff);
    }

    auto abort() noexcept -> void override { m_inner.abort(); } // nothing staged before commit

    // ---- config_binding_base ----
    [[nodiscard]] auto field_names() const -> const std::vector<std::string>& override { return m_field_names; }
    [[nodiscard]] auto encode_all() const -> json override { return m_inner.encode(); }
    [[nodiscard]] auto describe_field(const std::string& field) const -> json override {
        auto it = m_field_describe.find(field);
        return it == m_field_describe.end() ? json(nullptr) : it->second;
    }

    auto run_pending() -> void override {
        // Run a staged reaction, if any. Called at the worker loop-top (worker thread,
        // before process()) every iteration, or inline by the writer / stop path when
        // there is no live worker. HOT PATH: lock-free fast exit when nothing is staged —
        // the worker calls this each loop, so we only take the mailbox lock when a config
        // change has actually landed (a cold event).
        if (!m_pending_reaction.load(std::memory_order_acquire)) {
            return;
        }
        json diff;
        T prev{};
        {
            std::scoped_lock lk{m_mailbox_mtx};
            // Re-check under the lock; the exchange serializes any double-drain (e.g. an
            // inline drain racing a just-started worker) — the loser sees false and no-ops.
            if (!m_pending_reaction.exchange(false, std::memory_order_acq_rel)) {
                return;
            }
            diff = std::move(m_pending_diff);
            m_pending_diff = json(); // clear BEFORE on_apply so a reentrant stage isn't clobbered
            prev = std::move(m_pending_prev);
        }
        // Run on_apply OUTSIDE the lock: it may reentrantly set_properties() (a supported
        // worker-self-write), which re-stages a fresh reaction into the now-clean mailbox.
        // changes<T> binds the LOCAL diff (outlives this call); new_value reads the live value.
        const changes<T> ch{diff, m_cfg->m_value};
        m_cfg->m_on_apply(prev, ch);
    }

private:
    // Stage the reaction for the worker to run at its next loop-top. If a prior
    // reaction has not yet drained, COALESCE: keep the EARLIEST prev (the value before
    // the first un-drained change) and union the changed top-level fields (later value
    // wins), so on_apply sees "state before I last looked -> state I now read". Runs
    // under the mailbox lock (serialized with a cold-path drain).
    void stage_reaction(const json& diff) {
        if (!m_cfg->m_on_apply || diff.is_null()) {
            return;
        }
        std::scoped_lock lk{m_mailbox_mtx};
        if (!m_pending_reaction.load(std::memory_order_relaxed)) {
            m_pending_prev = m_prev;
            m_pending_diff = diff;
        } else {
            // Key-by-key overlay, NOT merge_patch: a field whose new value is JSON null (a
            // cleared std::optional, or a json-null leaf) is a VALUE, not an RFC-7396
            // deletion — merge_patch would drop the key and changed(&field) would wrongly
            // report false at drain. Only top-level key membership matters here, since
            // changes<T>::new_value reads the live committed value, not this diff.
            for (auto it = diff.begin(); it != diff.end(); ++it) {
                m_pending_diff[it.key()] = it.value();
            }
        }
        m_pending_reaction.store(true, std::memory_order_release);
    }

    void build_field_table() {
        std::apply(
            [&](auto&&... f) {
                (
                    [&] {
                        const std::string nm{f.name};
                        m_field_names.push_back(nm);
                        const bool is_rt = reflect::has_attr<reflect::runtime_attr>(f.attrs);
                        m_field_cfg.emplace(nm, is_rt ? config_type::RUNTIME : m_default_cfg);
                    }(),
                    ...);
            },
            reflect::descriptor<T>::fields());
    }

    void install_attr_validators() {
        std::apply([&](auto&&... f) { (install_for_field(f), ...); }, reflect::descriptor<T>::fields());
    }

    template <typename F>
    void install_for_field(const F& f) {
        using M = typename F::member_type;
        using Attrs = typename F::attrs_type;
        // Compile-time guard: an attribute on an incompatible field type is a typo, not a
        // silently-ignored hint. Without this, describe()/schema_2020_12 would advertise a
        // constraint that prepare() never enforces.
        static_assert(!reflect::has_attr<reflect::range_attr>(Attrs{}) ||
                          (std::is_arithmetic_v<M> && !std::is_same_v<M, bool>),
                      "range() requires an arithmetic (non-bool) field");
        static_assert(!reflect::has_attr<reflect::power_of_two_attr>(Attrs{}) ||
                          (std::is_integral_v<M> && !std::is_same_v<M, bool>),
                      "power_of_two requires an integer field");
        static_assert(!reflect::tuple_has_one_of<Attrs>() || (std::is_same_v<M, std::string> || std::is_enum_v<M>),
                      "one_of() requires a string or enum field");
        const auto ptr = f.ptr;
        const std::string nm{f.name};
        if constexpr (std::is_arithmetic_v<M> && !std::is_same_v<M, bool>) {
            if (const auto* r = reflect::find_attr<reflect::range_attr>(f.attrs)) {
                const double lo = r->lo;
                const double hi = r->hi;
                m_inner.validate(
                    [ptr, lo, hi](const T& c) {
                        const double v = static_cast<double>(c.*ptr);
                        return v >= lo && v <= hi;
                    },
                    nm + " out of range [" + detail::trim_double(lo) + ", " + detail::trim_double(hi) + "]");
            }
        }
        if constexpr (std::is_integral_v<M> && !std::is_same_v<M, bool>) {
            if (reflect::has_attr<reflect::power_of_two_attr>(f.attrs)) {
                m_inner.validate(
                    [ptr](const T& c) {
                        const auto v = c.*ptr;
                        return v > 0 && (v & (v - 1)) == 0;
                    },
                    nm + " must be a power of two");
            }
        }
        reflect::for_each_attr(f.attrs, [&](const auto& a) {
            using A = std::decay_t<decltype(a)>;
            if constexpr (reflect::is_one_of_attr<A>::value) {
                std::vector<std::string> choices;
                for (auto sv : a.values) {
                    choices.emplace_back(sv);
                }
                if constexpr (std::is_same_v<M, std::string>) {
                    m_inner.validate(
                        [ptr, choices](const T& c) {
                            for (const auto& s : choices) {
                                if (c.*ptr == s) {
                                    return true;
                                }
                            }
                            return false;
                        },
                        nm + " must be one of the allowed values");
                } else if constexpr (std::is_enum_v<M>) {
                    m_inner.validate(
                        [ptr, choices](const T& c) {
                            const json e = reflect::encode(c.*ptr);
                            for (const auto& s : choices) {
                                if (e == s) {
                                    return true;
                                }
                            }
                            return false;
                        },
                        nm + " must be one of the allowed values");
                }
            }
        });
    }

    void enforce_field_configurability(const json& patch, config_type ctx) {
        if (ctx != config_type::RUNTIME) {
            return;
        } // INITIALIZE may set anything
        if (!patch.is_object()) {
            return;
        } // type errors handled by the engine
        for (auto it = patch.begin(); it != patch.end(); ++it) {
            auto cit = m_field_cfg.find(it.key());
            if (cit != m_field_cfg.end() && cit->second == config_type::INITIALIZE) {
                throw config_violation(it.key());
            }
        }
    }

    void build_describe() {
        const json defaults = m_inner.encode(); // initial values == registration defaults
        std::apply([&](auto&&... f) { (build_field_entry(f, defaults), ...); }, reflect::descriptor<T>::fields());
    }

    template <typename F>
    void build_field_entry(const F& f, const json& defaults) {
        using M = typename F::member_type;
        const std::string nm{f.name};
        json e = reflect::type_schema<M>(); // {type, [fields|items|choices|minimum|maximum]}
        e["name"] = nm;
        e["configurability"] = (m_field_cfg.at(nm) == config_type::RUNTIME) ? "runtime" : "initialize";
        e["default"] = defaults.at(nm);
        if (const auto* u = reflect::find_attr<reflect::unit_attr>(f.attrs)) {
            e["unit"] = std::string(u->text);
        }
        if (const auto* d = reflect::find_attr<reflect::doc_attr>(f.attrs)) {
            e["description"] = std::string(d->text);
        }
        if (const auto* r = reflect::find_attr<reflect::range_attr>(f.attrs)) {
            e["minimum"] = r->lo;
            e["maximum"] = r->hi;
        }
        if (reflect::has_attr<reflect::power_of_two_attr>(f.attrs)) {
            e["powerOfTwo"] = true;
        }
        reflect::for_each_attr(f.attrs, [&](const auto& a) {
            using A = std::decay_t<decltype(a)>;
            if constexpr (reflect::is_one_of_attr<A>::value) {
                json ch = json::array();
                for (auto sv : a.values) {
                    ch.push_back(std::string(sv));
                }
                e["choices"] = std::move(ch);
            }
        });
        m_field_describe.emplace(nm, std::move(e));
    }

    std::string m_name{"config"};
    composite::config<T>* m_cfg;
    typed_property<T> m_inner; ///< the reused engine object (candidate/diff/null-reset)
    config_type m_default_cfg;
    T m_prev{}; ///< value snapshotted in commit(), staged as on_apply's prev
    // Reaction mailbox: staged by a writer, drained at the worker loop-top (or inline /
    // on the stop path when there is no worker). The atomic flag is the lock-free hot-path
    // gate; m_mailbox_mtx serializes the cold-path stage vs drain of the json/T payload.
    std::atomic<bool> m_pending_reaction{false};
    std::mutex m_mailbox_mtx;
    json m_pending_diff;
    T m_pending_prev{};
    std::vector<std::string> m_field_names;
    std::map<std::string, config_type> m_field_cfg;
    std::map<std::string, json> m_field_describe;
};

// ---------------------------------------------------------------------------
// JSON Schema 2020-12 export. Built on the internal reflect::type_schema<M>()
// vocabulary + the field attributes.
//
// to_2020_12()/to_schema_entry() are what GET /app/components/:id/schema
// publishes, via property_set::schema(). schema_2020_12() below is the
// standalone whole-struct form for a single config<T> — usable (and tested)
// without a property_set, e.g. to emit a schema for a config type on its own.
// ---------------------------------------------------------------------------

/// Map the legacy type_schema vocabulary to JSON Schema 2020-12 (fields->properties,
/// choices->enum), recursively. Integer minimum/maximum already match.
inline auto to_2020_12(json node) -> json {
    if (!node.is_object()) {
        return node;
    }
    if (node.contains("fields")) {
        json props = json::object();
        for (auto it = node["fields"].begin(); it != node["fields"].end(); ++it) {
            props[it.key()] = to_2020_12(it.value());
        }
        node.erase("fields");
        node["properties"] = std::move(props);
        node["additionalProperties"] = false;
    }
    if (node.contains("items")) {
        node["items"] = to_2020_12(node["items"]);
    }
    if (node.contains("choices")) {
        node["enum"] = node["choices"];
        node.erase("choices");
    }
    return node;
}

/// Convert ONE describe()/describe_field() entry into a JSON Schema 2020-12 sub-schema.
///
/// The entry carries two kinds of key: standard ones that pass straight through (`type`,
/// `minimum`, `maximum`, `default`, `description`) and composite-specific ones that are not
/// JSON Schema keywords. The latter are re-emitted under the `x-` vendor-extension prefix so
/// the result is a conformant schema rather than a lookalike:
///   `unit`         -> `x-composite-unit`
///   `powerOfTwo`   -> `x-composite-powerOfTwo`
///   `configurability` -> `x-composite-configurability`
/// `name` is dropped: in the published document the property name is the KEY, not a field.
/// Structural translation (`fields`->`properties`, `choices`->`enum`, recursion through
/// `items`) is delegated to to_2020_12(), which also handles nested reflected members.
inline auto to_schema_entry(json entry) -> json {
    auto take = [&entry](const char* key) -> json {
        if (!entry.contains(key)) {
            return json(nullptr);
        }
        json v = entry.at(key);
        entry.erase(key);
        return v;
    };
    const json unit = take("unit");
    const json configurability = take("configurability");
    const json power_of_two = take("powerOfTwo");
    entry.erase("name");

    json out = to_2020_12(std::move(entry));
    if (!unit.is_null()) {
        out["x-composite-unit"] = unit;
    }
    if (!configurability.is_null()) {
        out["x-composite-configurability"] = configurability;
    }
    if (!power_of_two.is_null()) {
        out["x-composite-powerOfTwo"] = power_of_two;
    }
    return out;
}

template <reflect::reflected T>
auto schema_2020_12(const config<T>& live) -> reflect::json {
    using json = reflect::json;
    json props = json::object();
    json required = json::array();
    const json defaults = reflect::encode(*live);
    std::apply(
        [&](auto&&... f) {
            (
                [&] {
                    using M = typename std::decay_t<decltype(f)>::member_type;
                    const std::string nm{f.name};
                    json p = to_2020_12(reflect::type_schema<M>());
                    if (const auto* r = reflect::find_attr<reflect::range_attr>(f.attrs)) {
                        p["minimum"] = r->lo;
                        p["maximum"] = r->hi;
                    }
                    if (const auto* u = reflect::find_attr<reflect::unit_attr>(f.attrs)) {
                        p["x-composite-unit"] = std::string(u->text);
                    }
                    if (const auto* d = reflect::find_attr<reflect::doc_attr>(f.attrs)) {
                        p["description"] = std::string(d->text);
                    }
                    if (reflect::has_attr<reflect::power_of_two_attr>(f.attrs)) {
                        p["x-composite-powerOfTwo"] = true;
                    }
                    reflect::for_each_attr(f.attrs, [&](const auto& a) {
                        using A = std::decay_t<decltype(a)>;
                        if constexpr (reflect::is_one_of_attr<A>::value) {
                            json e = json::array();
                            for (auto sv : a.values) {
                                e.push_back(std::string(sv));
                            }
                            p["enum"] = std::move(e);
                        }
                    });
                    p["default"] = defaults.at(nm);
                    props[nm] = std::move(p);
                    if (!reflect::is_optional<M>::value) {
                        required.push_back(nm);
                    }
                }(),
                ...);
        },
        reflect::descriptor<T>::fields());
    return json{
        {"$schema", "https://json-schema.org/draft/2020-12/schema"},
        {"type", "object"},
        {"additionalProperties", false},
        {"properties", std::move(props)},
        {"required", std::move(required)},
    };
}

} // namespace properties
} // namespace composite
