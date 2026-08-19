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
 * along with this program.  If not, see http://www.gnu.org/licenses/.
 */

#pragma once

#ifdef COMPOSITE_USE_OPENTELEMETRY

#include "composite/metrics/registry.hpp"
#include "composite/util/export.hpp"
#include "config.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace composite::telemetry {

/**
 * @brief OpenTelemetry metrics exporter bridge
 *
 * This manager bridges the native composite::metrics::registry to OpenTelemetry
 * collectors via OTLP/HTTP. It runs a background thread that periodically reads
 * metrics from the native registry and exports them.
 *
 * Key design principles:
 * - Native metrics (composite::metrics::*) are always collected regardless of OTel
 * - This manager is optional - only needed when exporting to OTel collectors
 * - Export runs on a background thread, never blocks component threads
 * - Reads from metrics::registry using snapshot_all() (shared lock, no contention)
 *
 * Usage:
 * 1. Configure telemetry in JSON or via environment variables
 * 2. Call initialize() during application startup
 * 3. Native metrics are automatically exported at configured interval
 * 4. Call shutdown() during application teardown
 *
 * @note The manager uses COMPOSITE_API to ensure the singleton instance is shared
 *       across all dynamically loaded component libraries.
 */
class manager {
public:
    /**
     * @brief Get the singleton instance
     */
    COMPOSITE_API
    static auto instance() -> manager&;

    /**
     * @brief Initialize OpenTelemetry OTLP exporter
     *
     * Starts a background thread that periodically exports metrics from
     * the native metrics::registry to the configured OTLP endpoint.
     *
     * Configuration takes precedence over environment variables (OTEL_*).
     * ENV vars only apply as defaults when config uses default values.
     *
     * @param cfg Telemetry configuration
     * @return true if initialization succeeded, false otherwise
     */
    COMPOSITE_API
    auto initialize(const telemetry::config& cfg) -> bool;

    /**
     * @brief Shutdown OpenTelemetry exporter
     *
     * Stops the export thread and flushes any pending metrics.
     */
    COMPOSITE_API
    auto shutdown() -> void;

    /**
     * @brief Check if telemetry export is active
     *
     * @return true if initialized and exporting
     */
    COMPOSITE_API
    auto is_initialized() const -> bool;

    /**
     * @brief SUPPORTED diagnostics: (instrument count, total exported series).
     *
     * Deliberately part of the public v0.5 surface, not a test hook. Export cardinality is the
     * main operational hazard of an OTLP bridge — an unbounded label set costs money at the
     * collector long before it costs anything here — and this is the only way to see it from
     * inside the process.
     *
     * It also pins the invariant one INSTRUMENT carries many SERIES, whose violation is otherwise
     * invisible locally: if N components publishing the same metric name collapse into a single
     * series, this side still looks healthy and you only discover the loss at the collector, where
     * N-1 components have silently gone missing.
     *
     * Kept unconditional rather than gated behind COMPOSITE_TESTING because that macro is scoped
     * to the test directory: gating the declaration but not the definition breaks the build, and
     * gating both makes the library's exported symbols depend on whether tests were enabled.
     * A public API that appears and disappears with a build flag is worse than a small one.
     */
    COMPOSITE_API
    auto exported_series_counts() const -> std::pair<std::size_t, std::size_t>;

    // Disable copy/move (singleton)
    manager(const manager&) = delete;
    manager(manager&&) = delete;
    auto operator=(const manager&) -> manager& = delete;
    auto operator=(manager&&) -> manager& = delete;

private:
    manager();
    ~manager();

    /**
     * @brief Create an OTel instrument for a native metric
     *
     * Called by the registration observer when metrics are created.
     *
     * @param meta Metric metadata
     * @param metric_ptr Pointer to the native metric
     */
    auto create_otel_instrument(const metrics::metric_metadata& meta, void* metric_ptr) -> void;

    /**
     * @brief Remove OTel instruments for a deregistered metric
     *
     * Called by the deregistration observer when native metrics are removed.
     * Prevents use-after-free by cleaning up instruments before the native
     * metric is destroyed.
     *
     * Matches on the POINTER, not on name+labels. A name is reusable — removing "x" and creating
     * "x" again produces a different metric — so name matching would cancel a live replacement
     * when the original's retraction arrives. Pointer identity also sweeps all three of a
     * histogram's roles, which share one native metric, and makes a refused type/unit collision a
     * non-event (a refused metric is a different object).
     *
     * @param meta       Metadata of the metric being removed (used for diagnostics)
     * @param metric_ptr The metric being removed — the identity every series is matched against
     */
    auto remove_otel_instrument(const metrics::metric_metadata& meta, void* metric_ptr) -> void;

    /// Find-or-create the instrument that carries @p otel_name, returning it as an opaque
    /// handle (void* so this header need not see the OTel types). One instrument per NAME:
    /// the SDK keys them that way, so a second instrument with the same name would supersede
    /// the first rather than adding a series. Returns nullptr if creation failed.
    auto group_for(const std::string& otel_name, const std::string& base_name, metrics::metric_type type, int role_raw,
                   const std::string& description, const std::string& unit) -> void*;

    /// Drop instruments that carry no series. Call with instrument_mutex held.
    auto prune_empty_groups() -> void;

    /// Append one exported series to a group obtained from group_for().
    auto add_series(void* group_ptr, void* metric, const metrics::labels_t& raw_labels,
                    const std::vector<std::pair<std::string, std::string>>& attributes, std::size_t bucket_idx) -> void;

    struct impl;
    std::unique_ptr<impl> m_impl;
};

} // namespace composite::telemetry

#endif // COMPOSITE_USE_OPENTELEMETRY
