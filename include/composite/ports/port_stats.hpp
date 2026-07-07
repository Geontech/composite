/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "composite/metrics/metrics.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace composite {

/**
 * @brief Port statistics for monitoring and performance analysis
 *
 * Thread-safe statistics tracking for port operations. Statistics are
 * automatically exposed as metrics when register_metrics() is called
 * (typically from component::add_port()).
 *
 * Metrics are named with the pattern "composite.port.<metric_name>" and
 * include labels for component_id, port_name, and port_type.
 */
struct port_stats {
    /**
     * @brief Constructor initializes start time
     */
    port_stats() {
        auto now = std::chrono::steady_clock::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()
        ).count();
        m_start_time_ns.store(ns, std::memory_order_relaxed);
        m_last_activity_ns.store(ns, std::memory_order_relaxed);
    }

    /**
     * @brief Register metrics for this port
     *
     * Creates counter and gauge metrics in the global registry. Should be
     * called once when the port is added to a component (via add_port).
     *
     * @param component_id ID of the owning component
     * @param port_name Name of the port
     * @param port_type "input" or "output"
     */
    auto register_metrics(
        std::string_view component_id,
        std::string_view port_name,
        std::string_view port_type
    ) -> void {
        auto& registry = metrics::registry::instance();

        metrics::labels_t labels = {
            {"component_id", std::string{component_id}},
            {"port_name", std::string{port_name}},
            {"port_type", std::string{port_type}}
        };

        m_packets_transferred = &registry.get_or_create_counter(
            "composite.port.packets_transferred",
            "Number of packets successfully transferred",
            "1",
            labels
        );

        m_packets_dropped = &registry.get_or_create_counter(
            "composite.port.packets_dropped",
            "Number of packets dropped due to queue overflow",
            "1",
            labels
        );

        m_bytes_transferred = &registry.get_or_create_counter(
            "composite.port.bytes_transferred",
            "Total bytes transferred through this port",
            "By",
            labels
        );
    }

    /**
     * @brief Check if metrics have been registered
     */
    [[nodiscard]]
    auto is_registered() const -> bool {
        return m_packets_transferred != nullptr;
    }

    // ========================================================================
    // Recording methods - called from port implementations
    // ========================================================================

    /**
     * @brief Record a successful packet transfer
     * @param bytes Number of bytes transferred
     * @param packets Number of packets transferred (default 1; batch paths pass k
     *                so packets_transferred and drop_rate() stay accurate)
     */
    auto record_transfer(std::size_t bytes, std::size_t packets = 1) -> void {
        if (m_packets_transferred) {
            m_packets_transferred->add(packets);
        }
        if (m_bytes_transferred) {
            m_bytes_transferred->add(bytes);
        }
        // No clock here. The per-packet steady_clock::now() (a vDSO call on the hot
        // path, paid on every send AND every pop) is gone; last-activity is derived at
        // scrape time in time_since_last_activity() from the transfer counters instead.
    }

    /**
     * @brief Record a dropped packet
     */
    auto record_drop() -> void {
        if (m_packets_dropped) {
            m_packets_dropped->add(1);
        }
    }

    // ========================================================================
    // Value accessors - for reading current statistics
    // ========================================================================

    /**
     * @brief Get number of packets transferred
     */
    [[nodiscard]]
    auto packets_transferred() const -> uint64_t {
        return m_packets_transferred ? m_packets_transferred->value() : 0;
    }

    /**
     * @brief Get number of packets dropped
     */
    [[nodiscard]]
    auto packets_dropped() const -> uint64_t {
        return m_packets_dropped ? m_packets_dropped->value() : 0;
    }

    /**
     * @brief Get total bytes transferred
     */
    [[nodiscard]]
    auto bytes_transferred() const -> uint64_t {
        return m_bytes_transferred ? m_bytes_transferred->value() : 0;
    }

    // ========================================================================
    // Computed metrics
    // ========================================================================

    /**
     * @brief Calculate throughput in megabits per second
     * @return Current throughput based on elapsed time
     */
    [[nodiscard]]
    auto throughput_mbps() const -> double {
        auto start = m_start_time_ns.load(std::memory_order_relaxed);
        auto now = std::chrono::steady_clock::now();
        auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()
        ).count();

        auto elapsed_ns = now_ns - start;
        if (elapsed_ns <= 0) {
            return 0.0;
        }

        auto bytes = bytes_transferred();
        auto bits = bytes * 8.0;
        auto elapsed_s = elapsed_ns / 1e9;

        return (bits / 1e6) / elapsed_s; // Mbps
    }

    /**
     * @brief Calculate packet drop rate
     * @return Ratio of dropped packets to total packets (0.0 to 1.0)
     */
    [[nodiscard]]
    auto drop_rate() const -> double {
        auto dropped = packets_dropped();
        auto transferred = packets_transferred();
        auto total = dropped + transferred;

        if (total == 0) {
            return 0.0;
        }

        return static_cast<double>(dropped) / static_cast<double>(total);
    }

    /**
     * @brief Get time elapsed since last activity
     * @return Duration since the last observed send/receive operation
     *
     * Derived at SCRAPE time (not on the data path): each call samples the cumulative
     * transfer/drop counters; if they advanced since the previous call, "now" becomes the
     * last-activity instant. Resolution is therefore the scrape interval — ample for a
     * liveness diagnostic, and it keeps the per-packet hot path clock-free. The mutable
     * observation state is only touched here (the scrape path), never by record_transfer.
     */
    [[nodiscard]]
    auto time_since_last_activity() const -> std::chrono::nanoseconds {
        auto now = std::chrono::steady_clock::now();
        auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()
        ).count();

        const auto traffic = packets_transferred() + packets_dropped();
        if (traffic != m_last_seen_traffic.load(std::memory_order_relaxed)) {
            m_last_seen_traffic.store(traffic, std::memory_order_relaxed);
            m_last_activity_ns.store(now_ns, std::memory_order_relaxed);
            return std::chrono::nanoseconds{0};
        }
        return std::chrono::nanoseconds{now_ns - m_last_activity_ns.load(std::memory_order_relaxed)};
    }

    /**
     * @brief Reset statistics timestamps
     *
     * Note: Metrics are cumulative counters - they cannot be reset.
     * This only resets the local timestamps used for throughput calculation.
     */
    auto reset() -> void {
        auto now = std::chrono::steady_clock::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()
        ).count();

        m_start_time_ns.store(ns, std::memory_order_relaxed);
        m_last_activity_ns.store(ns, std::memory_order_relaxed);
        // Counters are cumulative (not reset) — baseline the observation at the current
        // total so the next scrape doesn't spuriously report activity.
        m_last_seen_traffic.store(packets_transferred() + packets_dropped(), std::memory_order_relaxed);
    }

private:
    // Metric pointers (nullptr until register_metrics() is called)
    metrics::counter<uint64_t>* m_packets_transferred{nullptr};
    metrics::counter<uint64_t>* m_packets_dropped{nullptr};
    metrics::counter<uint64_t>* m_bytes_transferred{nullptr};

    // Timestamps (local storage - not exposed as metrics). m_last_activity_ns and
    // m_last_seen_traffic are scrape-side observation state (mutable: updated only by the
    // const time_since_last_activity() diagnostic, never by the data path).
    std::atomic<int64_t> m_start_time_ns{0};
    mutable std::atomic<int64_t> m_last_activity_ns{0};
    mutable std::atomic<uint64_t> m_last_seen_traffic{0};

}; // struct port_stats

} // namespace composite
