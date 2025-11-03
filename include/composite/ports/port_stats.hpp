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
#include <chrono>
#include <cstdint>

namespace composite {

/**
 * @brief Port statistics for monitoring and performance analysis
 *
 * Thread-safe statistics tracking for port operations. All counters use
 * atomic operations to ensure correctness in multi-threaded environments.
 */
struct port_stats {
    /// Number of packets successfully sent/received
    std::atomic<uint64_t> packets_transferred{0};

    /// Number of packets dropped due to queue overflow
    std::atomic<uint64_t> packets_dropped{0};

    /// Total bytes transferred through this port
    std::atomic<uint64_t> bytes_transferred{0};

    /// Maximum queue depth observed (high-water mark)
    std::atomic<std::size_t> max_queue_depth{0};

    /// Timestamp of last activity (send/receive)
    std::atomic<int64_t> last_activity_ns{0};

    /// Timestamp when statistics were reset or port created
    std::atomic<int64_t> start_time_ns{0};

    /**
     * @brief Constructor initializes start time
     */
    port_stats() {
        auto now = std::chrono::steady_clock::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()
        ).count();
        start_time_ns.store(ns, std::memory_order_relaxed);
        last_activity_ns.store(ns, std::memory_order_relaxed);
    }

    /**
     * @brief Calculate throughput in megabits per second
     * @return Current throughput based on elapsed time
     */
    auto throughput_mbps() const -> double {
        auto start = start_time_ns.load(std::memory_order_relaxed);
        auto now = std::chrono::steady_clock::now();
        auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()
        ).count();

        auto elapsed_ns = now_ns - start;
        if (elapsed_ns <= 0) {
            return 0.0;
        }

        auto bytes = bytes_transferred.load(std::memory_order_relaxed);
        auto bits = bytes * 8.0;
        auto elapsed_s = elapsed_ns / 1e9;

        return (bits / 1e6) / elapsed_s; // Mbps
    }

    /**
     * @brief Calculate packet drop rate
     * @return Ratio of dropped packets to total packets (0.0 to 1.0)
     */
    auto drop_rate() const -> double {
        auto dropped = packets_dropped.load(std::memory_order_relaxed);
        auto transferred = packets_transferred.load(std::memory_order_relaxed);
        auto total = dropped + transferred;

        if (total == 0) {
            return 0.0;
        }

        return static_cast<double>(dropped) / static_cast<double>(total);
    }

    /**
     * @brief Get time elapsed since last activity
     * @return Duration since last send/receive operation
     */
    auto time_since_last_activity() const -> std::chrono::nanoseconds {
        auto last = last_activity_ns.load(std::memory_order_relaxed);
        auto now = std::chrono::steady_clock::now();
        auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()
        ).count();

        return std::chrono::nanoseconds{now_ns - last};
    }

    /**
     * @brief Reset all statistics counters
     */
    auto reset() -> void {
        auto now = std::chrono::steady_clock::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()
        ).count();

        packets_transferred.store(0, std::memory_order_relaxed);
        packets_dropped.store(0, std::memory_order_relaxed);
        bytes_transferred.store(0, std::memory_order_relaxed);
        max_queue_depth.store(0, std::memory_order_relaxed);
        start_time_ns.store(ns, std::memory_order_relaxed);
        last_activity_ns.store(ns, std::memory_order_relaxed);
    }

    /**
     * @brief Update last activity timestamp to now
     */
    auto update_activity_timestamp() -> void {
        auto now = std::chrono::steady_clock::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()
        ).count();
        last_activity_ns.store(ns, std::memory_order_relaxed);
    }

}; // struct port_stats

} // namespace composite
