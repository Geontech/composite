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

#include "types.hpp"

#include "composite/util/export.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace composite::metrics {

/**
 * @brief Snapshot of histogram data for reporting
 */
struct histogram_snapshot {
    std::vector<double> boundaries;
    std::vector<uint64_t> bucket_counts;
    uint64_t count;
    double sum;
};

/**
 * @brief Snapshot of a single metric for reporting
 */
struct metric_snapshot {
    std::string name;
    std::string description;
    std::string unit;
    metric_type type;
    labels_t labels;
    std::variant<uint64_t, int64_t, double, histogram_snapshot> value;
    std::chrono::system_clock::time_point timestamp;
};

/**
 * @brief Metadata for a registered metric
 */
struct metric_metadata {
    std::string name;
    std::string description;
    std::string unit;
    metric_type type;
    labels_t labels;
};

/**
 * @brief Callback signature for metric registration observers
 *
 * Observers are notified when new metrics are created. This enables
 * external systems (like OTel bridge) to create corresponding instruments.
 *
 * @param metadata Metadata of the newly registered metric
 * @param metric_ptr Opaque pointer to the metric (cast based on type)
 */
using registration_callback = std::function<void(const metric_metadata&, void*)>;

/**
 * @brief Callback signature for observer error handling
 *
 * Called when an observer callback throws an exception during metric registration.
 *
 * @param observer_id ID of the observer that threw
 * @param metric_name Name of the metric being registered
 * @param error Exception that was thrown (as exception_ptr)
 */
using error_callback =
    std::function<void(std::size_t observer_id, const std::string& metric_name, std::exception_ptr error)>;

/**
 * @brief Callback signature for metric deregistration observers
 *
 * Observers are notified when metrics are removed. This enables
 * external systems (like OTel bridge) to clean up corresponding instruments.
 *
 * @param metadata Metadata of the metric being removed
 */
using deregistration_callback = std::function<void(const metric_metadata&)>;

/**
 * @brief Exception thrown when a duplicate metric is detected
 */
class duplicate_metric_error : public std::runtime_error {
public:
    explicit duplicate_metric_error(const std::string& name) : std::runtime_error("Metric already exists: " + name) {}
};

/**
 * @brief Exception thrown when a metric name is invalid
 */
class invalid_metric_name_error : public std::runtime_error {
public:
    explicit invalid_metric_name_error(const std::string& name, const std::string& reason)
        : std::runtime_error("Invalid metric name '" + name + "': " + reason) {}
};

/**
 * @brief Exception thrown when a label key or value is invalid
 */
class invalid_label_error : public std::runtime_error {
public:
    explicit invalid_label_error(const std::string& key, const std::string& reason)
        : std::runtime_error("Invalid label key '" + key + "': " + reason) {}
};

/**
 * @brief Exception thrown when metric limit is exceeded
 */
class metric_limit_exceeded_error : public std::runtime_error {
public:
    explicit metric_limit_exceeded_error(std::size_t limit)
        : std::runtime_error("Metric limit exceeded: maximum " + std::to_string(limit) + " metrics allowed") {}
};

/**
 * @brief Sanitize a string for use in a metric name
 *
 * Converts an arbitrary string into a valid metric name segment by:
 * - Replacing invalid characters (dashes, spaces, etc.) with underscores
 * - Prefixing with 'c' if the string starts with a digit
 * - Returning "unnamed" for empty strings
 *
 * This is useful for converting component IDs or other identifiers that
 * may contain characters invalid in metric names.
 *
 * @param id String to sanitize
 * @return Sanitized string safe for use in metric names
 */
inline auto sanitize_for_metric_name(std::string_view id) -> std::string {
    auto fnv1a_64 = [](std::string_view text) -> uint64_t {
        constexpr uint64_t offset_basis = 14695981039346656037ULL;
        constexpr uint64_t prime = 1099511628211ULL;
        uint64_t hash = offset_basis;
        for (unsigned char c : text) {
            hash ^= c;
            hash *= prime;
        }
        return hash;
    };

    if (id.empty()) {
        return "unnamed";
    }

    std::string result;
    result.reserve(id.size() + 1);

    // Prefix with 'c' if starts with a digit
    if (std::isdigit(static_cast<unsigned char>(id[0]))) {
        result += 'c';
    }

    for (char c : id) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            result += c;
        } else if (c == '.' || c == '_') {
            // Keep dots and underscores, but avoid consecutive ones
            if (!result.empty() && result.back() != '.' && result.back() != '_') {
                result += c;
            }
        } else {
            // Replace other chars (dash, space, etc.) with underscore
            if (!result.empty() && result.back() != '_' && result.back() != '.') {
                result += '_';
            }
        }
    }

    // Remove trailing dot or underscore
    while (!result.empty() && (result.back() == '.' || result.back() == '_')) {
        result.pop_back();
    }

    if (result.empty()) {
        result = "unnamed";
    }

    if (result != id) {
        result += "_";
        result += std::format("{:016x}", fnv1a_64(id));
    }

    return result;
}

/**
 * @brief Validate a metric name follows OpenTelemetry naming conventions
 *
 * Valid names:
 * - Must be non-empty
 * - Must start with a letter
 * - May contain lowercase letters, digits, underscores, and dots
 * - Must not end with a dot
 * - Must not have consecutive dots
 *
 * @param name Metric name to validate
 * @throws invalid_metric_name_error if the name is invalid
 */
inline auto validate_metric_name(std::string_view name) -> void {
    if (name.empty()) {
        throw invalid_metric_name_error(std::string{name}, "name cannot be empty");
    }

    // Must start with a letter
    if (!std::isalpha(static_cast<unsigned char>(name[0]))) {
        throw invalid_metric_name_error(std::string{name}, "must start with a letter");
    }

    // Must not end with a dot
    if (name.back() == '.') {
        throw invalid_metric_name_error(std::string{name}, "must not end with a dot");
    }

    // Check all characters and for consecutive dots
    bool prev_was_dot = false;
    for (std::size_t i = 0; i < name.size(); ++i) {
        char c = name[i];
        bool is_valid = std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.';

        if (!is_valid) {
            throw invalid_metric_name_error(std::string{name}, "contains invalid character '" + std::string(1, c) +
                                                                   "' at position " + std::to_string(i) +
                                                                   " (allowed: a-z, A-Z, 0-9, _, .)");
        }

        if (c == '.') {
            if (prev_was_dot) {
                throw invalid_metric_name_error(std::string{name},
                                                "contains consecutive dots at position " + std::to_string(i));
            }
            prev_was_dot = true;
        } else {
            prev_was_dot = false;
        }
    }
}

/**
 * @brief Validate label keys and values follow OpenTelemetry conventions
 *
 * Valid label keys:
 * - Must be non-empty
 * - Must start with a letter
 * - May contain letters, digits, and underscores
 *
 * Valid label values:
 * - May be empty
 * - No control characters
 *
 * @param labels Labels to validate
 * @throws invalid_label_error if any label is invalid
 */
inline auto validate_labels(const labels_t& labels) -> void {
    for (const auto& [key, value] : labels) {
        // Key validation
        if (key.empty()) {
            throw invalid_label_error("", "label key cannot be empty");
        }

        if (!std::isalpha(static_cast<unsigned char>(key[0]))) {
            throw invalid_label_error(key, "must start with a letter");
        }

        for (std::size_t i = 0; i < key.size(); ++i) {
            char c = key[i];
            bool is_valid = std::isalnum(static_cast<unsigned char>(c)) || c == '_';
            if (!is_valid) {
                throw invalid_label_error(key, "contains invalid character '" + std::string(1, c) + "' at position " +
                                                   std::to_string(i) + " (allowed: a-z, A-Z, 0-9, _)");
            }
        }

        // Value validation - just check for control characters
        for (std::size_t i = 0; i < value.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(value[i]);
            if (std::iscntrl(c) && c != '\t') {
                throw invalid_label_error(key, "value contains control character at position " + std::to_string(i));
            }
        }
    }
}

/**
 * @brief Normalize labels by sorting them by key
 *
 * This ensures that labels in different orders are treated as equivalent.
 * For example, {{"b","2"},{"a","1"}} becomes {{"a","1"},{"b","2"}}.
 *
 * @param labels Labels to normalize (modified in place)
 */
