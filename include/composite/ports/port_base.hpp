/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "composite/core/metadata.hpp"
#include "composite/transports/transport.hpp"
#include "port_stats.hpp"

#include <algorithm>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

namespace composite {

/**
 * @brief Abstract base class for all port types
 *
 * Provides common interface for both input and output ports, including:
 * - Port naming
 * - Type identification via type_id hash
 * - Mutability query (immutable vs mutable buffers)
 *
 * This base class enables polymorphic handling of ports in the component system.
 */
class port_base {
public:
    /**
     * @brief Construct a port with a name
     * @param name Port name (used for identification and connection)
     */
    explicit port_base(std::string_view name) : m_name(name) {}

    /**
     * @brief Virtual destructor for polymorphic deletion
     */
    virtual ~port_base() = default;

    /**
     * @brief Get the port name
     * @return Port name as string_view
     */
    auto name() const -> std::string_view { return m_name; }

    /**
     * @brief Get type index for the element type (T in buffer<T>)
     * @return std::type_index for compile-time type information
     *
     * Used for transformation lookup and diagnostic output.
     */
    virtual auto element_type() const -> std::type_index = 0;

    /**
     * @brief Get type identifier for the element type (T in buffer<T>)
     * @return Hash code from typeid(T).hash_code()
     *
     * Used for type safety - ensures connections are between compatible types.
     */
    virtual auto element_type_id() const -> std::size_t = 0;

    /**
     * @brief Check if this port uses mutable buffers
     * @return true for mutable_buffer ports, false for immutable_buffer ports
     *
     * Used to determine optimal transfer strategy (move vs share).
     */
    virtual auto is_mutable() const -> bool = 0;

protected:
    std::string m_name;  ///< Port name for identification
};

/**
 * @brief Base class for input ports with queue management and statistics
 *
 * Provides common functionality for all input port specializations:
 * - Thread-safe queue with configurable depth
 * - Backpressure via is_full() and available_capacity()
 * - Statistics tracking (packets, bytes, throughput)
 * - Metadata latching for stream properties
 * - Overflow handling with callbacks
 *
 * Templated specializations (input_port<immutable_buffer<T>> and
 * input_port<mutable_buffer<T>>) extend this with type-specific functionality.
 */
class input_port_base : public port_base {
public:
    /**
     * @brief Callback invoked when packets are dropped due to queue overflow
     * @param dropped_count Number of packets dropped in this overflow event
     *
     * Useful for logging, alerting, or implementing backoff strategies.
     */
    using overflow_callback = std::function<void(std::size_t dropped_count)>;

    /**
     * @brief Inherit port_base constructor
     */
    using port_base::port_base;

    /**
     * @brief Destructor - notifies all waiting threads on condition variable
     *
     * Ensures any threads blocked in get_data() are woken up during shutdown.
     */
    ~input_port_base() noexcept override {
        m_cv.notify_all();
    }

    /**
     * @brief Get the queue depth limit
     * @return Maximum number of packets that can be queued
     *
     * Default is unbounded (std::numeric_limits<std::size_t>::max()).
     * When queue reaches this limit, new packets are dropped and overflow
     * callbacks are invoked.
     */
    auto depth() const -> std::size_t {
        const auto lock = std::scoped_lock{m_mtx};
        return m_depth;
    }

    /**
     * @brief Set the queue depth limit
     * @param value Maximum queue capacity (0 = disabled port)
     *
     * Setting depth to 0 effectively disables the port (all packets dropped).
     * Use std::numeric_limits<std::size_t>::max() for unbounded.
     */
    auto depth(std::size_t value) -> void {
        const auto lock = std::scoped_lock{m_mtx};
        m_depth = value;
    }

    /**
     * @brief Latch metadata for the next data packet
     * @param md Metadata to associate with next received data
     *
     * Called internally by output_port::send_metadata(). The metadata
     * is stored until the next data packet arrives, at which point it's
     * packaged together in the queue tuple.
     */
    auto metadata(const composite::metadata& md) -> void {
        const auto lock = std::scoped_lock{m_mtx};
        m_metadata = md;
    }

    /**
     * @brief Get port statistics
     * @return Reference to port statistics structure
     */
    auto stats() const -> const port_stats& {
        return m_stats;
    }

    /**
     * @brief Reset statistics counters
     */
    auto reset_stats() -> void {
        m_stats.reset();
    }

