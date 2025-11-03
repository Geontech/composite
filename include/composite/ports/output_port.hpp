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

#include "composite/core/timestamp.hpp"
#include "composite/transports/transport.hpp"
#include "input_port.hpp"
#include "port_base.hpp"

#include <ranges>
#include <string_view>
#include <typeinfo>
#include <vector>

namespace composite {

/**
 * @brief Forward declaration of output_port template
 *
 * output_port is specialized for immutable_buffer<T> and mutable_buffer<T>.
 */
template <typename BufferType> class output_port;

/**
 * @brief Output port specialization for immutable buffers
 * @tparam T Element type (e.g., float, int, custom struct)
 *
 * Sends immutable_buffer<T> data to connected input ports. Features:
 * - Zero-copy sharing to immutable input ports (via shared_ptr)
 * - Deep copy when sending to mutable input ports
 * - Fan-out support (one output → many inputs)
 * - Statistics tracking (throughput, packets sent)
 * - Metadata broadcasting via send_metadata()
 *
 * **Transfer Optimization:**
 * - immutable → immutable: Zero-copy share (fast)
 * - immutable → mutable: Deep copy (required for exclusive ownership)
 *
 * **Thread Safety:** All public methods are thread-safe. The connection
 * list is protected by a mutex during send operations.
 *
 * **Typical Usage:**
 * @code
 * output_port<immutable_buffer<float>> output{"samples_out"};
 *
 * // In component process():
 * auto buffer = make_immutable<float>(1024);
 * // ... fill buffer ...
 * output.send_data(std::move(buffer), timestamp::now());
 * @endcode
 */
template<typename T>
class output_port<immutable_buffer<T>> : public output_port_base {
public:
    using buffer_type = immutable_buffer<T>;  ///< Buffer type for this port
    using value_type = T;                     ///< Element type stored in buffers

    /**
     * @brief Inherit constructor from output_port_base
     */
    using output_port_base::output_port_base;

    /**
     * @brief Default destructor
     */
    ~output_port() override = default;

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
     * @brief Send data to all connected input ports
     * @param buffer Immutable buffer to send
     * @param ts Timestamp associated with the data
     *
     * Delivers buffer to all connected input ports with optimal transfer strategy:
     * - For immutable inputs: Zero-copy share via buffer.share()
     * - For mutable inputs: Deep copy to new mutable_buffer
     *
     * Updates statistics (packets, bytes, throughput) and checks queue capacity.
     * If an input port is full, the packet is dropped at that port (not here).
     */
    auto send_data(buffer_type buffer, timestamp ts) -> void {
        // Update outgoing statistics
        auto bytes = buffer.size() * sizeof(T);
        m_stats.packets_transferred.fetch_add(1, std::memory_order_relaxed);
        m_stats.bytes_transferred.fetch_add(bytes, std::memory_order_relaxed);
        m_stats.update_activity_timestamp();

        // Lock connection list for thread-safe access
        const auto lock = std::scoped_lock{m_connection_mtx};

        for (auto* port_base : m_connected_ports) {
            if (port_base == nullptr) { continue; };

            if (port_base->is_mutable()) {
                // immutable → mutable: Must perform deep copy
                auto* mutable_port = static_cast<input_port<mutable_buffer<T>>*>(port_base);

                // Create mutable copy from immutable data
                auto vec = std::make_unique<std::vector<T>>(buffer.begin(), buffer.end());
                mutable_port->add_data(mutable_buffer<T>{std::move(vec)}, ts);
            } else {
                // immutable → immutable: Zero-copy share (optimal)
                auto* immutable_port = static_cast<input_port<immutable_buffer<T>>*>(port_base);
                immutable_port->add_data(buffer.share(), ts);
            }
        }

        // Send to external transports
        if (!m_transports.empty()) {
            send_to_transports(buffer.as_span(), ts);
        }
    }

private:
    /**
     * @brief Send data to all attached transports
     * @param data Span of buffer data
     * @param ts Timestamp
     *
     * Sends data as raw bytes to all configured transports (NATS, etc.).
     * Failed sends are tracked in transport statistics but don't affect
     * port-to-port delivery.
     */
    auto send_to_transports(std::span<const T> data, timestamp ts) -> void {
        // Convert to byte span
        auto byte_span = std::as_bytes(data);

        const auto lock = std::scoped_lock{m_transport_mtx};
        for (auto& transport : m_transports) {
            if (transport && transport->is_connected()) {
                transport->send(byte_span, ts);
            }
        }
    }

}; // class output_port<immutable_buffer<T>>

/**
 * @brief Output port specialization for mutable buffers
 * @tparam T Element type (e.g., float, int, custom struct)
 *
 * Sends mutable_buffer<T> data to connected input ports. Features:
 * - Move semantics for single connections (zero-copy, optimal)
 * - Smart fan-out: copies to all but last receiver, move to last
 * - Automatic promotion to immutable when sending to immutable inputs
 * - Fan-out support (one output → many inputs)
 * - Statistics tracking (throughput, packets sent)
 *
 * **Transfer Optimization (Single Output):**
 * - mutable → mutable: Move (optimal, zero-copy)
 * - mutable → immutable: Promote to immutable via to_immutable()
 *
 * **Transfer Optimization (Fan-Out):**
 * - First N-1 receivers: Deep copy (required for independent ownership)
 * - Last receiver: Move original buffer (saves one copy)
 *
 * **Thread Safety:** All public methods are thread-safe. The connection
 * list is protected by a mutex during send operations.
 *
 * **Typical Usage:**
 * @code
 * output_port<mutable_buffer<float>> output{"samples_out"};
 *
 * // In component process():
 * auto buffer = make_mutable<float>(1024);
 * // ... fill buffer ...
 * output.send_data(std::move(buffer), timestamp::now());
 * @endcode
 */
template<typename T>
class output_port<mutable_buffer<T>> : public output_port_base {
public:
    using buffer_type = mutable_buffer<T>;  ///< Buffer type for this port
    using value_type = T;                   ///< Element type stored in buffers

