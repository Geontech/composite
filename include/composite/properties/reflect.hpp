/*
 * Copyright (C) 2024-2026 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

/**
 * @file reflect.hpp
 * @brief Compile-time struct reflection for the property system.
 *
 * One declaration per struct:
 * @code
 * struct ChannelConfig { std::string name; double center_freq{}; Window window{}; };
 * COMPOSITE_STRUCT(ChannelConfig, name, center_freq, window);
 * COMPOSITE_ENUM(Window, hann, hamming, blackman_harris);
 * @endcode
 * yields, generically (no per-struct hand-written serializer):
 *  - encode/decode  : struct <-> nlohmann::json
 *  - merge          : RFC 7396 JSON Merge Patch (null resets; objects merge; arrays/scalars replace)
 *  - diff           : the 7396 patch that turns one value into another (the change-notification payload)
 *  - equal          : structural equality
 * Field types may be scalars, std::string, bool, enums (with COMPOSITE_ENUM),
 * std::optional<T>, std::vector<T>, nested reflected structs, or nlohmann::json
 * (an opaque 7396-merged leaf). See ASSESSMENT.md §4.A / §11.
 */
namespace composite::reflect {

using json = nlohmann::json;

// ---------------------------------------------------------------- descriptor

/// A reflected field: its JSON name, a pointer-to-member (compiler-checked), and an
/// optional tuple of attributes (range/unit/doc/runtime/...). The defaulted empty
/// Attrs keeps every existing field()/algorithm call source-compatible — encode/
/// decode/merge/diff/equal/type_schema only ever read `.name`/`.ptr`.
template <typename C, typename M, typename Attrs = std::tuple<>>
struct field_t {
    std::string_view name;
    M C::* ptr;
    Attrs attrs{};
    using member_type = M;
    using class_type = C;
    using attrs_type = Attrs;