    /**
     * @brief Check if port queue is full
     * @return true if queue is at capacity
     */
    virtual auto is_full() const -> bool = 0;

    /**
     * @brief Get available capacity in queue
     * @return Number of packets that can be queued before reaching depth limit
     */
    virtual auto available_capacity() const -> std::size_t = 0;

    /**
     * @brief Set overflow callback for dropped packets
     * @param callback Function to call when packets are dropped
     */
    auto set_overflow_callback(overflow_callback callback) -> void {
        const auto lock = std::scoped_lock{m_mtx};
        m_overflow_callback = std::move(callback);
    }

protected:
    mutable std::mutex m_mtx;                                      ///< Protects queue and metadata
    std::condition_variable m_cv;                                  ///< Signals data availability for get_data()
    std::optional<composite::metadata> m_metadata;                 ///< Latched metadata for next packet
    std::size_t m_depth{std::numeric_limits<std::size_t>::max()};  ///< Queue depth limit (unbounded by default)
    mutable port_stats m_stats;                                    ///< Statistics tracking
    overflow_callback m_overflow_callback;                         ///< Callback for dropped packets

}; // class input_port_base

/**
 * @brief Base class for output ports with connection management
 *
 * Provides common functionality for all output port specializations:
 * - Connection management (connect, disconnect, query)
 * - Fan-out support (one output → many inputs)
 * - Metadata broadcasting to all connected inputs
 * - Statistics tracking
 * - Backpressure checking via can_send()
 *
 * Templated specializations (output_port<immutable_buffer<T>> and
 * output_port<mutable_buffer<T>>) extend this with type-specific data transfer.
 */
class output_port_base : public port_base {
public:
    /**
     * @brief Inherit port_base constructor
     */
    using port_base::port_base;

    /**
     * @brief Virtual destructor
     */
    ~output_port_base() override = default;

    /**
     * @brief Connect this output to an input port
     * @param port Pointer to the input port to connect to
     *
     * Adds the input port to the list of connected ports. Data sent through
     * this output will be delivered to all connected inputs. Supports fan-out
     * (multiple connections).
     *
     * **Note:** Type safety is enforced in the templated component::connect()
     * method, not at this base class level.
     */
    auto connect(input_port_base* port) -> void {
        const auto lock = std::scoped_lock{m_connection_mtx};
        m_connected_ports.emplace_back(port);
    }

    /**
     * @brief Disconnect from a specific input port
     *
     * Removes the connection to the specified input port. If the port is not
     * connected, this is a no-op.
     *
     * @param port Pointer to the input port to disconnect
     * @return true if port was connected and is now disconnected, false if wasn't connected
     */
    auto disconnect(input_port_base* port) -> bool {
        const auto lock = std::scoped_lock{m_connection_mtx};
        auto it = std::find(m_connected_ports.begin(), m_connected_ports.end(), port);
        if (it != m_connected_ports.end()) {
            m_connected_ports.erase(it);
            return true;
        }
        return false;
    }

    /**
     * @brief Disconnect from all connected input ports
     *
     * @return Number of ports that were disconnected
     */
    auto disconnect() -> std::size_t {
        const auto lock = std::scoped_lock{m_connection_mtx};
        auto count = m_connected_ports.size();
        m_connected_ports.clear();
        return count;
    }

    /**
     * @brief Check if this output is connected to any input
     *
     * @return true if at least one input port is connected
     */
    auto is_connected() const -> bool {
        const auto lock = std::scoped_lock{m_connection_mtx};
        return !m_connected_ports.empty();
    }

    /**
     * @brief Check if this output is connected to a specific input port
     *
     * @param port Pointer to the input port to check
     * @return true if connected to the specified port
     */
    auto is_connected_to(const input_port_base* port) const -> bool {
        const auto lock = std::scoped_lock{m_connection_mtx};
        return std::find(m_connected_ports.begin(), m_connected_ports.end(), port)
               != m_connected_ports.end();
    }

    /**
     * @brief Get the number of connected input ports
     *
     * @return Number of active connections
     */
    auto connection_count() const -> std::size_t {
        const auto lock = std::scoped_lock{m_connection_mtx};
        return m_connected_ports.size();
    }