inline auto normalize_labels(labels_t& labels) -> void {
    std::sort(labels.begin(), labels.end(), [](const label_pair& a, const label_pair& b) { return a.first < b.first; });
}

namespace detail {

/// An observer plus the state remove_observer() needs to retire it safely. Held by shared_ptr so
/// a notifier's copy keeps the slot alive even as the map entry is erased. At namespace scope
/// because the notify signatures below name it in PARAMETER types, which are not a
/// complete-class context.
template <typename Callback>
struct observer_slot {
    explicit observer_slot(Callback cb) : callback(std::move(cb)) {}
    Callback callback;
    std::atomic<bool> closed{false}; ///< set by removal; checked after registering in-flight
    std::atomic<int> in_flight{0};   ///< callbacks currently executing
};

} // namespace detail

using registration_observer_map =
    std::unordered_map<std::size_t, std::shared_ptr<detail::observer_slot<registration_callback>>>;
using deregistration_observer_map =
    std::unordered_map<std::size_t, std::shared_ptr<detail::observer_slot<deregistration_callback>>>;

/**
 * @brief Central registry for all application metrics
 *
 * The registry is a singleton that owns all metric instances. It provides:
 * - Factory methods for creating metrics
 * - Query interface for REST/SSE consumers
 * - Thread-safe access for concurrent writers and readers
 *
 * Design principles:
 * - Metric creation is NOT hot-path (happens at component construction)
 * - Metric recording is hot-path (just atomic ops, no registry interaction)
 * - Metric reading is NOT hot-path (happens periodically for export)
 *
 * Usage:
 * @code
 * // At component construction (not hot path)
 * auto& packets = metrics::registry::instance()
 *     .create_counter("packets_sent", "Total packets sent", "1",
 *                     {{"component", my_id}});
 *
 * // In process() loop (hot path - just atomic increment)
 * packets.inc();
 *
 * // For export/monitoring (not hot path)
 * auto snapshots = metrics::registry::instance().snapshot_all();
 * @endcode
 */
class registry {
public:
    /// Default maximum number of metrics (0 = unlimited)
    static constexpr std::size_t DEFAULT_MAX_METRICS = 10000;

    /**
     * @brief Get the singleton registry instance
     *
     * This is defined in registry.cpp to ensure a single instance across
     * all dynamically loaded component libraries (DSO boundary safe).
     */
    COMPOSITE_API
    static auto instance() -> registry&;

    // ========================================================================
    // Configuration
    // ========================================================================

    /**
     * @brief Set the maximum number of metrics allowed
     *
     * Set to 0 for unlimited metrics. Default is 10000.
     *
     * @param max Maximum number of metrics (0 = unlimited)
     */
    auto set_max_metrics(std::size_t max) -> void {
        auto lock = std::unique_lock{m_mutex};
        m_max_metrics = max;
    }

    /**
     * @brief Get the current maximum metrics limit
     */
    [[nodiscard]]
    auto max_metrics() const -> std::size_t {
        return m_max_metrics;
    }

    /**
     * @brief Set the error handler for observer failures
     *
     * When an observer callback throws an exception, this handler is called
     * instead of silently ignoring the error. Useful for logging or debugging.
     *
     * @param handler Callback to invoke on observer errors (nullptr to disable)
     */
    auto set_error_handler(error_callback handler) -> void {
        auto lock = std::unique_lock{m_mutex};
        m_error_handler = std::move(handler);
    }

    // ========================================================================
    // Factory Methods - called at initialization
    // ========================================================================

    /**
     * @brief Create a monotonically increasing counter
     *
     * @param name Metric name (should be namespaced, e.g., "mycomponent.packets_sent")
     * @param description Human-readable description
     * @param unit Unit of measurement (e.g., "1", "bytes", "ms")
     * @param labels Dimensional labels for this metric instance
     * @return Reference to the created counter
     * @throws duplicate_metric_error if a counter with same name+labels exists
     */
    auto create_counter(std::string name, std::string description = "", std::string unit = "1", labels_t labels = {})
        -> counter<uint64_t>& {
        validate_metric_name(name);
        validate_labels(labels);
        normalize_labels(labels);

        counter<uint64_t>* result_ptr = nullptr;
        metric_metadata meta_copy;
        decltype(m_observers) observers_copy;
        error_callback error_handler_copy;

        {
            auto lock = std::unique_lock{m_mutex};
            if (auto* existing = find_counter(name, labels); existing != nullptr) {
                throw duplicate_metric_error(name);
            }
            auto& ref =
                create_counter_impl(std::move(name), std::move(description), std::move(unit), std::move(labels));
            result_ptr = &ref;
            meta_copy = m_counter_metadata.back();
            observers_copy = m_observers;
            error_handler_copy = m_error_handler;
        }

        // Notify observers outside lock to prevent slow observers from blocking
        notify_registration_unlocked(meta_copy, result_ptr, observers_copy, error_handler_copy);
        return *result_ptr;
    }

    /**
     * @brief Get existing counter or create new one (idempotent)
     *
     * Unlike create_counter(), this is idempotent - calling multiple times
     * with the same name+labels returns the same counter.
     */
    auto get_or_create_counter(std::string name, std::string description = "", std::string unit = "1",
                               labels_t labels = {}) -> counter<uint64_t>& {
        validate_metric_name(name);
        validate_labels(labels);
        normalize_labels(labels);

        counter<uint64_t>* result_ptr = nullptr;
        bool created = false;
        metric_metadata meta_copy;
        decltype(m_observers) observers_copy;
        error_callback error_handler_copy;

        {
            auto lock = std::unique_lock{m_mutex};
            if (auto* existing = find_counter(name, labels); existing != nullptr) {
                return *existing;
            }
            auto& ref =
                create_counter_impl(std::move(name), std::move(description), std::move(unit), std::move(labels));
            result_ptr = &ref;
            created = true;
            meta_copy = m_counter_metadata.back();
            observers_copy = m_observers;
            error_handler_copy = m_error_handler;
        }

        // Notify observers outside lock to prevent slow observers from blocking
        if (created) {
            notify_registration_unlocked(meta_copy, result_ptr, observers_copy, error_handler_copy);
        }
        return *result_ptr;
    }

    /**
     * @brief Create a non-monotonic up/down counter
     *
     * @param name Metric name
     * @param description Human-readable description
     * @param unit Unit of measurement
     * @param labels Dimensional labels
     * @return Reference to the created counter
     * @throws duplicate_metric_error if an updown_counter with same name+labels exists
     */
    auto create_updown_counter(std::string name, std::string description = "", std::string unit = "1",
                               labels_t labels = {}) -> updown_counter<int64_t>& {
        validate_metric_name(name);
        validate_labels(labels);
        normalize_labels(labels);

        updown_counter<int64_t>* result_ptr = nullptr;
        metric_metadata meta_copy;
        decltype(m_observers) observers_copy;
        error_callback error_handler_copy;

        {
            auto lock = std::unique_lock{m_mutex};
            if (auto* existing = find_updown_counter(name, labels); existing != nullptr) {
                throw duplicate_metric_error(name);
            }
            auto& ref =
                create_updown_counter_impl(std::move(name), std::move(description), std::move(unit), std::move(labels));
            result_ptr = &ref;
            meta_copy = m_updown_counter_metadata.back();
            observers_copy = m_observers;
            error_handler_copy = m_error_handler;
        }

        // Notify observers outside lock to prevent slow observers from blocking
        notify_registration_unlocked(meta_copy, result_ptr, observers_copy, error_handler_copy);
        return *result_ptr;
    }

    /**
     * @brief Get existing updown_counter or create new one (idempotent)
     */
    auto get_or_create_updown_counter(std::string name, std::string description = "", std::string unit = "1",
                                      labels_t labels = {}) -> updown_counter<int64_t>& {
        validate_metric_name(name);
        validate_labels(labels);
        normalize_labels(labels);

        updown_counter<int64_t>* result_ptr = nullptr;
        bool created = false;
        metric_metadata meta_copy;
        decltype(m_observers) observers_copy;
        error_callback error_handler_copy;

        {
            auto lock = std::unique_lock{m_mutex};
            if (auto* existing = find_updown_counter(name, labels); existing != nullptr) {
                return *existing;
            }
            auto& ref =
                create_updown_counter_impl(std::move(name), std::move(description), std::move(unit), std::move(labels));
            result_ptr = &ref;
            created = true;
            meta_copy = m_updown_counter_metadata.back();
            observers_copy = m_observers;
            error_handler_copy = m_error_handler;
        }

        // Notify observers outside lock to prevent slow observers from blocking
        if (created) {
            notify_registration_unlocked(meta_copy, result_ptr, observers_copy, error_handler_copy);
        }
        return *result_ptr;
    }

