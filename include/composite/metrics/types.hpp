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

#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace composite::metrics {

/**
 * @brief Label key-value pair for dimensional metrics
 */
using label_pair = std::pair<std::string, std::string>;
using labels_t = std::vector<label_pair>;

/**
 * @brief Metric type enumeration
 */
enum class metric_type { counter, updown_counter, gauge, histogram };

/**
 * @brief Convert metric type to string
 */
inline auto to_string(metric_type type) -> std::string_view {
    switch (type) {
    case metric_type::counter:
        return "counter";
    case metric_type::updown_counter:
        return "updown_counter";
    case metric_type::gauge:
        return "gauge";
    case metric_type::histogram:
        return "histogram";
    }
    return "unknown";
}

// ============================================================================
// Cache-line aligned storage for zero false-sharing
// ============================================================================

/**
 * @brief Cache-line aligned atomic storage
 *
 * Ensures each metric's atomic value sits on its own cache line to prevent
 * false sharing when multiple threads update different metrics.
 */
template <typename T>
struct alignas(64) aligned_atomic {
    std::atomic<T> value{};

    static_assert(sizeof(std::atomic<T>) <= 64, "aligned_atomic requires atomic size <= cache line");

    aligned_atomic() = default;
    explicit aligned_atomic(T initial) : value(initial) {}

    // No manual padding member: alignas(64) on the struct already rounds sizeof to a full
    // cache line, and the previous char[64 - sizeof(atomic<T>)] became a zero-length array
    // (ill-formed ISO C++) whenever the atomic exactly filled the line.
};
static_assert(sizeof(aligned_atomic<std::uint64_t>) == 64, "aligned_atomic must occupy a full cache line");

// ============================================================================
// Counter - Monotonically increasing value
// ============================================================================

/**
 * @brief Monotonically increasing counter
 *
 * Use for values that only go up: packets sent, bytes transferred, errors.
 * Thread-safe with minimal overhead using relaxed memory ordering.
 */
template <typename T = uint64_t>
class counter {
public:
    counter() = default;

    /**
     * @brief Add a positive delta to the counter
     * @param delta Value to add (should be positive)
     */
    [[gnu::always_inline]]
    void add(T delta) noexcept {
        m_storage.value.fetch_add(delta, std::memory_order_relaxed);
    }

    /**
     * @brief Increment counter by 1
     */
    [[gnu::always_inline]]
    void inc() noexcept {
        add(1);
    }

    /**
     * @brief Get current counter value
     * @return Current value (may be slightly stale due to relaxed ordering)
     */
    [[nodiscard]]
    auto value() const noexcept -> T {
        return m_storage.value.load(std::memory_order_relaxed);
    }

    /**
     * @brief Reset counter to zero
     *
     * @deprecated A `counter` is the framework's MONOTONIC instrument: the OTLP bridge exports
     * it as a monotonic sum, and a collector reads any decrease as a counter reset — it then
     * adds the whole new value and fabricates an enormous rate (the exact failure 0.5 fixed
     * for histogram sums). Scheduled for removal in 0.6 with no replacement: track a baseline
     * and subtract, or use `updown_counter` (whose `reset()` remains) for resettable values.
     */
    [[deprecated("counters are exported as monotonic sums; resetting one fabricates rates in "
                 "OTLP collectors. Removed in 0.6 — track a baseline, or use updown_counter.")]]
    void reset() noexcept {
        m_storage.value.store(0, std::memory_order_relaxed);
    }

    // ---- Arithmetic operators ----

    /**
     * @brief Prefix increment (++counter)
     * @return Reference to this counter
     */
    [[gnu::always_inline]]
    auto operator++() noexcept -> counter& {
        inc();
        return *this;
    }

    /**
     * @brief Postfix increment (counter++)
     * @return Previous value before increment
     */
    [[gnu::always_inline]]
    auto operator++(int) noexcept -> T {
        return m_storage.value.fetch_add(1, std::memory_order_relaxed);
    }

    /**
     * @brief Add-assign operator (counter += delta)
     * @param delta Value to add
     * @return Reference to this counter
     */
    [[gnu::always_inline]]
    auto operator+=(T delta) noexcept -> counter& {
        add(delta);
        return *this;
    }

private:
    aligned_atomic<T> m_storage{};
};