    /// Attach attributes; returns a new field_t carrying them (used by the
    /// COMPOSITE_FIELDS macro's fld(...).with(...)).
    template <typename... A>
    constexpr auto with(A... a) const -> field_t<C, M, std::tuple<A...>> {
        return field_t<C, M, std::tuple<A...>>{name, ptr, std::make_tuple(a...)};
    }
};

template <typename C, typename M>
constexpr auto field(std::string_view name, M C::* ptr) -> field_t<C, M> {
    return field_t<C, M>{name, ptr};
}
/// fld(): the spelling the COMPOSITE_FIELDS macro emits; identical to field().
template <typename C, typename M>
constexpr auto fld(std::string_view name, M C::* ptr) -> field_t<C, M> {
    return field_t<C, M>{name, ptr};
}

/// Specialized by COMPOSITE_STRUCT; the empty primary means "not reflected".
template <typename T, typename = void>
struct descriptor {};

template <typename T, typename = void>
struct is_reflected : std::false_type {};
template <typename T>
struct is_reflected<T, std::void_t<decltype(descriptor<T>::fields())>> : std::true_type {};
template <typename T>
inline constexpr bool is_reflected_v = is_reflected<T>::value;

/// `requires reflected<T>` — turns the deep descriptor<T> backtrace into one line.
template <typename T>
concept reflected = is_reflected_v<T>;

// ----------------------------------------------- ADL reflection (COMPOSITE_FIELDS)
// COMPOSITE_FIELDS declares a hidden friend `composite_reflect_fields(fields_tag<T>)`
// so a struct can be reflected INSIDE a user namespace (an explicit specialization
// of descriptor<T> is only legal at composite::reflect scope). The tag's associated
// namespaces are T's and composite::reflect, so ADL finds the friend. descriptor<T>
// is bridged to the hook below; is_reflected_v lights up for both paths unchanged.
template <typename T>
struct fields_tag {};

template <typename T, typename = void>
struct has_adl_fields : std::false_type {};
template <typename T>
struct has_adl_fields<T, std::void_t<decltype(composite_reflect_fields(std::declval<fields_tag<T>>()))>>
    : std::true_type {};

template <typename T>
struct descriptor<T, std::enable_if_t<has_adl_fields<T>::value>> {
    static constexpr auto fields() { return composite_reflect_fields(fields_tag<T>{}); }
};

// --------------------------------------------------------------- field attributes
// Declared per-field inside COMPOSITE_FIELDS and stored in field_t::attrs. The
// `attr` builders are brought into scope by the macro body (`using namespace
// composite::reflect::attr`), so a field token can write them unqualified.
struct range_attr {
    double lo;
    double hi;
}; ///< numeric inclusive bounds
struct unit_attr {
    std::string_view text;
};
struct doc_attr {
    std::string_view text;
};
struct power_of_two_attr {};
struct runtime_attr {}; ///< field is RUNTIME-configurable
template <std::size_t N>
struct one_of_attr {
    std::array<std::string_view, N> values;
};

namespace attr {
// Independent Lo/Hi so mixed literals convert implicitly — range(0.0, 10), range(1, 65535u)
// — instead of failing template deduction. NOTE: bounds are stored as double, so an
// integer field with a bound at/above 2^53 may have an imprecise range check (rare:
// byte/sample/ns-count fields); prefer a tighter unit or a custom validate() there.
template <typename Lo, typename Hi>
constexpr auto range(Lo lo, Hi hi) -> range_attr {
    return range_attr{static_cast<double>(lo), static_cast<double>(hi)};
}
constexpr auto unit(std::string_view u) -> unit_attr {
    return unit_attr{u};
}
constexpr auto doc(std::string_view d) -> doc_attr {
    return doc_attr{d};
}
inline constexpr power_of_two_attr power_of_two{};
inline constexpr runtime_attr runtime{};
template <typename... S>
constexpr auto one_of(S... s) -> one_of_attr<sizeof...(S)> {
    return one_of_attr<sizeof...(S)>{{std::string_view{s}...}};
}
} // namespace attr

template <typename T>
struct is_one_of_attr : std::false_type {};
template <std::size_t N>
struct is_one_of_attr<one_of_attr<N>> : std::true_type {};

/// True if the attrs tuple type contains a one_of_attr<N> (any N). For compile-time guards.
template <typename Tup>
constexpr auto tuple_has_one_of() -> bool {
    bool found = false;
    std::apply([&](const auto&... a) { ((found = found || is_one_of_attr<std::decay_t<decltype(a)>>::value), ...); },
               Tup{});
    return found;
}

/// True if the attrs tuple contains an attribute of (exact) type A.
template <typename A, typename Tup>
constexpr auto has_attr(const Tup& attrs) -> bool {
    bool found = false;
    std::apply([&](const auto&... a) { ((found = found || std::is_same_v<std::decay_t<decltype(a)>, A>), ...); },
               attrs);
    return found;
}

/// Pointer to the first attribute of type A in the tuple, or nullptr.
template <typename A, typename Tup>
constexpr auto find_attr(const Tup& attrs) -> const A* {
    const A* result = nullptr;
    std::apply(
        [&](const auto&... a) {
            (
                [&] {
                    if constexpr (std::is_same_v<std::decay_t<decltype(a)>, A>) {
                        result = &a;
                    }
                }(),
                ...);
        },
        attrs);
    return result;
}

/// Invoke fn(attr) for each attribute in the tuple (caller uses if constexpr).
template <typename Tup, typename Fn>
constexpr void for_each_attr(const Tup& attrs, Fn&& fn) {
    std::apply([&](const auto&... a) { (fn(a), ...); }, attrs);
}

/// Specialized by COMPOSITE_ENUM.
template <typename E>
struct enum_traits {};
template <typename E, typename = void>
struct has_enum_traits : std::false_type {};
template <typename E>
struct has_enum_traits<E, std::void_t<decltype(enum_traits<E>::names())>> : std::true_type {};
template <typename E>
inline constexpr bool has_enum_traits_v = has_enum_traits<E>::value;

// ------------------------------------------------------------- type helpers

template <typename T>
struct is_optional : std::false_type {};
template <typename T>
struct is_optional<std::optional<T>> : std::true_type {
    using inner = T;
};
template <typename T>
struct is_vector : std::false_type {};
template <typename T>
struct is_vector<std::vector<T>> : std::true_type {
    using elem = T;
};

// ------------------------------------------------------------------ encode

template <typename M>
auto encode(const M& v) -> json {
    if constexpr (std::is_same_v<M, json>) {
        return v; // opaque leaf
    } else if constexpr (is_reflected_v<M>) {
        json j = json::object();
        std::apply([&](auto&&... f) { ((j[std::string(f.name)] = encode(v.*(f.ptr))), ...); }, descriptor<M>::fields());
        return j;
    } else if constexpr (is_optional<M>::value) {
        return v.has_value() ? encode(*v) : json(nullptr);
    } else if constexpr (is_vector<M>::value) {
        json a = json::array();
        for (const auto& e : v) {
            a.push_back(encode(e));
        }
        return a;
    } else if constexpr (std::is_enum_v<M>) {
        static_assert(has_enum_traits_v<M>, "enum property requires COMPOSITE_ENUM");
        for (const auto& [val, name] : enum_traits<M>::names()) {
            if (val == v) {
                return std::string(name);
            }
        }
        throw std::runtime_error("encode: unmapped enum value");
    } else {
        // arithmetic, bool, std::string. A class type reaching here is NOT reflected:
        // either the COMPOSITE_FIELDS/COMPOSITE_STRUCT macro is missing, or it was placed
        // where it is not visible (COMPOSITE_FIELDS_EXTERN outside T's namespace), or
        // is_reflected_v<T> was queried before T was complete. Catch it with a clear
        // message instead of an opaque nlohmann "no matching constructor" backtrace.
        static_assert(!std::is_class_v<M> || std::is_constructible_v<json, const M&>,
                      "reflect::encode: this struct is not reflected — add COMPOSITE_FIELDS"
                      "(T, fields...) inside the struct (or COMPOSITE_FIELDS_EXTERN in T's "
                      "namespace), and ensure it is visible before T is used reflectively");
        return json(v);
    }
}

// ----------------------------------------------------------- strict decode
// decode/merge are strict, path-precise, and error-accumulating: an unknown
// field at any depth, an out-of-range or fractional integer, a type mismatch,
// or a non-object where a struct is expected are each collected (with their JSON
// path) and reported together as a decode_failure — rather than silently
// truncating (the 70000 -> uint16_t 4464 class) or ignoring the field. On
// success the result is byte-identical to a plain member assignment.

/// One decode/merge problem: the dotted JSON path and a human-readable reason.
struct decode_error {
    std::string path; ///< e.g. "net.port" or "channels[2].bandwidth" ("" = the value itself)
    std::string message;
};

/// Thrown by decode()/merge() when one or more values are invalid. Carries the
/// structured list (suitable for a REST 400 body) and a multi-line what().
class decode_failure : public std::runtime_error {
public:
    std::vector<decode_error> errors;
    explicit decode_failure(std::vector<decode_error> errs)
        : std::runtime_error(build_what(errs)), errors(std::move(errs)) {}

private:
    static auto build_what(const std::vector<decode_error>& errs) -> std::string {
        std::string s = errs.size() > 1 ? "invalid values:" : "invalid value:";
        for (const auto& e : errs) {
            s += "\n  - ";
            s += e.path.empty() ? std::string("(value)") : e.path;
            s += ": ";
            s += e.message;
        }
        return s;
    }
};

namespace detail {

/// Levenshtein edit distance (cold path; only for did-you-mean hints).
inline auto edit_distance(std::string_view a, std::string_view b) -> std::size_t {
    const std::size_t n = a.size();
    const std::size_t m = b.size();
    if (n == 0) {
        return m;
    }
    if (m == 0) {
        return n;
    }
    std::vector<std::size_t> prev(m + 1);
    std::vector<std::size_t> cur(m + 1);
    for (std::size_t j = 0; j <= m; ++j) {
        prev[j] = j;
    }
    for (std::size_t i = 1; i <= n; ++i) {
        cur[0] = i;
        for (std::size_t j = 1; j <= m; ++j) {
            const std::size_t cost = (a[i - 1] == b[j - 1]) ? 0U : 1U;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        std::swap(prev, cur);
    }
    return prev[m];
}

/// " (did you mean 'x'?)" when a close field name exists, else "".
inline auto suggest(std::string_view unknown, const std::vector<std::string_view>& known) -> std::string {
    std::string_view best;
    std::size_t best_d = static_cast<std::size_t>(-1);
    for (auto k : known) {
        const std::size_t d = edit_distance(unknown, k);
        if (d < best_d) {
            best_d = d;
            best = k;
        }
    }
    const std::size_t threshold = std::max<std::size_t>(2, unknown.size() / 3);
    if (!best.empty() && best_d <= threshold) {
        return " (did you mean '" + std::string(best) + "'?)";
    }
    return "";
}

/// Accumulates errors and tracks the current dotted JSON path during recursion.
struct ctx {
    std::vector<decode_error> errors;
    std::string path;
    auto fail(std::string message) -> void { errors.push_back({path, std::move(message)}); }
};

/// RAII path segment: ".field" (object member) or "[i]" (array index), popped on exit.
struct scoped_path {
    ctx& c;
    std::size_t prev;
    scoped_path(ctx& ctx_, std::string_view seg, bool index = false) : c(ctx_), prev(ctx_.path.size()) {
        if (index) {
            c.path += '[';
            c.path += seg;
            c.path += ']';
        } else {
            if (!c.path.empty()) {
                c.path += '.';
            }
            c.path += seg;
        }
    }
    scoped_path(const scoped_path&) = delete;
    auto operator=(const scoped_path&) -> scoped_path& = delete;
    ~scoped_path() { c.path.resize(prev); }
};

template <typename M>
auto decode_impl(const json& j, M& out, ctx& c) -> void;
template <typename M>
auto merge_impl(M& obj, const json& patch, const M* dflt, ctx& c) -> void;
template <typename M>
auto merge_field_impl(M& field, const json& p, const M* dflt, ctx& c) -> void;

/// "[0, MAX]" (unsigned) or "[MIN, MAX]" (signed) for range messages.
template <typename M>
auto int_range_msg() -> std::string {
    if constexpr (std::is_unsigned_v<M>) {
        return "[0, " + std::to_string(std::numeric_limits<M>::max()) + "]";
    } else {
        return "[" + std::to_string(std::numeric_limits<M>::min()) + ", " +
               std::to_string(std::numeric_limits<M>::max()) + "]";
    }
}

/// Checked numeric decode: rejects fractional-into-integer, negative-into-unsigned,
/// and out-of-range (the silent 70000 -> uint16_t 4464 truncation bug).
template <typename M>
auto decode_number(const json& j, M& out, ctx& c) -> void {
    if (!j.is_number()) {
        c.fail("expected a number");
        return;
    }
    if constexpr (std::is_integral_v<M>) {
        if (j.is_number_float()) {
            // An integer literal that overflows int64/uint64 is parsed by nlohmann as
            // a float. If it is whole-valued, it's an out-of-range integer, not a
            // genuinely fractional value — report it as such.
            const double d = j.template get<double>();
            double intpart = 0.0;
            if (std::modf(d, &intpart) == 0.0) {
                c.fail("value out of range " + int_range_msg<M>());
            } else {
                c.fail("expected an integer, not a fractional number");
            }
            return;
        }
        using lim = std::numeric_limits<M>;
        if (j.is_number_unsigned()) {
            const auto v = j.template get<std::uint64_t>();
            if (v > static_cast<std::uint64_t>(lim::max())) {
                c.fail("value " + std::to_string(v) + " out of range " + int_range_msg<M>());
                return;
            }
            out = static_cast<M>(v);
        } else {
            const auto v = j.template get<std::int64_t>();
            if constexpr (std::is_unsigned_v<M>) {
                if (v < 0) {
                    c.fail("expected a non-negative integer, got " + std::to_string(v));
                    return;
                }
                if (static_cast<std::uint64_t>(v) > static_cast<std::uint64_t>(lim::max())) {
                    c.fail("value " + std::to_string(v) + " out of range " + int_range_msg<M>());
                    return;
                }
            } else if (v < static_cast<std::int64_t>(lim::min()) || v > static_cast<std::int64_t>(lim::max())) {
                c.fail("value " + std::to_string(v) + " out of range " + int_range_msg<M>());
                return;
            }
            out = static_cast<M>(v);
        }
    } else {
        out = static_cast<M>(j.template get<double>()); // floating point: any JSON number
    }
}

template <typename E>
auto decode_enum(const json& j, E& out, ctx& c) -> void {
    if (!j.is_string()) {
        c.fail("expected one of the allowed names (as a string)");
        return;
    }
    const auto s = j.template get<std::string>();
    std::string choices;
    for (const auto& [val, name] : enum_traits<E>::names()) {
        if (name == s) {
            out = val;
            return;
        }
        if (!choices.empty()) {
            choices += ", ";
        }
        choices += name;
    }
    c.fail("unknown value '" + s + "' (expected one of: " + choices + ")");
}

template <typename M>
auto reject_unknown_fields(const json& j, ctx& c) -> void {
    std::vector<std::string_view> known;
    std::apply([&](auto&&... f) { (known.push_back(f.name), ...); }, descriptor<M>::fields());
    for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string& key = it.key();
        bool ok = false;
        for (auto k : known) {
            if (k == key) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            scoped_path sp{c, key};
            c.fail("unknown field" + suggest(key, known));
        }
    }
}

template <typename M>
auto decode_impl(const json& j, M& out, ctx& c) -> void {
    if constexpr (std::is_same_v<M, json>) {
        out = j; // opaque leaf: accept any JSON
    } else if constexpr (is_reflected_v<M>) {
        if (!j.is_object()) {
            c.fail("expected an object");
            return;
        }
        reject_unknown_fields<M>(j, c);
        std::apply(
            [&](auto&&... f) {
                (
                    [&] {
                        const std::string key{f.name};
                        if (!j.contains(key)) {
                            return;
                        } // absent: keep current/default
                        scoped_path sp{c, f.name};
                        decode_impl(j.at(key), out.*(f.ptr), c);
                    }(),
                    ...);
            },
            descriptor<M>::fields());
    } else if constexpr (is_optional<M>::value) {
        if (j.is_null()) {
            out.reset();
        } else {
            typename is_optional<M>::inner tmp{};
            decode_impl(j, tmp, c);
            out = std::move(tmp);
        }
    } else if constexpr (is_vector<M>::value) {
        if (!j.is_array()) {
            c.fail("expected an array");
            return;
        }
        out.clear();
        std::size_t i = 0;
        for (const auto& e : j) {
            scoped_path sp{c, std::to_string(i), /*index=*/true};
            typename is_vector<M>::elem tmp{};
            decode_impl(e, tmp, c);
            out.push_back(std::move(tmp));
            ++i;
        }
    } else if constexpr (std::is_enum_v<M>) {
        static_assert(has_enum_traits_v<M>, "enum property requires COMPOSITE_ENUM");
        decode_enum(j, out, c);
    } else if constexpr (std::is_same_v<M, bool>) {
        if (!j.is_boolean()) {
            c.fail("expected a boolean (true/false)");
            return;
        }
        out = j.template get<bool>();
    } else if constexpr (std::is_arithmetic_v<M>) {
        decode_number(j, out, c);
    } else if constexpr (std::is_same_v<M, std::string>) {
        if (!j.is_string()) {
            c.fail("expected a string");
            return;
        }
        out = j.template get<std::string>();
    } else {
        out = j.template get<M>(); // last resort for any other directly-convertible type
    }
}

// @p dflt (when non-null) is the corresponding field of the registered/typed
// default; a null in the patch resets the field to it (so a nested scalar comes
// back to its member initializer, not the zero value).
template <typename M>
auto merge_field_impl(M& field, const json& p, const M* dflt, ctx& c) -> void {
    if constexpr (std::is_same_v<M, json>) {
        // Opaque leaf: wholesale replace. A deep RFC-7396 merge_patch here cannot
        // losslessly round-trip (removed keys and literal nulls are both encoded as
        // null), which would break the documented merge(a, diff(a,b)) == b invariant
        // and the change-notification payload. Replace matches a top-level json
        // property (decode path) and round-trips exactly with diff's encode(b).
        field = p;
    } else if constexpr (is_optional<M>::value) {
        if (p.is_null()) {
            field.reset();
        } // optional's "default" is nullopt
        else {
            if (!field.has_value()) {
                field.emplace();
            }
            const typename is_optional<M>::inner* idflt = (dflt != nullptr && dflt->has_value()) ? &**dflt : nullptr;
            merge_field_impl(*field, p, idflt, c);
        }
    } else if constexpr (is_reflected_v<M>) {
        if (p.is_null()) {
            field = (dflt != nullptr) ? *dflt : M{};
        } else {
            merge_impl(field, p, dflt, c);
        }
    } else {
        if (p.is_null()) {
            field = (dflt != nullptr) ? *dflt : M{};
        } // reset to registered/typed default
        else {
            decode_impl(p, field, c);
        } // scalar/enum/array: strict replace
    }
}

template <typename M>
auto merge_impl(M& obj, const json& patch, const M* dflt, ctx& c) -> void {
    if constexpr (is_reflected_v<M>) {
        if (!patch.is_object()) {
            c.fail("expected an object");
            return;
        }
        reject_unknown_fields<M>(patch, c);
        // Source nested null-resets from the registered default when provided, else a
        // default-constructed instance (which re-runs every member initializer at this
        // depth) — so a nested scalar null resets to its initializer, not to zero.
        M fallback{};
        const M& d = (dflt != nullptr) ? *dflt : fallback;
        std::apply(
            [&](auto&&... f) {
                (
                    [&] {
                        const std::string key{f.name};
                        if (!patch.contains(key)) {
                            return;
                        }
                        scoped_path sp{c, f.name};
                        merge_field_impl(obj.*(f.ptr), patch.at(key), &(d.*(f.ptr)), c);
                    }(),
                    ...);
            },
            descriptor<M>::fields());
    } else {
        decode_impl(patch, obj, c);
    }
}

} // namespace detail

// ------------------------------------------------------------------ decode
// Full strict decode (INITIALIZE-time config / scalar replace). @p root seeds
// the error path (e.g. the property name) so a message reads "net.port: ...".
template <typename M>
auto decode(const json& j, M& out, std::string_view root = {}) -> void {
    detail::ctx c;
    c.path = std::string(root);
    detail::decode_impl(j, out, c);
    if (!c.errors.empty()) {
        throw decode_failure(std::move(c.errors));
    }
}

// ------------------------------------------------------------------- equal

template <typename M>
auto equal(const M& a, const M& b) -> bool {
    if constexpr (is_reflected_v<M>) {
        bool eq = true;
        std::apply([&](auto&&... f) { ((eq = eq && equal(a.*(f.ptr), b.*(f.ptr))), ...); }, descriptor<M>::fields());
        return eq;
    } else {
        return a == b;
    }
}

// ------------------------------------------------------------------- merge
// RFC 7396, strict: a null member resets to default; a nested object merges; an
// array or scalar replaces; an opaque `json` leaf is replaced WHOLESALE (NOT
// deep-merged — so merge(a, diff(a,b)) == b round-trips losslessly even for
// removed keys / literal nulls; see merge_field_impl). An unknown field, a
// non-object where a struct is expected, or an
// out-of-range/wrong-typed value is rejected (with its path) rather than
// silently applied. (Keyed-list-by-key merge is a deliberate extension layered
// at the property layer, not here — generic arrays replace wholesale per 7396.)
// @p root seeds the error path (e.g. the property name). @p dflt (optional) is the
// registered default whose fields nested nulls reset to; if null, a default-
// constructed instance is used at each depth (member initializers).
template <typename M>
auto merge(M& obj, const json& patch, std::string_view root = {}, const M* dflt = nullptr) -> void {
    detail::ctx c;
    c.path = std::string(root);
    detail::merge_impl(obj, patch, dflt, c);
    if (!c.errors.empty()) {
        throw decode_failure(std::move(c.errors));
    }
}

// -------------------------------------------------------------------- diff
// Produce the RFC-7396 patch that turns `a` into `b` (the change payload):
// changed scalars/leaves carry the new value; changed nested structs carry a
// sub-patch. Applying merge(a_copy, diff(a, b)) yields b.

template <typename M>
auto diff_value(const M& a, const M& b) -> json;

template <typename T>
auto diff(const T& a, const T& b) -> json {
    static_assert(is_reflected_v<T>, "diff requires a reflected struct");
    json d = json::object();
    std::apply(
        [&](auto&&... f) {
            ((equal(a.*(f.ptr), b.*(f.ptr)) ? (void)0
                                            : (void)(d[std::string(f.name)] = diff_value(a.*(f.ptr), b.*(f.ptr)))),
             ...);
        },
        descriptor<T>::fields());
    return d;
}

template <typename M>
auto diff_value(const M& a, const M& b) -> json {
    if constexpr (is_reflected_v<M>) {
        return diff(a, b); // nested sub-patch
    } else {
        (void)a;
        return encode(b); // scalar/leaf/array/optional: carry the new value
    }
}

// ------------------------------------------------------------------ schema
// A JSON description of a field type for introspection / UI generation: a
// JSON-Schema-flavored view (object+fields, array+items, enum choices, integer
// range from the C++ type). Recursive, so nested structs / vectors / optionals
// are fully described. property_set::describe() builds on this; the /schema endpoint formalizes it
// into a JSON Schema 2020-12 document.

template <typename M>
auto type_schema() -> json;

template <typename M>
auto type_schema() -> json {
    json s = json::object();
    if constexpr (std::is_same_v<M, json>) {
        s["type"] = "any";
    } else if constexpr (is_reflected_v<M>) {
        s["type"] = "object";
        json fields = json::object();
        std::apply(
            [&](auto&&... f) {
                ((fields[std::string(f.name)] = type_schema<typename std::decay_t<decltype(f)>::member_type>()), ...);
            },
            descriptor<M>::fields());
        s["fields"] = std::move(fields);
    } else if constexpr (is_optional<M>::value) {
        s = type_schema<typename is_optional<M>::inner>();
        s["optional"] = true;
    } else if constexpr (is_vector<M>::value) {
        s["type"] = "array";
        s["items"] = type_schema<typename is_vector<M>::elem>();
    } else if constexpr (std::is_enum_v<M>) {
        s["type"] = "string";
        json choices = json::array();
        for (const auto& [val, name] : enum_traits<M>::names()) {
            (void)val;
            choices.push_back(std::string(name));
        }
        s["choices"] = std::move(choices);
    } else if constexpr (std::is_same_v<M, bool>) {
        s["type"] = "boolean";
    } else if constexpr (std::is_integral_v<M>) {
        s["type"] = "integer";
        s["minimum"] = std::numeric_limits<M>::min();
        s["maximum"] = std::numeric_limits<M>::max();
    } else if constexpr (std::is_floating_point_v<M>) {
        s["type"] = "number";
    } else if constexpr (std::is_same_v<M, std::string>) {
        s["type"] = "string";
    } else {
        s["type"] = "unknown";
    }
    return s;
}

} // namespace composite::reflect

