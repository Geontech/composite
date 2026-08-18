/*
 * Copyright (C) 2024-2026 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "composite/core/component.hpp"

#include <memory>
#include <string>
#include <string_view>

/**
 * @file register.hpp
 * @brief The single dynamic-component ABI: create_args + the ABI-version handshake +
 *        COMPOSITE_REGISTER_COMPONENT.
 *
 * A dynamically-loaded component (.so) exports exactly ONE factory ABI:
 *
 *     extern "C" std::shared_ptr<composite::component>
 *     create(std::string_view id, const composite::create_args& args);
 *
 * plus an ABI-version symbol the loader checks BEFORE calling create(). Components
 * declare this with the COMPOSITE_REGISTER_COMPONENT / COMPOSITE_REGISTER_SIMPLE macros
 * rather than hand-writing the extern "C" block — which previously drifted into three
 * incompatible signatures (create(), create(type), create(id), create(id, arg)) that the
 * loader called through mismatched function pointers (UB) and that silently ignored the
 * configured id.
 */
namespace composite {

/**
 * @brief ABI version of the dynamic-component contract.
 *
 * Independent of the library semver: bump ONLY when create_args, create()'s signature,
 * or the component vtable/ownership contract changes in an ABI-incompatible way. The
 * loader refuses to call create() on a library whose composite_abi_version() differs
 * from this value (a stale/foreign build whose contract may not match).
 */
inline constexpr unsigned long abi_version = 1;

/**
 * @brief Construction-time arguments handed to a component factory.
 *
 * Distinct from runtime properties (which flow through set_properties() AFTER
 * construction): @ref values carries only what must be known to BUILD the object —
 * chiefly the template discriminator under "type" (e.g. {"type": "cf32"}), but any
 * number of named values are supported. Passed by const-reference across the dlopen
 * boundary; extensible — new framework-supplied fields can be appended without changing
 * create()'s signature (the ABI-version handshake guards real incompatibilities).
 */
struct create_args {
    properties::json values{properties::json::object()}; ///< a JSON object of named args

    /// True if no args were supplied.
    [[nodiscard]] auto empty() const -> bool { return !values.is_object() || values.empty(); }
    /// The "type" discriminator (template variant selector), or "" if absent.
    [[nodiscard]] auto type() const -> std::string {
        return values.is_object() ? values.value("type", std::string{}) : std::string{};
    }
    /// A named arg with a typed fallback.
    template <typename T = std::string>
    [[nodiscard]] auto value(std::string_view key, T fallback = T{}) const -> T {
        return values.is_object() ? values.value(std::string(key), fallback) : fallback;
    }
};

} // namespace composite

/**
 * Register a component's dynamic factory. The argument is any callable
 *   (std::string_view id, const composite::create_args& args)
 *       -> std::shared_ptr<composite::component>
 * Emits the C ABI the loader expects: the ABI-version handshake symbol + the single
 * create(). Variadic so a factory lambda whose body contains commas (template arguments,
 * std::format calls) is accepted as one macro argument.
 *
 * Build the component with composite::make_component<CLASS>(id) inside the factory (as
 * COMPOSITE_REGISTER_SIMPLE does), not make_shared: its deleter stops the component while
 * the leaf type is intact, so dropping the last shared_ptr of a still-running component
 * tears down the worker safely instead of racing destruction.
 */
#define COMPOSITE_REGISTER_COMPONENT(...)                                                                              \
    extern "C" {                                                                                                       \
    unsigned long composite_abi_version() {                                                                            \
        return ::composite::abi_version;                                                                               \
    }                                                                                                                  \
    std::shared_ptr<::composite::component> create(std::string_view id, const ::composite::create_args& args) {        \
        return (__VA_ARGS__)(id, args);                                                                                \
    }                                                                                                                  \
    }

/**
 * Convenience for a non-templated component with a `Class(std::string_view id)` ctor and
 * no construction args.
 */
#define COMPOSITE_REGISTER_SIMPLE(CLASS)                                                                               \
    COMPOSITE_REGISTER_COMPONENT(                                                                                      \
        [](std::string_view id, const ::composite::create_args&) { return ::composite::make_component<CLASS>(id); })