// ============================================================================
// UpDownCounter - Non-monotonic sum (can increase or decrease)
// ============================================================================

/**
 * @brief Non-monotonic counter that can increase or decrease
 *
 * Use for values that represent a sum of increments/decrements:
 * queue depth, active connections, in-flight requests.
 *
 * Unlike gauge, this tracks changes rather than absolute values.
 */
template <typename T = int64_t>
class updown_counter {
public:
    updown_counter() = default;

    /**
     * @brief Add a delta (positive or negative) to the counter
     * @param delta Value to add
     */
    [[gnu::always_inline]]
    void add(T delta) noexcept {
        m_storage.value.fetch_add(delta, std::memory_order_relaxed);
    }

    /**
     * @brief Increment counter by 1
     */
    [[gnu::always_inline]]
    void inc() noexcept {
        add(1);
    }

    /**
     * @brief Decrement counter by 1
     */
    [[gnu::always_inline]]
    void dec() noexcept {
        add(-1);
    }

    /**
     * @brief Get current counter value
     * @return Current value (may be slightly stale due to relaxed ordering)
     */
    [[nodiscard]]
    auto value() const noexcept -> T {
        return m_storage.value.load(std::memory_order_relaxed);
    }

    /**
     * @brief Reset counter to zero
     */
    void reset() noexcept { m_storage.value.store(0, std::memory_order_relaxed); }

    // ---- Arithmetic operators ----

    /**
     * @brief Prefix increment (++counter)
     * @return Reference to this counter
     */
    [[gnu::always_inline]]
    auto operator++() noexcept -> updown_counter& {
        inc();
        return *this;
    }

    /**
     * @brief Postfix increment (counter++)
     * @return Previous value before increment
     */
    [[gnu::always_inline]]
    auto operator++(int) noexcept -> T {
        return m_storage.value.fetch_add(1, std::memory_order_relaxed);
    }

    /**
     * @brief Prefix decrement (--counter)
     * @return Reference to this counter
     */
    [[gnu::always_inline]]
    auto operator--() noexcept -> updown_counter& {
        dec();
        return *this;
    }

    /**
     * @brief Postfix decrement (counter--)
     * @return Previous value before decrement
     */
    [[gnu::always_inline]]
    auto operator--(int) noexcept -> T {
        return m_storage.value.fetch_sub(1, std::memory_order_relaxed);
    }

    /**
     * @brief Add-assign operator (counter += delta)
     * @param delta Value to add
     * @return Reference to this counter
     */
    [[gnu::always_inline]]
    auto operator+=(T delta) noexcept -> updown_counter& {
        add(delta);
        return *this;
    }

    /**
     * @brief Subtract-assign operator (counter -= delta)
     * @param delta Value to subtract
     * @return Reference to this counter
     */
    [[gnu::always_inline]]
    auto operator-=(T delta) noexcept -> updown_counter& {
        add(-delta);
        return *this;
    }

private:
    aligned_atomic<T> m_storage{};
};

// ============================================================================
// Gauge - Point-in-time value
// ============================================================================

/**
 * @brief Point-in-time measurement
 *
 * Use for values where you set the current state:
 * CPU percentage, temperature, memory usage.
 *
 * Unlike updown_counter, this sets absolute values rather than tracking changes.
 */
template <typename T = double>
class gauge {
public:
    gauge() = default;

    /**
     * @brief Set the gauge to a specific value
     * @param val New value
     */
    [[gnu::always_inline]]
    void set(T val) noexcept {
        if constexpr (std::is_floating_point_v<T>) {
            // For floating point, we need to use a bit-cast approach
            // since std::atomic<double> may not be lock-free on all platforms
            static_assert(sizeof(T) <= sizeof(uint64_t), "Floating point type too large");
            m_storage.value.store(std::bit_cast<uint64_t>(val), std::memory_order_relaxed);
        } else {
            m_storage.value.store(val, std::memory_order_relaxed);
        }
    }

    /**
     * @brief Get current gauge value
     * @return Current value
     */
    [[nodiscard]]
    auto value() const noexcept -> T {
        if constexpr (std::is_floating_point_v<T>) {
            return std::bit_cast<T>(m_storage.value.load(std::memory_order_relaxed));
        } else {
            return m_storage.value.load(std::memory_order_relaxed);
        }
    }

    // ---- Arithmetic operators ----