// =====================================================================
// Macros
// =====================================================================
// COMPOSITE_STRUCT(T, field1, field2, ...) and COMPOSITE_ENUM(E, v1, v2, ...)
// at namespace scope (the qualified specialization works at global scope too).

#define COMPOSITE_RFL_EXPAND(x) x

#define COMPOSITE_RFL_FIELD(T, f) ::composite::reflect::field(#f, &T::f)
#define COMPOSITE_RFL_ENUMV(E, v)                                                                                      \
    ::std::pair<E, ::std::string_view> {                                                                               \
        E::v, #v                                                                                                       \
    }

// Generic FOR_EACH(MACRO, ctx, ...) -> MACRO(ctx,a), MACRO(ctx,b), ...  (up to 64 fields)
#define COMPOSITE_RFL_FE_1(M, c, a) M(c, a)
#define COMPOSITE_RFL_FE_2(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_1(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_3(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_2(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_4(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_3(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_5(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_4(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_6(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_5(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_7(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_6(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_8(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_7(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_9(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_8(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_10(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_9(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_11(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_10(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_12(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_11(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_13(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_12(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_14(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_13(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_15(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_14(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_16(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_15(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_17(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_16(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_18(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_17(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_19(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_18(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_20(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_19(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_21(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_20(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_22(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_21(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_23(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_22(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_24(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_23(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_25(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_24(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_26(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_25(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_27(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_26(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_28(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_27(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_29(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_28(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_30(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_29(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_31(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_30(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_32(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_31(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_33(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_32(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_34(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_33(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_35(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_34(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_36(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_35(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_37(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_36(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_38(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_37(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_39(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_38(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_40(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_39(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_41(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_40(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_42(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_41(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_43(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_42(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_44(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_43(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_45(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_44(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_46(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_45(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_47(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_46(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_48(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_47(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_49(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_48(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_50(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_49(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_51(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_50(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_52(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_51(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_53(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_52(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_54(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_53(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_55(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_54(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_56(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_55(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_57(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_56(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_58(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_57(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_59(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_58(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_60(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_59(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_61(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_60(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_62(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_61(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_63(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_62(M, c, __VA_ARGS__))
#define COMPOSITE_RFL_FE_64(M, c, a, ...) M(c, a), COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_FE_63(M, c, __VA_ARGS__))