    /**
     * @brief Create a gauge for point-in-time values
     *
     * @param name Metric name
     * @param description Human-readable description
     * @param unit Unit of measurement
     * @param labels Dimensional labels
     * @return Reference to the created gauge
     * @throws duplicate_metric_error if a gauge with same name+labels exists
     */
    auto create_gauge(std::string name, std::string description = "", std::string unit = "1", labels_t labels = {})
        -> gauge<double>& {
        validate_metric_name(name);
        validate_labels(labels);
        normalize_labels(labels);

        gauge<double>* result_ptr = nullptr;
        metric_metadata meta_copy;
        decltype(m_observers) observers_copy;
        error_callback error_handler_copy;

        {
            auto lock = std::unique_lock{m_mutex};
            if (auto* existing = find_gauge(name, labels); existing != nullptr) {
                throw duplicate_metric_error(name);
            }
            auto& ref = create_gauge_impl(std::move(name), std::move(description), std::move(unit), std::move(labels));
            result_ptr = &ref;
            meta_copy = m_gauge_metadata.back();
            observers_copy = m_observers;
            error_handler_copy = m_error_handler;
        }

        // Notify observers outside lock to prevent slow observers from blocking
        notify_registration_unlocked(meta_copy, result_ptr, observers_copy, error_handler_copy);
        return *result_ptr;
    }

    /**
     * @brief Get existing gauge or create new one (idempotent)
     */
    auto get_or_create_gauge(std::string name, std::string description = "", std::string unit = "1",
                             labels_t labels = {}) -> gauge<double>& {
        validate_metric_name(name);
        validate_labels(labels);
        normalize_labels(labels);

        gauge<double>* result_ptr = nullptr;
        bool created = false;
        metric_metadata meta_copy;
        decltype(m_observers) observers_copy;
        error_callback error_handler_copy;

        {
            auto lock = std::unique_lock{m_mutex};
            if (auto* existing = find_gauge(name, labels); existing != nullptr) {
                return *existing;
            }
            auto& ref = create_gauge_impl(std::move(name), std::move(description), std::move(unit), std::move(labels));
            result_ptr = &ref;
            created = true;
            meta_copy = m_gauge_metadata.back();
            observers_copy = m_observers;
            error_handler_copy = m_error_handler;
        }

        // Notify observers outside lock to prevent slow observers from blocking
        if (created) {
            notify_registration_unlocked(meta_copy, result_ptr, observers_copy, error_handler_copy);
        }
        return *result_ptr;
    }

    /**
     * @brief Create a histogram for value distributions
     *
     * @param name Metric name
     * @param description Human-readable description
     * @param unit Unit of measurement
     * @param boundaries Bucket boundaries (or use histogram::power_of_2())
     * @param labels Dimensional labels
     * @return Reference to the created histogram
     * @throws duplicate_metric_error if a histogram with same name+labels exists
     */
    auto create_histogram(std::string name, std::string description, std::string unit, std::vector<double> boundaries,
                          labels_t labels = {}) -> histogram& {
        validate_metric_name(name);
        validate_labels(labels);
        normalize_labels(labels);

        histogram* result_ptr = nullptr;
        metric_metadata meta_copy;
        decltype(m_observers) observers_copy;
        error_callback error_handler_copy;

        {
            auto lock = std::unique_lock{m_mutex};
            if (auto* existing = find_histogram(name, labels); existing != nullptr) {
                throw duplicate_metric_error(name);
            }
            auto& ref = create_histogram_impl(std::move(name), std::move(description), std::move(unit),
                                              std::move(boundaries), std::move(labels));
            result_ptr = &ref;
            meta_copy = m_histogram_metadata.back();
            observers_copy = m_observers;
            error_handler_copy = m_error_handler;
        }

        // Notify observers outside lock to prevent slow observers from blocking
        notify_registration_unlocked(meta_copy, result_ptr, observers_copy, error_handler_copy);
        return *result_ptr;
    }

    /**
     * @brief Get existing histogram or create new one (idempotent)
     */
    auto get_or_create_histogram(std::string name, std::string description, std::string unit,
                                 std::vector<double> boundaries, labels_t labels = {}) -> histogram& {
        validate_metric_name(name);
        validate_labels(labels);
        normalize_labels(labels);

        histogram* result_ptr = nullptr;
        bool created = false;
        metric_metadata meta_copy;
        decltype(m_observers) observers_copy;
        error_callback error_handler_copy;

        {
            auto lock = std::unique_lock{m_mutex};
            if (auto* existing = find_histogram(name, labels); existing != nullptr) {
                return *existing;
            }
            auto& ref = create_histogram_impl(std::move(name), std::move(description), std::move(unit),
                                              std::move(boundaries), std::move(labels));
            result_ptr = &ref;
            created = true;
            meta_copy = m_histogram_metadata.back();
            observers_copy = m_observers;
            error_handler_copy = m_error_handler;
        }

        // Notify observers outside lock to prevent slow observers from blocking
        if (created) {
            notify_registration_unlocked(meta_copy, result_ptr, observers_copy, error_handler_copy);
        }
        return *result_ptr;
    }

    /**
     * @brief Create a histogram with power-of-2 boundaries for O(1) lookup
     *
     * @param name Metric name
     * @param description Human-readable description
     * @param unit Unit of measurement
     * @param num_buckets Number of buckets (default 20, covering 0 to ~500K)
     * @param labels Dimensional labels
     * @return Reference to the created histogram
     * @throws duplicate_metric_error if a histogram with same name+labels exists
     */
    auto create_histogram_pow2(std::string name, std::string description = "", std::string unit = "1",
                               std::size_t num_buckets = 20, labels_t labels = {}) -> histogram& {
        validate_metric_name(name);
        validate_labels(labels);
        normalize_labels(labels);

        histogram* result_ptr = nullptr;
        metric_metadata meta_copy;
        decltype(m_observers) observers_copy;
        error_callback error_handler_copy;

        {
            auto lock = std::unique_lock{m_mutex};
            if (auto* existing = find_histogram(name, labels); existing != nullptr) {
                throw duplicate_metric_error(name);
            }
            auto& ref = create_histogram_pow2_impl(std::move(name), std::move(description), std::move(unit),
                                                   num_buckets, std::move(labels));
            result_ptr = &ref;
            meta_copy = m_histogram_metadata.back();
            observers_copy = m_observers;
            error_handler_copy = m_error_handler;
        }

        // Notify observers outside lock to prevent slow observers from blocking
        notify_registration_unlocked(meta_copy, result_ptr, observers_copy, error_handler_copy);
        return *result_ptr;
    }

    /**
     * @brief Get existing histogram or create new one with power-of-2 boundaries (idempotent)
     */
    auto get_or_create_histogram_pow2(std::string name, std::string description = "", std::string unit = "1",
                                      std::size_t num_buckets = 20, labels_t labels = {}) -> histogram& {
        validate_metric_name(name);
        validate_labels(labels);
        normalize_labels(labels);

        histogram* result_ptr = nullptr;
        bool created = false;
        metric_metadata meta_copy;
        decltype(m_observers) observers_copy;
        error_callback error_handler_copy;

        {
            auto lock = std::unique_lock{m_mutex};
            if (auto* existing = find_histogram(name, labels); existing != nullptr) {
                return *existing;
            }
            auto& ref = create_histogram_pow2_impl(std::move(name), std::move(description), std::move(unit),
                                                   num_buckets, std::move(labels));
            result_ptr = &ref;
            created = true;
            meta_copy = m_histogram_metadata.back();
            observers_copy = m_observers;
            error_handler_copy = m_error_handler;
        }

        // Notify observers outside lock to prevent slow observers from blocking
        if (created) {
            notify_registration_unlocked(meta_copy, result_ptr, observers_copy, error_handler_copy);
        }
        return *result_ptr;
    }

    // ========================================================================
    // Metric Removal - for dynamic component cleanup
    // ========================================================================

