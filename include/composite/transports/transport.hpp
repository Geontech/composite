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

#include "composite/core/timestamp.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace composite {

/**
 * @brief Enumeration of supported transport types
 *
 * Add new transport types here as they are implemented.
 */
enum class transport_type {
    nats,      ///< NATS messaging system
    // zeromq,    ///< ZeroMQ messaging (future)
    // udp,       ///< UDP transport (future)
    // tcp,       ///< TCP transport (future)
    // websocket  ///< WebSocket transport (future)
};

/**
 * @brief Convert transport_type enum to string representation
 * @param type The transport type enum value
 * @return String view of the transport type name
 */
constexpr auto to_string(transport_type type) -> std::string_view {
    switch (type) {
        case transport_type::nats:      return "nats";
        // case transport_type::zeromq:    return "zeromq"; (future)
        // case transport_type::udp:       return "udp"; (future)
        // case transport_type::tcp:       return "tcp"; (future)
        // case transport_type::websocket: return "websocket"; (future)
    }
    return "unknown";
}

/**
 * @brief Convert string to transport_type enum
 * @param str String representation of transport type
 * @return Optional containing transport_type if valid, nullopt otherwise
 */
inline auto from_string(std::string_view str) -> std::optional<transport_type> {
    if (str == "nats") { return transport_type::nats; }
    // if (str == "zeromq") { return transport_type::zeromq; } (future)
    // if (str == "udp") { return transport_type::udp; } (future)
    // if (str == "tcp") { return transport_type::tcp; } (future)
    // if (str == "websocket") { return transport_type::websocket; } (future)
    return std::nullopt;
}

/**
 * @brief Abstract base class for transport implementations
 *
 * Defines the interface for sending data over various transport protocols
 * (NATS, ZeroMQ, UDP, TCP, etc.). Output ports can attach transports to
 * publish data externally.
 *
 * **Design Goals:**
 * - Protocol-agnostic: Works with raw bytes
 * - Extensible: Easy to add new transports
 * - Statistics: Track send success/failure
 * - Connection management: Each transport handles its own lifecycle
 *
 * **Typical Usage:**
 * @code
 * // In application setup:
 * auto nats = std::make_unique<nats::transport>("nats://localhost:4222", "my.subject");
 * output_port.add_transport(std::move(nats));
 *
 * // In component process():
 * output_port.send_data(buffer, ts);  // Sends to both ports AND transports
 * @endcode
 */
class transport_base {
public:
    /**
     * @brief Virtual destructor for polymorphic deletion
     */
    virtual ~transport_base() = default;

    /**
     * @brief Send data over the transport
     * @param data Raw byte span of the data to send
     * @param ts Timestamp associated with the data
     * @return true if send succeeded, false otherwise
     *
     * Implementations should handle:
     * - Connection state checking
     * - Serialization if needed
     * - Error handling and recovery
     * - Statistics updates
     */
    virtual auto send(std::span<const std::byte> data, timestamp ts) -> bool = 0;

    /**
     * @brief Check if transport is connected and ready to send
     * @return true if transport can accept data
     */
    virtual auto is_connected() const -> bool = 0;

    /**
     * @brief Get transport type identifier
     * @return Transport type enum value
     *
     * Use composite::to_string(type()) to get the string representation.
     */
    virtual auto type() const -> transport_type = 0;

    /**
     * @brief Get transport-specific endpoint/address
     * @return String describing the endpoint (e.g., "nats://localhost:4222/subject")
     */
    virtual auto endpoint() const -> std::string = 0;

    /**
     * @brief Get number of packets successfully sent
     * @return Packet count
     */
    virtual auto packets_sent() const -> std::size_t = 0;

    /**
     * @brief Get number of bytes successfully sent
     * @return Byte count
     */
    virtual auto bytes_sent() const -> std::size_t = 0;

    /**
     * @brief Get number of send failures
     * @return Failure count
     */
    virtual auto send_failures() const -> std::size_t = 0;

    /**
     * @brief Reset statistics counters
     */
    virtual auto reset_stats() -> void = 0;

}; // class transport_base

} // namespace composite
