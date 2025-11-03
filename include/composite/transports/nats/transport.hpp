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

#ifdef COMPOSITE_USE_NATS

#include "client.hpp"
#include "composite/transports/transport.hpp"

#include <atomic>
#include <format>

namespace composite::nats {

/**
 * @brief NATS transport implementation
 *
 * Publishes data to a NATS subject as raw byte buffers. Features:
 * - Automatic connection management
 * - Statistics tracking (packets, bytes, failures)
 * - Thread-safe send operations
 * - Reconnection support
 *
 * **Thread Safety:** send() is thread-safe (NATS client handles locking).
 * Statistics are atomic.
 *
 * **Usage:**
 * @code
 * auto transport = std::make_unique<nats::transport>(
 *     "nats://localhost:4222",
 *     "sensors.temperature"
 * );
 *
 * if (transport->is_connected()) {
 *     transport->send(byte_data, timestamp::now());
 * }
 * @endcode
 */
class transport : public composite::transport_base {
public:
    /**
     * @brief Construct NATS transport with connection parameters
     * @param url NATS server URL (e.g., "nats://localhost:4222")
     * @param subject Subject name to publish on
     * @throws std::runtime_error if connection fails
     */
    transport(std::string url, std::string subject)
        : m_url(std::move(url))
        , m_subject(std::move(subject))
        , m_client(m_url) {
        if (!m_client.is_connected()) {
            throw std::runtime_error(
                std::format("Failed to connect to NATS server: {}", m_url)
            );
        }
    }

    /**
     * @brief Destructor - closes NATS connection
     */
    ~transport() override = default;

    /**
     * @brief Send data to NATS subject
     * @param data Raw bytes to publish
     * @param ts Timestamp (currently unused, for future metadata)
     * @return true if publish succeeded
     *
     * Publishes data as-is to the configured NATS subject. The receiver
     * is responsible for deserializing the bytes appropriately.
     */
    auto send(std::span<const std::byte> data, [[maybe_unused]] timestamp ts) -> bool override {
        auto status = m_client.publish(m_subject, data);

        if (status == NATS_OK) {
            m_packets_sent.fetch_add(1, std::memory_order_relaxed);
            m_bytes_sent.fetch_add(data.size(), std::memory_order_relaxed);
            return true;
        } else {
            m_send_failures.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    /**
     * @brief Check if connected to NATS server
     * @return true if connection is active
     */
    auto is_connected() const -> bool override {
        return m_client.is_connected();
    }

    /**
     * @brief Get transport type identifier
     * @return transport_type::nats
     */
    auto type() const -> transport_type override {
        return transport_type::nats;
    }

    /**
     * @brief Get full NATS endpoint string
     * @return "{url}/{subject}" (e.g., "nats://localhost:4222/sensors.temp")
     */
    auto endpoint() const -> std::string override {
        return std::format("{}/{}", m_url, m_subject);
    }

    /**
     * @brief Get packets successfully sent
     * @return Packet count
     */
    auto packets_sent() const -> std::size_t override {
        return m_packets_sent.load(std::memory_order_relaxed);
    }

    /**
     * @brief Get bytes successfully sent
     * @return Byte count
     */
    auto bytes_sent() const -> std::size_t override {
        return m_bytes_sent.load(std::memory_order_relaxed);
    }

    /**
     * @brief Get number of send failures
     * @return Failure count
     */
    auto send_failures() const -> std::size_t override {
        return m_send_failures.load(std::memory_order_relaxed);
    }

    /**
     * @brief Reset statistics counters
     */
    auto reset_stats() -> void override {
        m_packets_sent.store(0, std::memory_order_relaxed);
        m_bytes_sent.store(0, std::memory_order_relaxed);
        m_send_failures.store(0, std::memory_order_relaxed);
    }

    /**
     * @brief Get the NATS subject being published to
     * @return Subject string
     */
    auto subject() const -> const std::string& {
        return m_subject;
    }

    /**
     * @brief Get the NATS server URL
     * @return URL string
     */
    auto url() const -> const std::string& {
        return m_url;
    }

private:
    std::string m_url;                              ///< NATS server URL
    std::string m_subject;                          ///< Subject to publish on
    nats::client m_client;                          ///< NATS client connection
    std::atomic<std::size_t> m_packets_sent{0};     ///< Packets successfully sent
    std::atomic<std::size_t> m_bytes_sent{0};       ///< Bytes successfully sent
    std::atomic<std::size_t> m_send_failures{0};    ///< Failed send attempts

}; // class transport

} // namespace composite::nats

#endif // COMPOSITE_USE_NATS
