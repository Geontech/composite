/*
 * Copyright (C) 2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "composite/metrics/registry.hpp"

#include <cctype>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

namespace composite::metrics {

// ============================================================================
// Prometheus Text Exposition
// ============================================================================

namespace detail {

/**
 * @brief Sanitize a name for the Prometheus text format
 *
 * Anything outside [a-zA-Z0-9_:] becomes an underscore, which is what turns
 * the registry's dotted OTel-style names into Prometheus ones.
 */
inline auto sanitize_name(std::string_view in) -> std::string {
    auto out = std::string{};
    out.reserve(in.size());
    for (char c : in) {
        const auto uc = static_cast<unsigned char>(c);
        out += (std::isalnum(uc) != 0 || c == '_' || c == ':') ? c : '_';
    }
    return out;
}

/**
 * @brief Readable Prometheus suffix for a metric unit, empty if dimensionless
 */
inline auto unit_suffix(std::string_view unit) -> std::string_view {
    if (unit == "By") { return "bytes"; }
    if (unit == "s")  { return "seconds"; }
    if (unit == "ms") { return "milliseconds"; }
    if (unit == "us") { return "microseconds"; }
    if (unit == "ns") { return "nanoseconds"; }
    if (unit == "Hz") { return "hertz"; }
    return {};
}

/**
 * @brief Prometheus type keyword for a native metric type
 *
 * updown_counter maps to gauge rather than counter: it is non-monotonic, so
 * declaring it a counter would invite a meaningless rate().
 */
inline auto prometheus_type(metric_type type) -> std::string_view {
    switch (type) {
        case metric_type::counter:        return "counter";
        case metric_type::updown_counter: return "gauge";
        case metric_type::gauge:          return "gauge";
        case metric_type::histogram:      return "histogram";
    }
    return "untyped";
}

/// Escape a HELP string: backslash and newline only.
inline auto escape_help(std::string_view in) -> std::string {
    auto out = std::string{};
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

/// Escape a label value: backslash, double quote, and newline.
inline auto escape_label(std::string_view in) -> std::string {
    auto out = std::string{};
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

/**
 * @brief Render a label set, optionally with one extra pre-rendered pair
 *
 * @param extra Already-formatted pair such as `le="0.05"`, appended last.
 */
inline auto render_labels(const labels_t& labels, std::string_view extra = {}) -> std::string {
    if (labels.empty() && extra.empty()) {
        return {};
    }
    auto out = std::string{"{"};
    auto first = true;
    for (const auto& [key, value] : labels) {
        if (!first) {
            out += ',';
        }
        first = false;
        out += sanitize_name(key);
        out += "=\"";
        out += escape_label(value);
        out += '"';
    }
    if (!extra.empty()) {
        if (!first) {
            out += ',';
        }
        out += extra;
    }
    out += '}';
    return out;
}

/**
 * @brief True if `name` already ends with the unit's own abbreviation
 *
 * Catches names that spell the unit short-form, e.g. "cell_cfo_hz" with unit
 * "Hz" or "maint_us" with unit "us", which the long-form check misses.
 */
inline auto ends_with_unit_token(std::string_view name, std::string_view unit) -> bool {
    if (unit.empty()) {
        return false;
    }
    auto lowered = std::string{"_"};
    for (char c : unit) {
        lowered += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return name.size() > lowered.size() && name.ends_with(lowered);
}

} // namespace detail

/**
 * @brief Map a metric snapshot onto its Prometheus family name
 *
 * The unit is appended as a readable suffix ("By" -> "_bytes", "s" ->
 * "_seconds") unless the name already carries it — composite's names usually
 * do, e.g. "composite.port.bytes_transferred" — so we do not end up with
 * "..._bytes_transferred_bytes". Counters then take the conventional "_total".
 */
inline auto prometheus_name(const metric_snapshot& snap) -> std::string {
    auto name = detail::sanitize_name(snap.name);

    // Skip the suffix when the name already states the unit, in either the long
    // form ("bytes_transferred" + By) or the abbreviation ("cell_cfo_hz" + Hz).
    // Checking only the long form produced "lettuce_cell_cfo_hz_hertz".
    const auto suffix = detail::unit_suffix(snap.unit);
    if (!suffix.empty()
        && name.find(suffix) == std::string::npos
        && !detail::ends_with_unit_token(name, snap.unit)) {
        name += '_';
        name += suffix;
    }

    if (snap.type == metric_type::counter && !name.ends_with("_total")) {
        name += "_total";
    }
    return name;
}

/**
 * @brief Render registry snapshots as Prometheus text exposition (v0.0.4)
 *
 * One HELP/TYPE pair per family, then one line per label set. Metrics sharing a
 * name but differing in labels — which is every port_stats and stage_stats
 * metric — must be grouped, so snapshots are bucketed by exposition name
 * first. Insertion order is preserved so successive scrapes are stable.
 */
inline auto to_prometheus(const std::vector<metric_snapshot>& snapshots) -> std::string {
    auto order = std::vector<std::string>{};
    auto families = std::unordered_map<std::string, std::vector<const metric_snapshot*>>{};
    for (const auto& snap : snapshots) {
        auto name = prometheus_name(snap);
        auto [it, inserted] = families.try_emplace(name);
        if (inserted) {
            order.push_back(name);
        }
        it->second.push_back(&snap);
    }

    auto out = std::string{};
    out.reserve(snapshots.size() * 128);

    for (const auto& name : order) {
        const auto& family = families[name];
        const auto& first = *family.front();

        if (!first.description.empty()) {
            out += std::format("# HELP {} {}\n", name, detail::escape_help(first.description));
        }
        out += std::format("# TYPE {} {}\n", name, detail::prometheus_type(first.type));

        for (const auto* snap : family) {
            const auto labels = detail::render_labels(snap->labels);
            std::visit([&out, &name, &labels, snap](auto&& val) {
                using T = std::decay_t<decltype(val)>;
                if constexpr (std::is_same_v<T, histogram_snapshot>) {
                    // Prometheus buckets are cumulative and upper-bounded by
                    // `le`; the native snapshot holds per-bucket counts with a
                    // trailing overflow bucket, so accumulate as we go and let
                    // the +Inf bucket carry the total.
                    auto cumulative = uint64_t{0};
                    for (std::size_t i = 0; i < val.boundaries.size(); ++i) {
                        if (i < val.bucket_counts.size()) {
                            cumulative += val.bucket_counts[i];
                        }
                        out += std::format(
                            "{}_bucket{} {}\n",
                            name,
                            detail::render_labels(
                                snap->labels,
                                // {:.10g} rather than {} so a boundary like
                                // 0.0001 renders as "0.0001" and not "1e-04":
                                // `le` is matched as a string, and Prometheus
                                // convention (Go's %g) is the non-exponent
                                // form in this range.
                                std::format("le=\"{:.10g}\"", val.boundaries[i])),
                            cumulative);
                    }
                    out += std::format(
                        "{}_bucket{} {}\n",
                        name,
                        detail::render_labels(snap->labels, "le=\"+Inf\""),
                        val.count);
                    out += std::format("{}_sum{} {}\n", name, labels, val.sum);
                    out += std::format("{}_count{} {}\n", name, labels, val.count);
                } else {
                    out += std::format("{}{} {}\n", name, labels, val);
                }
            }, snap->value);
        }
    }

    return out;
}

} // namespace composite::metrics
