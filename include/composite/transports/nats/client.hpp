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

#include <format>
#include <map>
#include <memory>
#include <nats/nats.h>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include "message.hpp"
#include "subscription.hpp"

namespace composite::nats {

class client {
public:
    /**
     * @brief Default constructor
     */
    client() = default;

    /**
     * @brief URL constructor
     * @param url The NATS server URL (e.g., "nats://localhost:4222")
     * @throws std::runtime_error if connection fails
     */
    explicit client(const std::string& url) {
        auto status = natsConnection_ConnectTo(&m_connection, url.c_str());
        if (status != NATS_OK) {
            throw std::runtime_error(std::format(
              "Failed to connect to NATS server {}: {} ({})",
              url, natsStatus_GetText(status), static_cast<int>(status)
            ));
        }
    }
    
    /**
     * @brief Destructor
     */
    ~client() {
        if (m_connection != nullptr) {
            natsConnection_Destroy(m_connection);
        }
    }

    /**
     * @brief Deleted copy constructor (exclusive ownership of connection)
     */
    client(const client&) = delete;

    /**
     * @brief Deleted copy assignment (exclusive ownership of connection)
     */
    client& operator=(const client&) = delete;

    /**
     * @brief Move constructor
     */
    client(client&& other) noexcept :
      m_connection(std::exchange(other.m_connection, nullptr)),
      m_subscriptions(std::move(other.m_subscriptions)) {}

    /**
     * @brief Move assignment operator
     */
    client& operator=(client&& other) noexcept {
        if (this != &other) {
            // Clean up current connection
            if (m_connection != nullptr) {
                natsConnection_Destroy(m_connection);
            }
            // Transfer ownership
            m_connection = std::exchange(other.m_connection, nullptr);
            m_subscriptions = std::move(other.m_subscriptions);
        }
        return *this;
    }

    /**
     * @brief Returns whether or not the client is in a connected state
     * @return true if client is connected, otherwise false
     */
    auto is_connected() const -> bool {
        return natsConnection_Status(m_connection) == NATS_CONN_STATUS_CONNECTED;
    }

    /**
     * @brief Connect to a NATS server
     * @param url The URL of the NATS server
     * @return NATS status code
     *
     * If already connected, closes existing connection before connecting to new server.
     */
    auto connect(const std::string& url) -> natsStatus {
        // Close existing connection if any
        if (m_connection != nullptr) {
            natsConnection_Destroy(m_connection);
            m_connection = nullptr;
        }
        return natsConnection_ConnectTo(&m_connection, url.c_str());
    }

    /**
     * @brief Return the NATS server url
     * @return The URL of the NATS server, or empty string if not connected
     */
    auto url() const -> std::string {
        std::array<char, 256> url_str{};
        auto res = natsConnection_GetConnectedUrl(m_connection, url_str.data(), url_str.size());
        if (res != NATS_OK) {
            return "";
        }

        return std::string{url_str.data()};
    }

    /**
     * @brief Publish data bytes on the subject
     * @param subject Name of the subject data is sent to
     * @param data Data bytes to be sent
     * @param reply Optional reply subject
     * @return NATS status code (library handles null connection)
     */
    auto publish(const std::string& subject, std::span<const std::byte> data, const std::string& reply="") -> natsStatus {
        if (!reply.empty()) {
            return natsConnection_PublishRequest(m_connection, subject.c_str(), reply.c_str(), data.data(), data.size());
        }
        return natsConnection_Publish(m_connection, subject.c_str(), data.data(), data.size());
    }

    /**
     * @brief Subscribe to a subject
     * @param subject Name of the subject to subscribe to
     * @return NATS status code (library handles null connection and invalid subject)
     */
    auto subscribe(const std::string& subject) -> natsStatus {
        if (subject.empty()) {
            return NATS_INVALID_SUBJECT;
        }

        auto sub = std::make_unique<subscription>();
        auto sub_ptr = sub->get();
        auto res = natsConnection_Subscribe(&sub_ptr, m_connection, subject.c_str(), &subscription::on_msg, sub.get());

        // Only add subscription if successful
        if (res == NATS_OK) {
            m_subscriptions.emplace(subject, std::move(sub));
        }

        return res;
    }

    /**
     * @brief Get a subscription by subject name
     * @param subject Subject name of the subscription
     * @return subscription pointer
     */
    auto subscriber(std::string_view subject) const -> subscription* {
        auto subj_str = std::string{subject};
        if (!m_subscriptions.contains(subj_str)) {
            return nullptr;
        }
        return m_subscriptions.at(subj_str).get();
    }

private:
    natsConnection* m_connection{nullptr};
    std::map<std::string, std::unique_ptr<subscription>> m_subscriptions;

}; // class client

} // namespace composite::nats