    /**
     * @brief Remove a counter by name and labels
     *
     * @param name Metric name
     * @param labels Labels (order-insensitive)
     * @return true if the metric was found and removed
     */
    auto remove_counter(std::string_view name, labels_t labels = {}) -> bool {
        normalize_labels(labels);
        std::optional<metric_metadata> removed_meta;
        // Hold the removed metric ALIVE until the deregistration observers have run. Destroying
        // it first (which erase() would do) leaves an observer — the OTLP exporter above all —
        // detaching a metric that is already gone, while one of its export callbacks may still
        // be dereferencing the raw pointer. Destroyed at scope exit, after the notify below.
        std::unique_ptr<counter<uint64_t>> keep_alive_until_observers_return;
        {
            auto lock = std::unique_lock{m_mutex};
            for (std::size_t i = 0; i < m_counters.size(); ++i) {
                if (m_counter_metadata[i].name == name && labels_equal(m_counter_metadata[i].labels, labels)) {
                    removed_meta = std::move(m_counter_metadata[i]);
                    keep_alive_until_observers_return = std::move(m_counters[i]);
                    m_counters.erase(m_counters.begin() + static_cast<std::ptrdiff_t>(i));
                    m_counter_metadata.erase(m_counter_metadata.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
            }
        }
        // Notify outside lock to prevent slow observers from blocking. Wait out any enumeration
        // FIRST: one already publishing this metric must finish registering it (it is still alive
        // — keep_alive above holds it) before the retraction below, or the series it adds would
        // be stranded pointing at a metric this function is about to destroy.
        if (removed_meta) {
            finish_removal(std::move(keep_alive_until_observers_return), *removed_meta);
            return true;
        }
        return false;
    }

    /**
     * @brief Remove an updown_counter by name and labels
     */
    auto remove_updown_counter(std::string_view name, labels_t labels = {}) -> bool {
        normalize_labels(labels);
        std::optional<metric_metadata> removed_meta;
        // Hold the removed metric ALIVE until the deregistration observers have run. Destroying
        // it first (which erase() would do) leaves an observer — the OTLP exporter above all —
        // detaching a metric that is already gone, while one of its export callbacks may still
        // be dereferencing the raw pointer. Destroyed at scope exit, after the notify below.
        std::unique_ptr<updown_counter<int64_t>> keep_alive_until_observers_return;
        {
            auto lock = std::unique_lock{m_mutex};
            for (std::size_t i = 0; i < m_updown_counters.size(); ++i) {
                if (m_updown_counter_metadata[i].name == name &&
                    labels_equal(m_updown_counter_metadata[i].labels, labels)) {
                    removed_meta = std::move(m_updown_counter_metadata[i]);
                    keep_alive_until_observers_return = std::move(m_updown_counters[i]);
                    m_updown_counters.erase(m_updown_counters.begin() + static_cast<std::ptrdiff_t>(i));
                    m_updown_counter_metadata.erase(m_updown_counter_metadata.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
            }
        }
        if (removed_meta) {
            finish_removal(std::move(keep_alive_until_observers_return), *removed_meta);
            return true;
        }
        return false;
    }

    /**
     * @brief Remove a gauge by name and labels
     */
    auto remove_gauge(std::string_view name, labels_t labels = {}) -> bool {
        normalize_labels(labels);
        std::optional<metric_metadata> removed_meta;
        // Hold the removed metric ALIVE until the deregistration observers have run. Destroying
        // it first (which erase() would do) leaves an observer — the OTLP exporter above all —
        // detaching a metric that is already gone, while one of its export callbacks may still
        // be dereferencing the raw pointer. Destroyed at scope exit, after the notify below.
        std::unique_ptr<gauge<double>> keep_alive_until_observers_return;
        {
            auto lock = std::unique_lock{m_mutex};
            for (std::size_t i = 0; i < m_gauges.size(); ++i) {
                if (m_gauge_metadata[i].name == name && labels_equal(m_gauge_metadata[i].labels, labels)) {
                    removed_meta = std::move(m_gauge_metadata[i]);
                    keep_alive_until_observers_return = std::move(m_gauges[i]);
                    m_gauges.erase(m_gauges.begin() + static_cast<std::ptrdiff_t>(i));
                    m_gauge_metadata.erase(m_gauge_metadata.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
            }
        }
        if (removed_meta) {
            finish_removal(std::move(keep_alive_until_observers_return), *removed_meta);
            return true;
        }
        return false;
    }

    /**
     * @brief Remove a histogram by name and labels
     */
    auto remove_histogram(std::string_view name, labels_t labels = {}) -> bool {
        normalize_labels(labels);
        std::optional<metric_metadata> removed_meta;
        // Hold the removed metric ALIVE until the deregistration observers have run. Destroying
        // it first (which erase() would do) leaves an observer — the OTLP exporter above all —
        // detaching a metric that is already gone, while one of its export callbacks may still
        // be dereferencing the raw pointer. Destroyed at scope exit, after the notify below.
        std::unique_ptr<histogram> keep_alive_until_observers_return;
        {
            auto lock = std::unique_lock{m_mutex};
            for (std::size_t i = 0; i < m_histograms.size(); ++i) {
                if (m_histogram_metadata[i].name == name && labels_equal(m_histogram_metadata[i].labels, labels)) {
                    removed_meta = std::move(m_histogram_metadata[i]);
                    keep_alive_until_observers_return = std::move(m_histograms[i]);
                    m_histograms.erase(m_histograms.begin() + static_cast<std::ptrdiff_t>(i));
                    m_histogram_metadata.erase(m_histogram_metadata.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
            }
        }
        if (removed_meta) {
            finish_removal(std::move(keep_alive_until_observers_return), *removed_meta);
            return true;
        }
        return false;
    }

    /**
     * @brief Remove all metrics with names starting with a prefix
     *
     * Useful for cleaning up all metrics created by a component.
     * Example: remove_by_prefix("mycomponent.") removes all metrics
     * with names like "mycomponent.packets", "mycomponent.bytes", etc.
     *
     * @param prefix Name prefix to match
     * @return Number of metrics removed
     */
    auto remove_by_prefix(std::string_view prefix) -> std::size_t {
        std::vector<metric_metadata> removed_metrics;
        // Hold every removed metric ALIVE until the deregistration observers have run: erase()
        // would destroy them first, leaving an observer (the OTLP exporter above all) detaching
        // metrics that are already gone while an export callback may still be dereferencing
        // them. Destroyed at scope exit, after the notify loop below.
        std::vector<std::unique_ptr<counter<uint64_t>>> keep_alive_m_counters;
        std::vector<std::unique_ptr<updown_counter<int64_t>>> keep_alive_m_updown_counters;
        std::vector<std::unique_ptr<gauge<double>>> keep_alive_m_gauges;
        std::vector<std::unique_ptr<histogram>> keep_alive_m_histograms;
        {
            auto lock = std::unique_lock{m_mutex};

            // Remove counters
            for (auto it = m_counters.begin(); it != m_counters.end();) {
                auto idx = static_cast<std::size_t>(it - m_counters.begin());
                if (m_counter_metadata[idx].name.starts_with(prefix)) {
                    removed_metrics.push_back(std::move(m_counter_metadata[idx]));
                    keep_alive_m_counters.push_back(std::move(*it));
                    it = m_counters.erase(it);
                    m_counter_metadata.erase(m_counter_metadata.begin() + static_cast<std::ptrdiff_t>(idx));
                } else {
                    ++it;
                }
            }

            // Remove updown_counters
            for (auto it = m_updown_counters.begin(); it != m_updown_counters.end();) {
                auto idx = static_cast<std::size_t>(it - m_updown_counters.begin());
                if (m_updown_counter_metadata[idx].name.starts_with(prefix)) {
                    removed_metrics.push_back(std::move(m_updown_counter_metadata[idx]));
                    keep_alive_m_updown_counters.push_back(std::move(*it));
                    it = m_updown_counters.erase(it);
                    m_updown_counter_metadata.erase(m_updown_counter_metadata.begin() +
                                                    static_cast<std::ptrdiff_t>(idx));
                } else {
                    ++it;
                }
            }

            // Remove gauges
            for (auto it = m_gauges.begin(); it != m_gauges.end();) {
                auto idx = static_cast<std::size_t>(it - m_gauges.begin());
                if (m_gauge_metadata[idx].name.starts_with(prefix)) {
                    removed_metrics.push_back(std::move(m_gauge_metadata[idx]));
                    keep_alive_m_gauges.push_back(std::move(*it));
                    it = m_gauges.erase(it);
                    m_gauge_metadata.erase(m_gauge_metadata.begin() + static_cast<std::ptrdiff_t>(idx));
                } else {
                    ++it;
                }
            }

            // Remove histograms
            for (auto it = m_histograms.begin(); it != m_histograms.end();) {
                auto idx = static_cast<std::size_t>(it - m_histograms.begin());
                if (m_histogram_metadata[idx].name.starts_with(prefix)) {
                    removed_metrics.push_back(std::move(m_histogram_metadata[idx]));
                    keep_alive_m_histograms.push_back(std::move(*it));
                    it = m_histograms.erase(it);
                    m_histogram_metadata.erase(m_histogram_metadata.begin() + static_cast<std::ptrdiff_t>(idx));
                } else {
                    ++it;
                }
            }
        }

        // Same rule as the single-removal paths: defer when reentrant, otherwise drain first.
        if (!removed_metrics.empty()) {
            if (enumeration_depth() > 0) {
                const std::lock_guard lk{m_deferred_mtx};
                for (auto& owner : keep_alive_m_counters) {
                    m_deferred_owners.emplace_back(std::move(owner));
                }
                for (auto& owner : keep_alive_m_updown_counters) {
                    m_deferred_owners.emplace_back(std::move(owner));
                }
                for (auto& owner : keep_alive_m_gauges) {
                    m_deferred_owners.emplace_back(std::move(owner));
                }
                for (auto& owner : keep_alive_m_histograms) {
                    m_deferred_owners.emplace_back(std::move(owner));
                }
                for (const auto& meta : removed_metrics) {
                    m_deferred_removals.push_back(meta);
                }
                return removed_metrics.size();
            }
            wait_for_enumerations();
        }
        for (const auto& meta : removed_metrics) {
            notify_deregistration_unlocked(meta);
        }

        return removed_metrics.size();
    }

    /**
     * @brief Remove all metrics with a specific label value
     *
     * Useful for cleaning up metrics by component ID.
     * Example: remove_by_label("component", "my_component_id")
     *
     * @param label_key Label key to match
     * @param label_value Label value to match
     * @return Number of metrics removed
     */
    auto remove_by_label(std::string_view label_key, std::string_view label_value) -> std::size_t {
        std::vector<metric_metadata> removed_metrics;
        // Hold every removed metric ALIVE until the deregistration observers have run: erase()
        // would destroy them first, leaving an observer (the OTLP exporter above all) detaching
        // metrics that are already gone while an export callback may still be dereferencing
        // them. Destroyed at scope exit, after the notify loop below.
        std::vector<std::unique_ptr<counter<uint64_t>>> keep_alive_m_counters;
        std::vector<std::unique_ptr<updown_counter<int64_t>>> keep_alive_m_updown_counters;
        std::vector<std::unique_ptr<gauge<double>>> keep_alive_m_gauges;
        std::vector<std::unique_ptr<histogram>> keep_alive_m_histograms;
        {
            auto lock = std::unique_lock{m_mutex};

            auto has_label = [&](const labels_t& labels) {
                for (const auto& [k, v] : labels) {
                    if (k == label_key && v == label_value) return true;
                }
                return false;
            };

            // Remove counters
            for (auto it = m_counters.begin(); it != m_counters.end();) {
                auto idx = static_cast<std::size_t>(it - m_counters.begin());
                if (has_label(m_counter_metadata[idx].labels)) {
                    removed_metrics.push_back(std::move(m_counter_metadata[idx]));
                    keep_alive_m_counters.push_back(std::move(*it));
                    it = m_counters.erase(it);
                    m_counter_metadata.erase(m_counter_metadata.begin() + static_cast<std::ptrdiff_t>(idx));
                } else {
                    ++it;
                }
            }

            // Remove updown_counters
            for (auto it = m_updown_counters.begin(); it != m_updown_counters.end();) {
                auto idx = static_cast<std::size_t>(it - m_updown_counters.begin());
                if (has_label(m_updown_counter_metadata[idx].labels)) {
                    removed_metrics.push_back(std::move(m_updown_counter_metadata[idx]));
                    keep_alive_m_updown_counters.push_back(std::move(*it));
                    it = m_updown_counters.erase(it);
                    m_updown_counter_metadata.erase(m_updown_counter_metadata.begin() +
                                                    static_cast<std::ptrdiff_t>(idx));
                } else {
                    ++it;
                }
            }

            // Remove gauges
            for (auto it = m_gauges.begin(); it != m_gauges.end();) {
                auto idx = static_cast<std::size_t>(it - m_gauges.begin());
                if (has_label(m_gauge_metadata[idx].labels)) {
                    removed_metrics.push_back(std::move(m_gauge_metadata[idx]));
                    keep_alive_m_gauges.push_back(std::move(*it));
                    it = m_gauges.erase(it);
                    m_gauge_metadata.erase(m_gauge_metadata.begin() + static_cast<std::ptrdiff_t>(idx));
                } else {
                    ++it;
                }
            }

            // Remove histograms
            for (auto it = m_histograms.begin(); it != m_histograms.end();) {
                auto idx = static_cast<std::size_t>(it - m_histograms.begin());
                if (has_label(m_histogram_metadata[idx].labels)) {
                    removed_metrics.push_back(std::move(m_histogram_metadata[idx]));
                    keep_alive_m_histograms.push_back(std::move(*it));
                    it = m_histograms.erase(it);
                    m_histogram_metadata.erase(m_histogram_metadata.begin() + static_cast<std::ptrdiff_t>(idx));
                } else {
                    ++it;
                }
            }
        }

        // Same rule as the single-removal paths: defer when reentrant, otherwise drain first.
        if (!removed_metrics.empty()) {
            if (enumeration_depth() > 0) {
                const std::lock_guard lk{m_deferred_mtx};
                for (auto& owner : keep_alive_m_counters) {
                    m_deferred_owners.emplace_back(std::move(owner));
                }
                for (auto& owner : keep_alive_m_updown_counters) {
                    m_deferred_owners.emplace_back(std::move(owner));
                }
                for (auto& owner : keep_alive_m_gauges) {
                    m_deferred_owners.emplace_back(std::move(owner));
                }
                for (auto& owner : keep_alive_m_histograms) {
                    m_deferred_owners.emplace_back(std::move(owner));
                }
                for (const auto& meta : removed_metrics) {
                    m_deferred_removals.push_back(meta);
                }
                return removed_metrics.size();
            }
            wait_for_enumerations();
        }
        for (const auto& meta : removed_metrics) {
            notify_deregistration_unlocked(meta);
        }

        return removed_metrics.size();
    }

    // ========================================================================
    // Observer Registration - for external systems (OTel, etc.)
    // ========================================================================

    /**
     * @brief Register an observer to be notified of metric registrations
     *
     * The observer will be called for all subsequently created metrics.
     * If notify_existing is true, the observer is also called for all
     * metrics that already exist.
     *
     * @param callback Function to call when metrics are created
     * @param notify_existing If true, call callback for existing metrics
     * @return Observer ID that can be used to unregister
     */
    auto add_observer(registration_callback callback, bool notify_existing = true) -> std::size_t {
        std::size_t id{};
        std::vector<std::pair<metric_metadata, void*>> existing_metrics;
        error_callback error_handler_copy;

        // Held across the snapshot AND the notification below, so a concurrent removal cannot
        // destroy a metric in between. A removal that gets in first is simply absent from the
        // snapshot; one that arrives after blocks on this until the callbacks have run, and its
        // own keep-alive holds the metric valid throughout.
        std::shared_ptr<detail::observer_slot<registration_callback>> slot;
        {
            auto lock = std::unique_lock{m_mutex};

            id = m_next_observer_id++;
            slot = std::make_shared<detail::observer_slot<registration_callback>>(callback);
            m_observers[id] = slot;
            // Counted under m_mutex, so it is atomic with the snapshot below: a removal either
            // sees this enumeration and waits it out, or erases first and is simply absent here.
            if (notify_existing) {
                m_enumerations_in_flight.fetch_add(1, std::memory_order_seq_cst);
            }
            error_handler_copy = m_error_handler;

            if (notify_existing) {
                // Collect all existing metrics while holding lock
                existing_metrics.reserve(m_counters.size() + m_updown_counters.size() + m_gauges.size() +
                                         m_histograms.size());

                for (std::size_t i = 0; i < m_counters.size(); ++i) {
                    existing_metrics.emplace_back(m_counter_metadata[i], m_counters[i].get());
                }
                for (std::size_t i = 0; i < m_updown_counters.size(); ++i) {
                    existing_metrics.emplace_back(m_updown_counter_metadata[i], m_updown_counters[i].get());
                }
                for (std::size_t i = 0; i < m_gauges.size(); ++i) {
                    existing_metrics.emplace_back(m_gauge_metadata[i], m_gauges[i].get());
                }
                for (std::size_t i = 0; i < m_histograms.size(); ++i) {
                    existing_metrics.emplace_back(m_histogram_metadata[i], m_histograms[i].get());
                }
            }
        }

        // Notify for existing metrics outside lock to prevent slow observers from blocking.
        // RAII so the enumeration count is released even if a callback throws.
        struct enumeration_guard {
            registry* self;
            bool active;
            explicit enumeration_guard(registry* s, bool a) : self(s), active(a) {
                if (active) {
                    ++enumeration_depth();
                }
            }
            enumeration_guard(const enumeration_guard&) = delete;
            auto operator=(const enumeration_guard&) -> enumeration_guard& = delete;
            enumeration_guard(enumeration_guard&&) = delete;
            auto operator=(enumeration_guard&&) -> enumeration_guard& = delete;
            ~enumeration_guard() {
                if (!active) {
                    return;
                }
                const bool outermost = --enumeration_depth() == 0;
                if (self->m_enumerations_in_flight.fetch_sub(1, std::memory_order_seq_cst) == 1) {
                    const std::lock_guard lk{self->m_drain_mtx};
                    self->m_drain_cv.notify_all();
                }
                if (outermost) {
                    self->flush_deferred_removals(); // retract, then destroy, now it is safe
                }
            }
        } guard{this, notify_existing};

        for (const auto& [meta, ptr] : existing_metrics) {
            try {
                invoke_observer(slot, [&](const registration_callback& cb) { cb(meta, ptr); });
            } catch (...) {
                if (error_handler_copy) {
                    try {
                        error_handler_copy(id, meta.name, std::current_exception());
                    } catch (...) {
                        // Error handler itself threw - nothing we can do
                    }
                }
            }
        }

        return id;
    }

    /**
     * @brief Remove an observer
     *
     * @param observer_id ID returned from add_observer
     */
    auto remove_observer(std::size_t observer_id) -> void {
        // m_observer_mtx first: a notification may be running RIGHT NOW with a copy of the
        // callback map taken before this call. Erasing from the map alone would not stop it, so
        // this waits the in-flight callback out. Once this returns, the observer is neither
        // running nor reachable — which is what lets a subscriber (the telemetry manager) tear
        // down the state its callback touches.
        std::shared_ptr<detail::observer_slot<registration_callback>> slot;
        {
            auto lock = std::unique_lock{m_mutex};
            if (auto it = m_observers.find(observer_id); it != m_observers.end()) {
                slot = it->second;
                m_observers.erase(it);
            }
        }
        // Erasing the map entry is not enough: a notifier copies the map before calling, so a
        // copied slot may be about to run. Close it and wait for any call already in progress —
        // once this returns, the callback is neither running nor startable.
        retire_observer(slot);
    }

    /**
     * @brief Register an observer to be notified of metric removals
     *
     * The observer will be called when metrics are removed via remove_*()
     * or remove_by_prefix()/remove_by_label().
     *
     * @param callback Function to call when metrics are removed
     * @return Observer ID that can be used to unregister
     */
    auto add_deregistration_observer(deregistration_callback callback) -> std::size_t {
        auto lock = std::unique_lock{m_mutex};
        auto id = m_next_deregistration_observer_id++;
        m_deregistration_observers[id] =
            std::make_shared<detail::observer_slot<deregistration_callback>>(std::move(callback));
        return id;
    }

    /**
     * @brief Remove a deregistration observer
     *
     * @param observer_id ID returned from add_deregistration_observer
     */
    auto remove_deregistration_observer(std::size_t observer_id) -> void {
        std::shared_ptr<detail::observer_slot<deregistration_callback>> slot;
        {
            auto lock = std::unique_lock{m_mutex};
            if (auto it = m_deregistration_observers.find(observer_id); it != m_deregistration_observers.end()) {
                slot = it->second;
                m_deregistration_observers.erase(it);
            }
        }
        retire_observer(slot); // see remove_observer
    }

    // ========================================================================
    // Query Interface - NOT hot path, called by REST/SSE/OTel export
    // ========================================================================

    /**
     * @brief Get snapshots of all registered metrics
     *
     * Thread-safe: uses shared lock, does not block writers.
     *
     * @return Vector of metric snapshots
     */
    [[nodiscard]]
    auto snapshot_all() const -> std::vector<metric_snapshot> {
        auto lock = std::shared_lock{m_mutex};
        auto now = std::chrono::system_clock::now();
        std::vector<metric_snapshot> snapshots;
        snapshots.reserve(m_counters.size() + m_updown_counters.size() + m_gauges.size() + m_histograms.size());

        // Counters
        for (std::size_t i = 0; i < m_counters.size(); ++i) {
            const auto& meta = m_counter_metadata[i];
            snapshots.push_back(
                {meta.name, meta.description, meta.unit, meta.type, meta.labels, m_counters[i]->value(), now});
        }

        // UpDown Counters
        for (std::size_t i = 0; i < m_updown_counters.size(); ++i) {
            const auto& meta = m_updown_counter_metadata[i];
            snapshots.push_back(
                {meta.name, meta.description, meta.unit, meta.type, meta.labels, m_updown_counters[i]->value(), now});
        }

        // Gauges
        for (std::size_t i = 0; i < m_gauges.size(); ++i) {
            const auto& meta = m_gauge_metadata[i];
            snapshots.push_back(
                {meta.name, meta.description, meta.unit, meta.type, meta.labels, m_gauges[i]->value(), now});
        }

        // Histograms - use atomic snapshot for consistency
        for (std::size_t i = 0; i < m_histograms.size(); ++i) {
            const auto& meta = m_histogram_metadata[i];
            const auto& h = *m_histograms[i];
            auto data = h.snapshot(); // Atomic snapshot of bucket_counts, count, sum
            snapshots.push_back(
                {meta.name, meta.description, meta.unit, meta.type, meta.labels,
                 histogram_snapshot{h.boundaries(), std::move(data.bucket_counts), data.count, data.sum}, now});
        }

        return snapshots;
    }

    /**
     * @brief Get snapshots of metrics matching a name prefix
     *
     * @param prefix Prefix to match (e.g., "composite.port" matches
     *               "composite.port.packets", "composite.port.bytes", etc.)
     * @return Vector of matching metric snapshots
     */
    [[nodiscard]]
    auto snapshot_by_prefix(std::string_view prefix) const -> std::vector<metric_snapshot> {
        auto all = snapshot_all();
        std::vector<metric_snapshot> filtered;

        for (auto& snap : all) {
            if (snap.name.starts_with(prefix)) {
                filtered.push_back(std::move(snap));
            }
        }

        return filtered;
    }

    /**
     * @brief Get snapshots of metrics with a specific label value
     *
     * @param label_key Label key to filter by
     * @param label_value Label value to match
     * @return Vector of matching metric snapshots
     */
    [[nodiscard]]
    auto snapshot_by_label(std::string_view label_key, std::string_view label_value) const
        -> std::vector<metric_snapshot> {
        auto all = snapshot_all();
        std::vector<metric_snapshot> filtered;

        for (auto& snap : all) {
            for (const auto& [key, val] : snap.labels) {
                if (key == label_key && val == label_value) {
                    filtered.push_back(std::move(snap));
                    break;
                }
            }
        }

        return filtered;
    }

    /**
     * @brief Get count of registered metrics
     */
    [[nodiscard]]
    auto metric_count() const -> std::size_t {
        auto lock = std::shared_lock{m_mutex};
        return m_counters.size() + m_updown_counters.size() + m_gauges.size() + m_histograms.size();
    }

#ifdef COMPOSITE_TESTING
    /**
     * @brief Clear all metrics (tests only)
     *
     * Notifies deregistration observers for each metric before clearing.
     * Observer notification is done outside the lock to prevent slow
     * observers from blocking.
     */
    void clear() {
        // Collect all metadata while locked
        std::vector<metric_metadata> all_metadata;
        {
            auto lock = std::unique_lock{m_mutex};
            all_metadata.reserve(m_counter_metadata.size() + m_updown_counter_metadata.size() +
                                 m_gauge_metadata.size() + m_histogram_metadata.size());
            for (const auto& meta : m_counter_metadata) {
                all_metadata.push_back(meta);
            }
            for (const auto& meta : m_updown_counter_metadata) {
                all_metadata.push_back(meta);
            }
            for (const auto& meta : m_gauge_metadata) {
                all_metadata.push_back(meta);
            }
            for (const auto& meta : m_histogram_metadata) {
                all_metadata.push_back(meta);
            }
        }

        // Notify deregistration observers outside the lock
        for (const auto& meta : all_metadata) {
            notify_deregistration_unlocked(meta);
        }

        // Now clear everything while locked
        {
            auto lock = std::unique_lock{m_mutex};
            m_counters.clear();
            m_counter_metadata.clear();
            m_updown_counters.clear();
            m_updown_counter_metadata.clear();
            m_gauges.clear();
            m_gauge_metadata.clear();
            m_histograms.clear();
            m_histogram_metadata.clear();
        }
    }
#endif // COMPOSITE_TESTING

    // Disable copy/move (singleton)
    registry(const registry&) = delete;
    registry(registry&&) = delete;
    auto operator=(const registry&) -> registry& = delete;
    auto operator=(registry&&) -> registry& = delete;

private:
    registry() = default;

    /**
     * @brief Notify all registered observers of a new metric
     *
     * Called WITHOUT lock held to prevent slow observers from blocking.
     * Takes pre-copied observers and error handler from the caller.
     *
     * @param meta Metadata of the newly registered metric
     * @param ptr Pointer to the metric
     * @param observers_copy Copy of observers map (taken while lock was held)
     * @param error_handler_copy Copy of error handler (taken while lock was held)
     */
    auto notify_registration_unlocked(const metric_metadata& meta, void* ptr,
                                      const registration_observer_map& observers_copy,
                                      const error_callback& error_handler_copy) -> void {
        for (const auto& [id, slot] : observers_copy) {
            try {
                invoke_observer(slot, [&](const registration_callback& callback) { callback(meta, ptr); });
            } catch (...) {
                // Observer callbacks should not throw, but don't let one
                // bad observer break registration. Report error if handler set.
                if (error_handler_copy) {
                    try {
                        error_handler_copy(id, meta.name, std::current_exception());
                    } catch (...) {
                        // Error handler itself threw - nothing we can do
                    }
                }
            }
        }
    }

    /**
     * @brief Notify all deregistration observers of a removed metric
     *
     * Called WITHOUT lock held to prevent slow observers from blocking.
     * Copies the observer map to allow concurrent modifications.
     */
    auto notify_deregistration_unlocked(const metric_metadata& meta) -> void {
        // Copy observers under lock
        decltype(m_deregistration_observers) observers_copy;
        error_callback error_handler_copy;
        {
            auto lock = std::shared_lock{m_mutex};
            observers_copy = m_deregistration_observers;
            error_handler_copy = m_error_handler;
        }

        // Call observers without lock
        for (const auto& [id, slot] : observers_copy) {
            try {
                invoke_observer(slot, [&](const deregistration_callback& callback) { callback(meta); });
            } catch (...) {
                if (error_handler_copy) {
                    try {
                        error_handler_copy(id, meta.name, std::current_exception());
                    } catch (...) {
                        // Error handler itself threw - nothing we can do
                    }
                }
            }
        }
    }

    /**
     * @brief Check if labels match
     */
    static auto labels_equal(const labels_t& a, const labels_t& b) -> bool {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (a[i].first != b[i].first || a[i].second != b[i].second) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief Find existing counter with same name and labels
     *
     * @return Pointer to existing counter or nullptr if not found
     */
    auto find_counter(std::string_view name, const labels_t& labels) -> counter<uint64_t>* {
        for (std::size_t i = 0; i < m_counters.size(); ++i) {
            if (m_counter_metadata[i].name == name && labels_equal(m_counter_metadata[i].labels, labels)) {
                return m_counters[i].get();
            }
        }
        return nullptr;
    }

    /**
     * @brief Find existing updown_counter with same name and labels
     */
    auto find_updown_counter(std::string_view name, const labels_t& labels) -> updown_counter<int64_t>* {
        for (std::size_t i = 0; i < m_updown_counters.size(); ++i) {
            if (m_updown_counter_metadata[i].name == name &&
                labels_equal(m_updown_counter_metadata[i].labels, labels)) {
                return m_updown_counters[i].get();
            }
        }
        return nullptr;
    }

    /**
     * @brief Find existing gauge with same name and labels
     */
    auto find_gauge(std::string_view name, const labels_t& labels) -> gauge<double>* {
        for (std::size_t i = 0; i < m_gauges.size(); ++i) {
            if (m_gauge_metadata[i].name == name && labels_equal(m_gauge_metadata[i].labels, labels)) {
                return m_gauges[i].get();
            }
        }
        return nullptr;
    }

    /**
     * @brief Find existing histogram with same name and labels
     */
    auto find_histogram(std::string_view name, const labels_t& labels) -> histogram* {
        for (std::size_t i = 0; i < m_histograms.size(); ++i) {
            if (m_histogram_metadata[i].name == name && labels_equal(m_histogram_metadata[i].labels, labels)) {
                return m_histograms[i].get();
            }
        }
        return nullptr;
    }

    // ========================================================================
    // Implementation methods (must be called with lock held)
    // ========================================================================

    /**
     * @brief Check if adding another metric would exceed the limit
     *
     * @throws metric_limit_exceeded_error if limit would be exceeded
     */
    auto check_metric_limit() const -> void {
        if (m_max_metrics > 0) {
            auto current = m_counters.size() + m_updown_counters.size() + m_gauges.size() + m_histograms.size();
            if (current >= m_max_metrics) {
                throw metric_limit_exceeded_error(m_max_metrics);
            }
        }
    }

    auto create_counter_impl(std::string name, std::string description, std::string unit, labels_t labels)
        -> counter<uint64_t>& {
        check_metric_limit();
        m_counters.push_back(std::make_unique<counter<uint64_t>>());
        metric_metadata meta{std::move(name), std::move(description), std::move(unit), metric_type::counter,
                             std::move(labels)};
        auto* ptr = m_counters.back().get();
        m_counter_metadata.push_back(std::move(meta));
        // Note: Caller is responsible for notifying observers after releasing lock
        return *ptr;
    }

    auto create_updown_counter_impl(std::string name, std::string description, std::string unit, labels_t labels)
        -> updown_counter<int64_t>& {
        check_metric_limit();
        m_updown_counters.push_back(std::make_unique<updown_counter<int64_t>>());
        metric_metadata meta{std::move(name), std::move(description), std::move(unit), metric_type::updown_counter,
                             std::move(labels)};
        auto* ptr = m_updown_counters.back().get();
        m_updown_counter_metadata.push_back(std::move(meta));
        // Note: Caller is responsible for notifying observers after releasing lock
        return *ptr;
    }

    auto create_gauge_impl(std::string name, std::string description, std::string unit, labels_t labels)
        -> gauge<double>& {
        check_metric_limit();
        m_gauges.push_back(std::make_unique<gauge<double>>());
        metric_metadata meta{std::move(name), std::move(description), std::move(unit), metric_type::gauge,
                             std::move(labels)};
        auto* ptr = m_gauges.back().get();
        m_gauge_metadata.push_back(std::move(meta));
        // Note: Caller is responsible for notifying observers after releasing lock
        return *ptr;
    }

    auto create_histogram_impl(std::string name, std::string description, std::string unit,
                               std::vector<double> boundaries, labels_t labels) -> histogram& {
        check_metric_limit();
        m_histograms.push_back(std::make_unique<histogram>(std::move(boundaries)));
        metric_metadata meta{std::move(name), std::move(description), std::move(unit), metric_type::histogram,
                             std::move(labels)};
        auto* ptr = m_histograms.back().get();
        m_histogram_metadata.push_back(std::move(meta));
        // Note: Caller is responsible for notifying observers after releasing lock
        return *ptr;
    }

    auto create_histogram_pow2_impl(std::string name, std::string description, std::string unit,
                                    std::size_t num_buckets, labels_t labels) -> histogram& {
        check_metric_limit();
        auto boundaries = histogram::power_of_2_boundaries(num_buckets);
        m_histograms.push_back(std::make_unique<histogram>(std::move(boundaries)));
        m_histograms.back()->enable_power_of_2_lookup();
        metric_metadata meta{std::move(name), std::move(description), std::move(unit), metric_type::histogram,
                             std::move(labels)};
        auto* ptr = m_histograms.back().get();
        m_histogram_metadata.push_back(std::move(meta));
        // Note: Caller is responsible for notifying observers after releasing lock
        return *ptr;
    }

    mutable std::shared_mutex m_mutex;
    std::size_t m_max_metrics{DEFAULT_MAX_METRICS};
    error_callback m_error_handler;

    // Separate storage for each type (avoids variant overhead in hot path)
    /// Drain coordination for observer callbacks and metric enumeration.
    ///
    /// NO LOCK IS HELD WHILE A USER CALLBACK RUNS. An earlier attempt serialized everything on one
    /// mutex held across the callbacks, which deadlocked any observer that created a metric,
    /// removed one, or touched observer registration — a silent restriction on existing public
    /// API. Two independent counters replace it:
    ///
    ///  - each observer_slot tracks its own in-flight callbacks, so remove_observer() can close
    ///    admission and wait for its callback to finish. Copying the observer map under m_mutex
    ///    is not enough on its own: the copy predates the call, so an erase could otherwise
    ///    return while a copied callback was about to run against torn-down state.
    ///  - m_enumerations_in_flight covers add_observer(notify_existing), which publishes raw
    ///    metric pointers after releasing m_mutex. A removal waits this out BEFORE announcing the
    ///    deregistration, so an enumeration that is mid-flight registers the metric (still alive
    ///    via the remover's keep-alive) and the retraction lands afterwards rather than being
    ///    lost.
    mutable std::mutex m_drain_mtx;
    mutable std::condition_variable m_drain_cv;
    std::atomic<int> m_enumerations_in_flight{0};

    /// How many enumerations THIS thread is inside. A depth, not a flag: enumerations can nest
    /// if an observer callback subscribes another observer with notify_existing.
    static auto enumeration_depth() -> int& {
        static thread_local int depth = 0;
        return depth;
    }

    /// Slots this thread is currently invoking, innermost last. Lets an observer retire ITSELF
    /// without waiting for an in-flight count that only it can decrement.
    static auto invoking_slots() -> std::vector<const void*>& {
        static thread_local std::vector<const void*> slots;
        return slots;
    }

    /// Block until no enumeration is publishing metric pointers. Cold path (removal only).
    /// Callers must handle the reentrant case themselves — see finish_removal().
    auto wait_for_enumerations() const -> void {
        std::unique_lock lk{m_drain_mtx};
        m_drain_cv.wait(lk, [this] { return m_enumerations_in_flight.load(std::memory_order_acquire) == 0; });
    }

    /// Complete a removal: retract the series, then destroy the metric.
    ///
    /// Reentrant case (this thread is inside an enumeration): DEFER both. The enumeration holds
    /// raw pointers to a snapshot it has not finished walking, so destroying this metric now
    /// would leave a later entry dangling — and retracting now would let the enumeration register
    /// the metric AFTERWARDS, stranding a series. Both are queued and run when the outermost
    /// enumeration finishes, which puts the retraction after the registration and the destruction
    /// after both.
    template <typename Metric>
    auto finish_removal(std::unique_ptr<Metric> owner, const metric_metadata& meta) -> void {
        if (enumeration_depth() > 0) {
            const std::lock_guard lk{m_deferred_mtx};
            m_deferred_owners.emplace_back(std::move(owner)); // shared_ptr<void> adopts the deleter
            m_deferred_removals.push_back(meta);
            return;
        }
        wait_for_enumerations();
        notify_deregistration_unlocked(meta);
        // `owner` dies here, after every observer has retracted it.
    }

    /// Run the removals deferred by reentrant callbacks. Called when the outermost enumeration
    /// on this thread completes.
    auto flush_deferred_removals() -> void {
        std::vector<std::shared_ptr<void>> owners;
        std::vector<metric_metadata> removals;
        {
            const std::lock_guard lk{m_deferred_mtx};
            owners.swap(m_deferred_owners);
            removals.swap(m_deferred_removals);
        }
        for (const auto& meta : removals) {
            notify_deregistration_unlocked(meta);
        }
        // owners destroyed here, after the retractions above.
    }

    mutable std::mutex m_deferred_mtx;
    std::vector<std::shared_ptr<void>> m_deferred_owners; ///< kept alive until enumeration ends
    std::vector<metric_metadata> m_deferred_removals;     ///< retractions owed once it does

    std::vector<std::unique_ptr<counter<uint64_t>>> m_counters;
    std::vector<metric_metadata> m_counter_metadata;

    std::vector<std::unique_ptr<updown_counter<int64_t>>> m_updown_counters;
    std::vector<metric_metadata> m_updown_counter_metadata;

    std::vector<std::unique_ptr<gauge<double>>> m_gauges;
    std::vector<metric_metadata> m_gauge_metadata;

    std::vector<std::unique_ptr<histogram>> m_histograms;
    std::vector<metric_metadata> m_histogram_metadata;

    // Observer management
    /// Run @p slot's callback unless it has been closed, keeping remove_observer() informed.
    /// The seq_cst pairing matters: the increment and the `closed` load must not BOTH miss the
    /// remover's store and its in-flight read, or a callback could start after removal returned.
    template <typename Slot, typename Invoke>
    auto invoke_observer(const Slot& slot, Invoke&& invoke) const -> void {
        // The decrement MUST be RAII: observer callbacks are allowed to throw (the registry
        // catches and routes to the error handler), and a straight-line decrement after the call
        // is skipped on that path — leaving in_flight stuck above zero and remove_observer()
        // waiting forever. That is a hang, not a leak.
        struct flight_guard {
            const registry* self;
            const Slot& slot;
            flight_guard(const registry* registry_ptr, const Slot& observer_slot)
                : self(registry_ptr), slot(observer_slot) {}
            flight_guard(const flight_guard&) = delete;
            auto operator=(const flight_guard&) -> flight_guard& = delete;
            flight_guard(flight_guard&&) = delete;
            auto operator=(flight_guard&&) -> flight_guard& = delete;
            ~flight_guard() {
                if (slot->in_flight.fetch_sub(1, std::memory_order_seq_cst) == 1) {
                    const std::lock_guard lk{self->m_drain_mtx};
                    self->m_drain_cv.notify_all();
                }
            }
        };

        slot->in_flight.fetch_add(1, std::memory_order_seq_cst);
        const flight_guard guard{this, slot};
        if (!slot->closed.load(std::memory_order_seq_cst)) {
            invoking_slots().push_back(slot.get());
            struct pop_guard {
                ~pop_guard() { invoking_slots().pop_back(); }
            } popper;
            invoke(slot->callback);
        }
    }

    /// Close @p slot and wait for any callback already running to finish.
    template <typename Slot>
    auto retire_observer(const Slot& slot) const -> void {
        if (!slot) {
            return;
        }
        slot->closed.store(true, std::memory_order_seq_cst);
        // An observer removing ITSELF from inside its own callback must not wait: the only
        // in-flight count is this very call, and only its return can decrement it. `closed` is
        // already set, so no further invocation can start — which is the guarantee callers
        // actually need. (Two observers each removing the other, concurrently, would still
        // deadlock; that is documented as unsupported rather than defended against.)
        const auto& active = invoking_slots();
        if (std::find(active.begin(), active.end(), static_cast<const void*>(slot.get())) != active.end()) {
            return;
        }
        std::unique_lock lk{m_drain_mtx};
        m_drain_cv.wait(lk, [&slot] { return slot->in_flight.load(std::memory_order_seq_cst) == 0; });
    }

    registration_observer_map m_observers;
    std::size_t m_next_observer_id{};

    // Deregistration observer management
    deregistration_observer_map m_deregistration_observers;
    std::size_t m_next_deregistration_observer_id{};
};

} // namespace composite::metrics