    /**
     * @brief Assignment operator (gauge = value)
     * @param val Value to set
     * @return Reference to this gauge
     */
    [[gnu::always_inline]]
    auto operator=(T val) noexcept -> gauge& {
        set(val);
        return *this;
    }

private:
    // Use uint64_t storage for floating point to ensure lock-free operations
    using storage_type = std::conditional_t<std::is_floating_point_v<T>, uint64_t, T>;
    aligned_atomic<storage_type> m_storage{};
};

// ============================================================================
// Histogram - Distribution of values with fixed buckets
// ============================================================================

/**
 * @brief Snapshot of histogram data
 *
 * NOT a single point-in-time view while writers are active, and it never was — the earlier claim
 * that "all fields are guaranteed to be from the same point in time" (and the reference to a
 * seqlock in snapshot()) described an implementation that no longer exists. The seqlock was
 * removed because it was only correct for ONE recorder; independent per-field atomics are correct
 * for any number. The actual contract:
 *
 *  - CONCURRENT with record(): each field is individually never torn, but the fields are read one
 *    at a time and may disagree with each other. In particular the bucket counts need not sum to
 *    count, and sum need not correspond to either. Do not assert cross-field invariants on a
 *    snapshot taken while anything is recording.
 *  - QUIESCENT (no recorder running): exact and mutually consistent.
 *
 * This is what an aggregating exporter needs, and the honest description of what the code does.
 * Adding synchronization to make it stronger would put a lock on the hot record() path to fix a
 * discrepancy that resolves itself on the next export interval.
 */
struct histogram_data {
    std::vector<uint64_t> bucket_counts;
    uint64_t count;
    double sum;
    /// Observations record() refused for being negative, NaN or infinite. Carried in the snapshot
    /// (and out through REST) rather than left on the accessor alone: a non-zero value means
    /// instrumentation somewhere is feeding a histogram values it cannot represent, and advice to
    /// alert on it is not actionable if the number never leaves the process.
    uint64_t rejected;
};

/**
 * @brief Distribution tracking with fixed bucket boundaries
 *
 * Use for latency distributions, size distributions, etc.
 *
 * Bucket lookup is O(log n) in the number of boundaries (std::lower_bound). It used to advertise an
 * O(1) power-of-2 fast path; that path was removed because it disagreed with the binary-search path
 * about which bucket a value belongs to (e.g. 1.5), and correctness won. power_of_2_boundaries()
 * remains a convenient way to GENERATE boundaries — it just no longer changes the lookup.
 *
 * Thread safety:
 * - record() is always thread-safe
 * - Individual accessors (count(), sum(), bucket_counts()) are thread-safe
 *   but may return inconsistent data if called during concurrent writes
 * - Use snapshot() to read every field in one call. It is NOT atomic across fields while writers
 *   are active — see histogram_data for the exact contract — but it avoids interleaving caller
 *   logic between the individual accessors
 */
class histogram {
public:
    /**
     * @brief Construct histogram with explicit bucket boundaries
     *
     * @param boundaries INCLUSIVE upper bounds, one per bucket, plus an implicit overflow bucket.
     *        Values greater than the last boundary land in the overflow bucket.
     *        Example: {10, 50, 100, 500, 1000} creates 6 buckets:
     *        (-,10], (10,50], (50,100], (100,500], (500,1000], (1000,+inf)
     *
     *        The bounds are INCLUSIVE, matching the `le` ("less than or equal") convention that
     *        Prometheus and the OTLP bridge use — an observation exactly equal to a boundary lands
     *        in that boundary's bucket. This documentation used to describe exclusive bounds
     *        ([0,10) and so on), which contradicted the implementation: bucketing is a
     *        std::lower_bound, which finds the first boundary NOT LESS THAN the value, so 10.0 has
     *        always gone into the bucket labelled 10. The implementation was right and the
     *        documentation wrong, so the documentation is what changed.
     *
     * @throws std::invalid_argument if @p boundaries is not finite, non-negative and strictly
     *         increasing.
     *
     *         Validated rather than tolerated, as part of the v0.5 freeze, and for the same reason
     *         record() rejects negative and non-finite observations: a negative boundary can never
     *         be matched by a valid observation, a NaN boundary makes lower_bound's ordering
     *         requirement unmet (so bucketing becomes undefined, not merely odd), and a duplicate
     *         or out-of-order boundary produces two exported series whose `le` labels imply an
     *         ordering the bucket counts do not honour — a silently unreadable histogram
     *         downstream. Throwing at construction is a loud, immediate failure in setup code;
     *         every alternative is quiet corruption at export time.
     */
    explicit histogram(std::vector<double> boundaries)
        : m_boundaries(validated_boundaries(std::move(boundaries))),
          m_buckets(m_boundaries.size() + 1) // +1 for overflow bucket
    {}