#define COMPOSITE_RFL_GET(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, _17, _18, _19, _20,   \
                          _21, _22, _23, _24, _25, _26, _27, _28, _29, _30, _31, _32, _33, _34, _35, _36, _37, _38,    \
                          _39, _40, _41, _42, _43, _44, _45, _46, _47, _48, _49, _50, _51, _52, _53, _54, _55, _56,    \
                          _57, _58, _59, _60, _61, _62, _63, _64, N, ...)                                              \
    N
#define COMPOSITE_RFL_FE(M, c, ...)                                                                                    \
    COMPOSITE_RFL_EXPAND(COMPOSITE_RFL_GET(                                                                            \
        __VA_ARGS__, COMPOSITE_RFL_FE_64, COMPOSITE_RFL_FE_63, COMPOSITE_RFL_FE_62, COMPOSITE_RFL_FE_61,               \
        COMPOSITE_RFL_FE_60, COMPOSITE_RFL_FE_59, COMPOSITE_RFL_FE_58, COMPOSITE_RFL_FE_57, COMPOSITE_RFL_FE_56,       \
        COMPOSITE_RFL_FE_55, COMPOSITE_RFL_FE_54, COMPOSITE_RFL_FE_53, COMPOSITE_RFL_FE_52, COMPOSITE_RFL_FE_51,       \
        COMPOSITE_RFL_FE_50, COMPOSITE_RFL_FE_49, COMPOSITE_RFL_FE_48, COMPOSITE_RFL_FE_47, COMPOSITE_RFL_FE_46,       \
        COMPOSITE_RFL_FE_45, COMPOSITE_RFL_FE_44, COMPOSITE_RFL_FE_43, COMPOSITE_RFL_FE_42, COMPOSITE_RFL_FE_41,       \
        COMPOSITE_RFL_FE_40, COMPOSITE_RFL_FE_39, COMPOSITE_RFL_FE_38, COMPOSITE_RFL_FE_37, COMPOSITE_RFL_FE_36,       \
        COMPOSITE_RFL_FE_35, COMPOSITE_RFL_FE_34, COMPOSITE_RFL_FE_33, COMPOSITE_RFL_FE_32, COMPOSITE_RFL_FE_31,       \
        COMPOSITE_RFL_FE_30, COMPOSITE_RFL_FE_29, COMPOSITE_RFL_FE_28, COMPOSITE_RFL_FE_27, COMPOSITE_RFL_FE_26,       \
        COMPOSITE_RFL_FE_25, COMPOSITE_RFL_FE_24, COMPOSITE_RFL_FE_23, COMPOSITE_RFL_FE_22, COMPOSITE_RFL_FE_21,       \
        COMPOSITE_RFL_FE_20, COMPOSITE_RFL_FE_19, COMPOSITE_RFL_FE_18, COMPOSITE_RFL_FE_17, COMPOSITE_RFL_FE_16,       \
        COMPOSITE_RFL_FE_15, COMPOSITE_RFL_FE_14, COMPOSITE_RFL_FE_13, COMPOSITE_RFL_FE_12, COMPOSITE_RFL_FE_11,       \
        COMPOSITE_RFL_FE_10, COMPOSITE_RFL_FE_9, COMPOSITE_RFL_FE_8, COMPOSITE_RFL_FE_7, COMPOSITE_RFL_FE_6,           \
        COMPOSITE_RFL_FE_5, COMPOSITE_RFL_FE_4, COMPOSITE_RFL_FE_3, COMPOSITE_RFL_FE_2, COMPOSITE_RFL_FE_1,            \
        COMPOSITE_RFL_FE_0_UNUSED)(M, c, __VA_ARGS__))

