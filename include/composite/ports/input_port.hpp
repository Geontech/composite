/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
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

#include "composite/buffers/buffer.hpp"
#include "composite/core/metadata.hpp"
#include "composite/core/timestamp.hpp"
#include "port_base.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <limits>
#include <optional>
#include <ranges>
#include <tuple>
#include <typeinfo>

namespace composite {

/**
 * @brief Tag type for blocking get_data operations
 *
 * Use this tag to request blocking behavior (waits indefinitely until data arrives).
 *
 * Example usage:
 * @code
 * auto data = input.get_data(composite::blocking);  // Blocks until data available
 * auto data = input.get_data(500ms);                // Waits up to 500ms
 * auto data = input.get_data();                     // Default 1s timeout
 * @endcode
 */
struct blocking_t {};

/**
 * @brief Constant instance of blocking_t for use with get_data()
 */
inline constexpr blocking_t blocking{};

/**
 * @brief Forward declaration of input_port template
 *
 * input_port is specialized for immutable_buffer<T> and mutable_buffer<T>.
 */
template <typename BufferType> class input_port;

/**
 * @brief Input port specialization for immutable buffers
 * @tparam T Element type (e.g., float, int, custom struct)
 *
 * Receives immutable_buffer<T> data from connected output ports. Features:
 * - Zero-copy sharing when possible (immutable → immutable)
 * - Thread-safe queue with configurable depth and backpressure
 * - Multiple get_data() variants (timeout, blocking)
 * - Statistics tracking (throughput, drops, queue depth)
 * - Metadata association with data packets
 *
 * **Thread Safety:** All public methods are thread-safe. The queue is
 * protected by a mutex, and get_data() uses condition variables for
 * efficient blocking.
 *
 * **Typical Usage:**
 * @code
 * input_port<immutable_buffer<float>> input{"samples_in"};
 * input.depth(100);  // Limit queue to 100 packets
 *
 * // In component process():
 * auto [buffer, ts, metadata] = input.get_data();
 * if (buffer) {
 *     // Process data...
 * }
 * @endcode
 */
template <typename T>
class input_port<immutable_buffer<T>> : public input_port_base {
public:
    using buffer_type = immutable_buffer<T>;  ///< Buffer type for this port
    using value_type = T;                     ///< Element type stored in buffers
    using queue_type = std::tuple<buffer_type, timestamp, std::optional<composite::metadata>>;  ///< Queue entry structure

    /**
     * @brief Inherit constructor from input_port_base
     */
    using input_port_base::input_port_base;

    /**
     * @brief Default destructor
     */
    ~input_port() override = default;

    /**
     * @brief Get type identifier for element type T
     * @return Hash code from typeid(T)
     */
    auto element_type_id() const -> std::size_t override {
        return typeid(T).hash_code();
    }

    /**
     * @brief Check if this port uses mutable buffers
     * @return false (immutable_buffer port)
     */
    auto is_mutable() const -> bool override { return false; }

    /**
     * @brief Get data with timeout (defaults to 1 second)
     * @param timeout Maximum time to wait for data (default: 1s)
     * @return Tuple of buffer, timestamp, and optional metadata
     */
    template<typename Rep = int64_t, typename Period = std::ratio<1>>
    auto get_data(std::chrono::duration<Rep, Period> timeout = std::chrono::seconds(1)) -> queue_type {
        auto lock = std::unique_lock{m_mtx};
        m_cv.wait_for(lock, timeout, [this]{ return !m_queue.empty(); });

        if (!m_queue.empty()) {
            auto result = std::move(m_queue.front());
            m_queue.pop_front();

            // Update statistics
            m_stats.packets_transferred.fetch_add(1, std::memory_order_relaxed);
            auto& [buffer, ts_val, metadata] = result;
            m_stats.bytes_transferred.fetch_add(
                buffer.size() * sizeof(T),
                std::memory_order_relaxed
            );
            m_stats.update_activity_timestamp();

            return result;
        }
        return {};
    }

    /**
     * @brief Blocking get_data (waits indefinitely)
     * @param tag Blocking tag (use composite::blocking)
     * @return Tuple of buffer, timestamp, and optional metadata
     *
     * This overload waits indefinitely until data is available, with no timeout.
     */
    auto get_data(blocking_t) -> queue_type {
        auto lock = std::unique_lock{m_mtx};
        m_cv.wait(lock, [this]{ return !m_queue.empty(); });

        auto result = std::move(m_queue.front());
        m_queue.pop_front();

        // Update statistics
        m_stats.packets_transferred.fetch_add(1, std::memory_order_relaxed);
        auto& [buffer, ts_val, metadata] = result;
        m_stats.bytes_transferred.fetch_add(
            buffer.size() * sizeof(T),
            std::memory_order_relaxed
        );
        m_stats.update_activity_timestamp();

        return result;
    }