    /**
     * @brief Inherit constructor from output_port_base
     */
    using output_port_base::output_port_base;

    /**
     * @brief Default destructor
     */
    ~output_port() override = default;

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
     * @brief Send mutable buffer to connected input ports
     * @param buffer Mutable buffer to send (consumed via move)
     * @param ts Timestamp associated with the data
     *
     * Implements optimized fan-out strategy:
     *
     * **Single Connection:**
     * - mutable → mutable: Move buffer directly (zero-copy)
     * - mutable → immutable: Promote to immutable via to_immutable()
     *
     * **Multiple Connections (Fan-Out):**
     * - First N-1 connections: Deep copy via buffer.copy()
     * - Last connection: Move original buffer (saves one copy)
     * - Automatic type conversion (mutable ↔ immutable) as needed
     *
     * This strategy minimizes copies while maintaining correctness -
     * each receiver gets independent ownership of the data.
     */
    auto send_data(buffer_type buffer, timestamp ts) -> void {
        // Update statistics
        auto bytes = buffer.size() * sizeof(T);
        m_stats.packets_transferred.fetch_add(1, std::memory_order_relaxed);
        m_stats.bytes_transferred.fetch_add(bytes, std::memory_order_relaxed);
        m_stats.update_activity_timestamp();

        // Send to external transports FIRST (before buffer is moved)
        if (!m_transports.empty()) {
            send_to_transports(buffer.as_span(), ts);
        }

        // Lock for connection access
        const auto lock = std::scoped_lock{m_connection_mtx};

        if (m_connected_ports.empty()) { return; };

        if (m_connected_ports.size() == 1) {
            auto* port_base = m_connected_ports.at(0);
            if (port_base == nullptr) { return; };

            if (port_base->is_mutable()) {
                // Mutable to mutable: direct move
                auto* mutable_port = static_cast<input_port<mutable_buffer<T>>*>(port_base);
                mutable_port->add_data(std::move(buffer), ts);
            } else {
                // Mutable to immutable: promote
                auto* immutable_port = static_cast<input_port<immutable_buffer<T>>*>(port_base);
                immutable_port->add_data(std::move(buffer).to_immutable(), ts);
            }
        } else {
            // Fan-out: handle multiple outputs
            for (std::size_t i = 0; i < m_connected_ports.size() - 1; ++i) {
                auto* port_base = m_connected_ports.at(i);
                if (port_base == nullptr) { continue; };

                if (port_base->is_mutable()) {
                    auto* mutable_port = static_cast<input_port<mutable_buffer<T>>*>(port_base);
                    mutable_port->add_data(buffer.copy(), ts);
                } else {
                    auto* immutable_port = static_cast<input_port<immutable_buffer<T>>*>(port_base);
                    // Convert copy to immutable
                    immutable_port->add_data(buffer.copy().to_immutable(), ts);
                }
            }

            // Last output: move
            auto* last_port = m_connected_ports.back();
            if (last_port != nullptr) {
                if (last_port->is_mutable()) {
                    auto* mutable_port = static_cast<input_port<mutable_buffer<T>>*>(last_port);
                    mutable_port->add_data(std::move(buffer), ts);
                } else {
                    auto* immutable_port = static_cast<input_port<immutable_buffer<T>>*>(last_port);
                    immutable_port->add_data(std::move(buffer).to_immutable(), ts);
                }
            }
        }
    }

private:
    /**
     * @brief Send data to all attached transports
     * @param data Span of buffer data
     * @param ts Timestamp
     *
     * Sends data as raw bytes to all configured transports (NATS, etc.).
     * Failed sends are tracked in transport statistics but don't affect
     * port-to-port delivery.
     */
    auto send_to_transports(std::span<const T> data, timestamp ts) -> void {
        // Convert to byte span
        auto byte_span = std::as_bytes(data);

        const auto lock = std::scoped_lock{m_transport_mtx};
        for (auto& transport : m_transports) {
            if (transport && transport->is_connected()) {
                transport->send(byte_span, ts);
            }
        }
    }

}; // output_port<mutable_buffer<T>>

} // namespace composite