#define COMPOSITE_STRUCT(T, ...)                                                                                       \
    template <>                                                                                                        \
    struct composite::reflect::descriptor<T, void> {                                                                   \
        static constexpr auto fields() {                                                                               \
            return ::std::make_tuple(COMPOSITE_RFL_FE(COMPOSITE_RFL_FIELD, T, __VA_ARGS__));                           \
        }                                                                                                              \
    }

#define COMPOSITE_ENUM(E, ...)                                                                                         \
    template <>                                                                                                        \
    struct composite::reflect::enum_traits<E> {                                                                        \
        static constexpr auto names() {                                                                                \
            return ::std::array{COMPOSITE_RFL_FE(COMPOSITE_RFL_ENUMV, E, __VA_ARGS__)};                                \
        }                                                                                                              \
    }

// =====================================================================
// COMPOSITE_FIELDS(T, f1, (f2, attr...), ...) — like COMPOSITE_STRUCT but works
// INSIDE a user namespace (it declares a hidden-friend ADL hook) and lets each
// field carry attributes. A field token is a bare name `gain` or a parenthesized
// `(gain, runtime, range(0.0, 10.0), unit("dB"))`. COMPOSITE_FIELDS_EXTERN is the
// free-function form for a struct you cannot edit (write it in T's namespace).
//
// Notes / limitations:
//  - Apply exactly ONE reflection macro per type. If both COMPOSITE_FIELDS and
//    COMPOSITE_STRUCT are applied to the same T, the explicit COMPOSITE_STRUCT
//    specialization silently wins and the COMPOSITE_FIELDS list is ignored.
//  - In the parenthesized form, a field whose NAME equals an attribute keyword
//    (range/unit/doc/runtime/power_of_two/one_of) collides with the in-scope attr
//    builder — name such a field with the bare form, or rename the field.
//  - is_reflected_v<T>/descriptor<T>/encode<T> must not be instantiated before T is
//    complete (querying early permanently caches "not reflected"); using T through a
//    `reflected`-constrained API (config<T>, add_config) gives a clear error if so.
// =====================================================================