    /**
     * @brief Get current queue size
     * @return Number of packets currently queued
     */
    auto size() const -> std::size_t {
        const auto lock = std::scoped_lock{m_mtx};
        return m_queue.size();
    }

    /**
     * @brief Clear all queued packets
     *
     * Discards all pending data. Does not affect statistics or depth settings.
     * Useful for resetting port state between runs.
     */
    auto clear() -> void {
        const auto lock = std::scoped_lock{m_mtx};
        m_queue.clear();
    }

    /**
     * @brief Check if queue is at capacity
     * @return true if size() >= depth()
     *
     * Used by output ports to implement backpressure. When full, new
     * packets will be dropped until space becomes available.
     */
    auto is_full() const -> bool override {
        const auto lock = std::scoped_lock{m_mtx};
        return m_queue.size() >= m_depth;
    }

    /**
     * @brief Get available queue capacity
     * @return Number of packets that can be queued before reaching depth limit
     *
     * Returns 0 if full, depth - size otherwise. Useful for flow control.
     */
    auto available_capacity() const -> std::size_t override {
        const auto lock = std::scoped_lock{m_mtx};
        if (m_queue.size() >= m_depth) {
            return 0;
        }
        return m_depth - m_queue.size();
    }

private:
    /**
     * @brief Friend declaration for output_port to call add_data()
     */
    template<typename> friend class output_port;

    /**
     * @brief Internal method to add data to queue (called by output_port)
     * @param data Buffer to enqueue
     * @param ts Timestamp associated with the data
     *
     * This method:
     * 1. Updates queue depth high-water mark statistics
     * 2. Enqueues data if space available, or drops if full
     * 3. Associates latched metadata with data packet
     * 4. Notifies waiting threads via condition variable
     * 5. Invokes overflow callback if packet dropped
     */
    auto add_data(buffer_type data, timestamp ts) -> void {
        const auto lock = std::scoped_lock{m_mtx};

        // Update high-water mark for queue depth statistics
        auto current_size = m_queue.size();
        auto max_depth = m_stats.max_queue_depth.load(std::memory_order_relaxed);
        while (current_size > max_depth &&
               !m_stats.max_queue_depth.compare_exchange_weak(
                   max_depth, current_size, std::memory_order_relaxed)) {
            // Retry if another thread updated max_depth concurrently
        }

        if (current_size < m_depth) {
            // Space available - enqueue packet
            auto bytes = data.size() * sizeof(T);
            m_queue.emplace_back(std::move(data), ts, m_metadata);
            m_metadata.reset();  // Clear latched metadata after use
            m_cv.notify_one();   // Wake up one waiting get_data() call
        } else {
            // Queue full - drop packet and update statistics
            m_stats.packets_dropped.fetch_add(1, std::memory_order_relaxed);

            // Invoke overflow callback if set
            if (m_overflow_callback) {
                m_overflow_callback(1);
            }
        }
    }

    std::deque<queue_type> m_queue;  ///< Thread-safe queue of data packets

}; // input_port<immutable_buffer<T>>

/**
 * @brief Input port specialization for mutable buffers
 * @tparam T Element type (e.g., float, int, custom struct)
 *
 * Receives mutable_buffer<T> data from connected output ports. Features:
 * - Exclusive ownership of buffer data (move semantics)
 * - Thread-safe queue with configurable depth and backpressure
 * - Multiple get_data() variants (timeout, blocking)
 * - Statistics tracking (throughput, drops, queue depth)
 * - Metadata association with data packets
 *
 * **Thread Safety:** All public methods are thread-safe. The queue is
 * protected by a mutex, and get_data() uses condition variables for
 * efficient blocking.
 *
 * **Transfer Semantics:** Buffers are moved into the queue. When receiving
 * from fan-out scenarios, the last receiver gets the moved buffer while
 * earlier receivers get deep copies.
 *
 * **Typical Usage:**
 * @code
 * input_port<mutable_buffer<float>> input{"samples_in"};
 * input.depth(100);  // Limit queue to 100 packets
 *
 * // In component process():
 * auto [buffer, ts, metadata] = input.get_data();
 * if (buffer) {
 *     // Modify data in-place...
 *     buffer[0] *= 2.0f;
 * }
 * @endcode
 */
template<typename T>
class input_port<mutable_buffer<T>> : public input_port_base {
public:
    using buffer_type = mutable_buffer<T>;  ///< Buffer type for this port
    using value_type = T;                   ///< Element type stored in buffers
    using queue_type = std::tuple<buffer_type, timestamp, std::optional<composite::metadata>>;  ///< Queue entry structure

    /**
     * @brief Inherit constructor from input_port_base
     */
    using input_port_base::input_port_base;

    /**
     * @brief Get type identifier for element type T
     * @return Hash code from typeid(T)
     */
    auto element_type_id() const -> std::size_t override {
        return typeid(T).hash_code();
    }

    /**
     * @brief Check if this port uses mutable buffers
     * @return true (mutable_buffer port)
     */
    auto is_mutable() const -> bool override { return true; }