    /**
     * @brief Get list of connected input port names
     *
     * Useful for debugging and introspection.
     *
     * @return Vector of port names (component_id:port_name)
     */
    auto connected_ports() const -> std::vector<std::string> {
        const auto lock = std::scoped_lock{m_connection_mtx};
        std::vector<std::string> names;
        names.reserve(m_connected_ports.size());
        for (const auto& port : m_connected_ports) {
            if (port != nullptr) {
                names.emplace_back(port->name());  // Convert string_view to string
            }
        }
        return names;
    }

    /**
     * @brief Send metadata to all connected input ports
     *
     * Metadata is "latched" by input ports and will be associated with the next
     * data packet that arrives. This allows metadata to describe properties of
     * the upcoming data stream.
     *
     * @param value The metadata to send
     *
     * **Usage Rules:**
     * 1. Metadata must be sent BEFORE the corresponding data packet
     * 2. Metadata is consumed (reset) after being packaged with data
     * 3. Sending metadata without subsequent data will result in the metadata
     *    being discarded when the next data arrives
     * 4. Multiple metadata sends before data will use only the last one
     *
     * **Example:**
     * @code
     * // Correct usage:
     * metadata md;
     * md.sample_rate = 1e6;
     * output.send_metadata(md);      // Send metadata first
     * output.send_data(buffer, ts);  // Then send data
     *
     * // Incorrect - metadata after data:
     * output.send_data(buffer, ts);  // Data without metadata
     * output.send_metadata(md);      // This will apply to NEXT packet
     * @endcode
     */
    auto send_metadata(const composite::metadata& value) const -> void {
        const auto lock = std::scoped_lock{m_connection_mtx};
        for (const auto& port : m_connected_ports) {
            if (port != nullptr) {
                port->metadata(value);
            }
        }
    }

    /**
     * @brief Get port statistics
     * @return Reference to port statistics structure
     */
    auto stats() const -> const port_stats& {
        return m_stats;
    }

    /**
     * @brief Reset statistics counters
     */
    auto reset_stats() -> void {
        m_stats.reset();
    }

    /**
     * @brief Check if any connected input port can accept data
     * @return true if at least one connected port is not full
     */
    auto can_send() const -> bool {
        const auto lock = std::scoped_lock{m_connection_mtx};
        for (const auto& port : m_connected_ports) {
            if (port != nullptr && !port->is_full()) {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Add a transport for external publishing
     * @param transport Unique pointer to transport implementation
     *
     * Allows this output port to publish data to external systems (NATS,
     * ZeroMQ, UDP, etc.) in addition to connected input ports. Multiple
     * transports can be added to publish to different destinations.
     *
     * **Example:**
     * @code
     * auto nats = std::make_unique<nats_transport>("nats://localhost:4222", "topic");
     * output.add_transport(std::move(nats));
     * @endcode
     */
    auto add_transport(std::unique_ptr<transport_base> transport) -> void {
        const auto lock = std::scoped_lock{m_transport_mtx};
        m_transports.push_back(std::move(transport));
    }

    /**
     * @brief Remove all transports
     *
     * Clears all external transport connections. Useful for reconfiguration.
     */
    auto clear_transports() -> void {
        const auto lock = std::scoped_lock{m_transport_mtx};
        m_transports.clear();
    }

    /**
     * @brief Get number of attached transports
     * @return Count of transports
     */
    auto transport_count() const -> std::size_t {
        const auto lock = std::scoped_lock{m_transport_mtx};
        return m_transports.size();
    }

    /**
     * @brief Get list of transport endpoints
     * @return Vector of endpoint strings for debugging/introspection
     */
    auto transport_endpoints() const -> std::vector<std::string> {
        const auto lock = std::scoped_lock{m_transport_mtx};
        std::vector<std::string> endpoints;
        endpoints.reserve(m_transports.size());
        for (const auto& transport : m_transports) {
            if (transport) {
                endpoints.push_back(transport->endpoint());
            }
        }
        return endpoints;
    }

protected:
    std::vector<input_port_base*> m_connected_ports;            ///< List of connected input ports (fan-out)
    mutable std::mutex m_connection_mtx;                        ///< Protects m_connected_ports access
    mutable port_stats m_stats;                                 ///< Statistics tracking (bytes, packets, throughput)
    std::vector<std::unique_ptr<transport_base>> m_transports;  ///< External transports (NATS, etc.)
    mutable std::mutex m_transport_mtx;                         ///< Protects m_transports access

}; // class output_port_base

} // namespace composite