    /**
     * @brief Generate power-of-2 bucket boundaries
     *
     * Creates boundaries: 1, 2, 4, 8, ..., 2^(n-2)
     * Use with the histogram constructor for buckets, with INCLUSIVE upper bounds (`le`), matching
     * the constructor: an observation exactly equal to a boundary lands in that boundary's bucket.
     * (-,1], (1,2], (2,4], (4,8], ..., (2^(n-2), +inf)
     *
     * @param num_buckets Number of buckets (max 64)
     * @return Vector of boundaries
     */
    static auto power_of_2_boundaries(std::size_t num_buckets = 20) -> std::vector<double> {
        if (num_buckets < 2) {
            throw std::invalid_argument("num_buckets must be at least 2");
        }
        if (num_buckets > 64) {
            throw std::invalid_argument("num_buckets must be at most 64");
        }
        std::vector<double> bounds;
        bounds.reserve(num_buckets - 1);
        for (std::size_t i = 0; i < num_buckets - 1; ++i) {
            bounds.push_back(static_cast<double>(1ULL << i));
        }
        return bounds;
    }

    /**
     * @brief Record a value in the histogram
     *
     * Observations MUST be finite and non-negative. An observation that is negative, NaN, or
     * infinite is REJECTED — it updates neither the buckets, nor count(), nor sum() — and is
     * counted in rejected_observations().
     *
     * This is a v0.5 API guarantee, not a defensive nicety, and it exists so that sum() is
     * monotonic. The OTLP bridge exports a histogram as three instruments, and its `_sum` is a
     * monotonic Counter: a sum that could decrease would be read downstream as a counter RESET,
     * making the collector add the whole new value and invent an enormous spurious rate. One NaN
     * observation is worse still — it would poison the accumulated sum permanently, since
     * NaN + x is NaN forever after.
     *
     * Stays noexcept, and rejects silently rather than throwing: this is the hot data path, called
     * from process(), and a throwing metric would turn an instrumentation bug into a component
     * fault. Check rejected_observations() to find bad instrumentation.
     *
     * @param value Value to record; finite and >= 0
     */
    void record(double value) noexcept {
        // Rejected BEFORE any field is touched, so a bad observation cannot leave count, buckets
        // and sum disagreeing with each other. `!(value >= 0.0)` rather than `value < 0.0`: NaN
        // compares false against everything, so this catches NaN in the same test.
        if (!(value >= 0.0) || std::isinf(value)) {
            m_rejected.value.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // ONE bucketing rule for all boundaries (power-of-2 boundaries are just a
        // special case), so create_histogram and create_histogram_pow2 agree on
        // which bucket a value lands in. (The old O(1) power-of-2 fast path used a
        // different rounding and disagreed with the binary-search path, e.g. 1.5.)
        auto it = std::lower_bound(m_boundaries.begin(), m_boundaries.end(), value);
        auto bucket_idx = static_cast<std::size_t>(it - m_boundaries.begin());

        // Independent per-field atomics, no seqlock. Correct under MULTIPLE
        // concurrent recorders (the old single-writer seqlock was not). A snapshot
        // taken during concurrent records is per-field-never-torn / eventually
        // consistent (what an aggregating exporter needs); a quiescent snapshot is exact.
        m_buckets[bucket_idx].value.fetch_add(1, std::memory_order_relaxed);
        m_count.value.fetch_add(1, std::memory_order_relaxed);

        uint64_t current_bits = m_sum.value.load(std::memory_order_relaxed);
        uint64_t new_bits;
        do {
            double new_sum = std::bit_cast<double>(current_bits) + value;
            new_bits = std::bit_cast<uint64_t>(new_sum);
        } while (!m_sum.value.compare_exchange_weak(current_bits, new_bits, std::memory_order_relaxed));
    }

    /**
     * @brief Observations refused by record() for being negative, NaN, or infinite.
     *
     * Non-zero means instrumentation somewhere is feeding a histogram values it cannot represent.
     * The data is not silently wrong — the rejected observations are simply absent — but the
     * caller's intent was not honoured, so this is worth alerting on.
     */
    [[nodiscard]]
    auto rejected_observations() const noexcept -> uint64_t {
        return m_rejected.value.load(std::memory_order_relaxed);
    }

    /**
     * @brief Get bucket counts
     * @return Vector of counts per bucket
     *
     * @note This may be inconsistent with count()/sum() during concurrent writes.
     *       Use snapshot() to read every field in one call — though note that snapshot() is not
     *       atomic ACROSS fields either while writers are active; see histogram_data.
     */
    [[nodiscard]]
    auto bucket_counts() const -> std::vector<uint64_t> {
        std::vector<uint64_t> counts;
        counts.reserve(m_buckets.size());
        for (const auto& bucket : m_buckets) {
            counts.push_back(bucket.value.load(std::memory_order_relaxed));
        }
        return counts;
    }

    /**
     * @brief Read all histogram fields in one call
     *
     * Preferable to the individual accessors — it reads every field once, so a caller cannot
     * accidentally interleave its own logic between them — but it is NOT atomic across fields
     * while writers are active. See histogram_data for the exact contract.
     *
     * @return Histogram data: per-field consistent always, mutually consistent when quiescent
     */
    [[nodiscard]]
    auto snapshot() const -> histogram_data {
        histogram_data data;
        data.bucket_counts.reserve(m_buckets.size());
        for (const auto& bucket : m_buckets) {
            data.bucket_counts.push_back(bucket.value.load(std::memory_order_relaxed));
        }
        data.count = m_count.value.load(std::memory_order_relaxed);
        data.sum = std::bit_cast<double>(m_sum.value.load(std::memory_order_relaxed));
        data.rejected = m_rejected.value.load(std::memory_order_relaxed);
        return data;
    }

    /**
     * @brief Get bucket boundaries
     * @return Vector of upper bounds for each bucket
     */
    [[nodiscard]]
    auto boundaries() const -> const std::vector<double>& {
        return m_boundaries;
    }

    /**
     * @brief Get total count of recorded values
     */
    [[nodiscard]]
    auto count() const noexcept -> uint64_t {
        return m_count.value.load(std::memory_order_relaxed);
    }

    /**
     * @brief Get sum of all recorded values
     */
    [[nodiscard]]
    auto sum() const noexcept -> double {
        uint64_t bits = m_sum.value.load(std::memory_order_relaxed);
        return std::bit_cast<double>(bits);
    }

    /**
     * @brief Reset histogram to initial state
     *
     * @warning Not thread-safe with concurrent record() calls.
     *          Ensure no other threads are recording during reset.
     */
    void reset() noexcept {
        for (auto& bucket : m_buckets) {
            bucket.value.store(0, std::memory_order_relaxed);
        }
        m_count.value.store(0, std::memory_order_relaxed);
        m_sum.value.store(0, std::memory_order_relaxed);
        m_rejected.value.store(0, std::memory_order_relaxed); // "initial state" means ALL of it
    }

private:
    /// Enforce the boundary contract at construction. Static so it can run in the member
    /// initializer list, before m_boundaries is built from its result.
    static auto validated_boundaries(std::vector<double> boundaries) -> std::vector<double> {
        double previous = -std::numeric_limits<double>::infinity();
        for (const auto bound : boundaries) {
            if (!std::isfinite(bound)) {
                throw std::invalid_argument("histogram boundaries must be finite (no NaN or infinity)");
            }
            if (bound < 0.0) {
                throw std::invalid_argument("histogram boundaries must be non-negative");
            }
            if (bound <= previous) {
                throw std::invalid_argument("histogram boundaries must be strictly increasing");
            }
            previous = bound;
        }
        return boundaries;
    }

    std::vector<double> m_boundaries;
    std::vector<aligned_atomic<uint64_t>> m_buckets;
    aligned_atomic<uint64_t> m_count{};
    aligned_atomic<uint64_t> m_sum{};      // Stored as bits of double
    aligned_atomic<uint64_t> m_rejected{}; // negative / NaN / infinite observations refused
};

} // namespace composite::metrics