    /**
     * @brief Get data with timeout (defaults to 1 second)
     * @param timeout Maximum time to wait for data (default: 1s)
     * @return Tuple of buffer, timestamp, and optional metadata
     */
    template<typename Rep = int64_t, typename Period = std::ratio<1>>
    auto get_data(std::chrono::duration<Rep, Period> timeout = std::chrono::seconds(1)) -> queue_type {
        auto lock = std::unique_lock{m_mtx};
        m_cv.wait_for(lock, timeout, [this]{ return !m_queue.empty(); });

        if (!m_queue.empty()) {
            auto result = std::move(m_queue.front());
            m_queue.pop_front();

            // Update statistics
            m_stats.packets_transferred.fetch_add(1, std::memory_order_relaxed);
            auto& [buffer, ts_val, metadata] = result;
            m_stats.bytes_transferred.fetch_add(
                buffer.size() * sizeof(T),
                std::memory_order_relaxed
            );
            m_stats.update_activity_timestamp();

            return result;
        }
        return {};
    }

    /**
     * @brief Blocking get_data (waits indefinitely)
     * @param tag Blocking tag (use composite::blocking)
     * @return Tuple of buffer, timestamp, and optional metadata
     *
     * This overload waits indefinitely until data is available, with no timeout.
     */
    auto get_data(blocking_t) -> queue_type {
        auto lock = std::unique_lock{m_mtx};
        m_cv.wait(lock, [this]{ return !m_queue.empty(); });

        auto result = std::move(m_queue.front());
        m_queue.pop_front();

        // Update statistics
        m_stats.packets_transferred.fetch_add(1, std::memory_order_relaxed);
        auto& [buffer, ts_val, metadata] = result;
        m_stats.bytes_transferred.fetch_add(
            buffer.size() * sizeof(T),
            std::memory_order_relaxed
        );
        m_stats.update_activity_timestamp();

        return result;
    }

    /**
     * @brief Get current queue size
     * @return Number of packets currently queued
     */
    auto size() const -> std::size_t {
        const auto lock = std::scoped_lock{m_mtx};
        return m_queue.size();
    }

    /**
     * @brief Clear all queued packets
     *
     * Discards all pending data. Does not affect statistics or depth settings.
     * Useful for resetting port state between runs.
     */
    auto clear() -> void {
        const auto lock = std::scoped_lock{m_mtx};
        m_queue.clear();
    }

    /**
     * @brief Check if queue is at capacity
     * @return true if size() >= depth()
     *
     * Used by output ports to implement backpressure. When full, new
     * packets will be dropped until space becomes available.
     */
    auto is_full() const -> bool override {
        const auto lock = std::scoped_lock{m_mtx};
        return m_queue.size() >= m_depth;
    }

    /**
     * @brief Get available queue capacity
     * @return Number of packets that can be queued before reaching depth limit
     *
     * Returns 0 if full, depth - size otherwise. Useful for flow control.
     */
    auto available_capacity() const -> std::size_t override {
        const auto lock = std::scoped_lock{m_mtx};
        if (m_queue.size() >= m_depth) {
            return 0;
        }
        return m_depth - m_queue.size();
    }

private:
    /**
     * @brief Friend declaration for output_port to call add_data()
     */
    template<typename> friend class output_port;

    /**
     * @brief Internal method to add data to queue (called by output_port)
     * @param data Buffer to enqueue
     * @param ts Timestamp associated with the data
     *
     * This method:
     * 1. Updates queue depth high-water mark statistics
     * 2. Enqueues data if space available, or drops if full
     * 3. Associates latched metadata with data packet
     * 4. Notifies waiting threads via condition variable
     * 5. Invokes overflow callback if packet dropped
     */
    auto add_data(buffer_type data, timestamp ts) -> void {
        const auto lock = std::scoped_lock{m_mtx};

        // Update high-water mark for queue depth statistics
        auto current_size = m_queue.size();
        auto max_depth = m_stats.max_queue_depth.load(std::memory_order_relaxed);
        while (current_size > max_depth &&
               !m_stats.max_queue_depth.compare_exchange_weak(
                   max_depth, current_size, std::memory_order_relaxed)) {
            // Retry if another thread updated max_depth concurrently
        }

        if (current_size < m_depth) {
            // Space available - enqueue packet
            auto bytes = data.size() * sizeof(T);
            m_queue.emplace_back(std::move(data), ts, m_metadata);
            m_metadata.reset();  // Clear latched metadata after use
            m_cv.notify_one();   // Wake up one waiting get_data() call
        } else {
            // Queue full - drop packet and update statistics
            m_stats.packets_dropped.fetch_add(1, std::memory_order_relaxed);

            // Invoke overflow callback if set
            if (m_overflow_callback) {
                m_overflow_callback(1);
            }
        }
    }

    std::deque<queue_type> m_queue;  ///< Thread-safe queue of data packets

}; // input_port<mutable_buffer<T>>

} // namespace composite