// IS_PAREN(x): 1 if x is parenthesized, else 0.
#define COMPOSITE_RFL_IS_PAREN(x) COMPOSITE_RFL_IS_PAREN_(COMPOSITE_RFL_IS_PAREN_PROBE x)
#define COMPOSITE_RFL_IS_PAREN_PROBE(...) ~, 1
#define COMPOSITE_RFL_IS_PAREN_(...) COMPOSITE_RFL_IS_PAREN_SECOND(__VA_ARGS__, 0)
#define COMPOSITE_RFL_IS_PAREN_SECOND(a, b, ...) b

// head (field name) / tail (attributes) of a paren token `(name, attrs...)`
#define COMPOSITE_RFL_HEAD(a, ...) a
#define COMPOSITE_RFL_TAIL(a, ...) __VA_ARGS__
#define COMPOSITE_RFL_STR_(x) #x
#define COMPOSITE_RFL_STR(x) COMPOSITE_RFL_STR_(x) // forces x to expand, then stringizes
#define COMPOSITE_RFL_STRHEAD(...) COMPOSITE_RFL_STR(COMPOSITE_RFL_HEAD(__VA_ARGS__))

// per-field builder: bare name -> fld("n",&T::n); paren -> fld("n",&T::n).with(attrs...)
#define COMPOSITE_RFL_FLD(T, tok) COMPOSITE_RFL_FLD_SEL(T, COMPOSITE_RFL_IS_PAREN(tok), tok)
#define COMPOSITE_RFL_FLD_SEL(T, isp, tok) COMPOSITE_RFL_FLD_SEL_(T, isp, tok)
#define COMPOSITE_RFL_FLD_SEL_(T, isp, tok) COMPOSITE_RFL_FLD_##isp(T, tok)
#define COMPOSITE_RFL_FLD_0(T, name) ::composite::reflect::fld(#name, &T::name)
#define COMPOSITE_RFL_FLD_1(T, tok)                                                                                    \
    ::composite::reflect::fld(COMPOSITE_RFL_STRHEAD tok, &T::COMPOSITE_RFL_HEAD tok).with(COMPOSITE_RFL_TAIL tok)

#define COMPOSITE_FIELDS(T, ...)                                                                                       \
    friend constexpr auto composite_reflect_fields(::composite::reflect::fields_tag<T>) {                              \
        using namespace ::composite::reflect::attr;                                                                    \
        return ::std::make_tuple(COMPOSITE_RFL_FE(COMPOSITE_RFL_FLD, T, __VA_ARGS__));                                 \
    }

#define COMPOSITE_FIELDS_EXTERN(T, ...)                                                                                \
    inline constexpr auto composite_reflect_fields(::composite::reflect::fields_tag<T>) {                              \
        using namespace ::composite::reflect::attr;                                                                    \
        return ::std::make_tuple(COMPOSITE_RFL_FE(COMPOSITE_RFL_FLD, T, __VA_ARGS__));                                 \
    }                                                                                                                  \
    static_assert(::composite::reflect::is_reflected_v<T>,                                                             \
                  "COMPOSITE_FIELDS_EXTERN(T, ...) must be written inside T's namespace so the "                       \
                  "free function is visible to ADL; placed elsewhere the struct is silently "                          \
                  "unreflected")
