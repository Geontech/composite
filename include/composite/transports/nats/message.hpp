/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <format>
#include <nats/nats.h>
#include <span>
#include <stdexcept>
#include <string>

namespace composite::nats {

class message {
public:
    /**
     * @brief Constructor
     * @param msg NATS message to be managed
     */
    explicit message(natsMsg* msg) : m_message(msg) {}

    /**
     * @brief Creation constructor
     * @param subject The name of the subject on which the message will be published
     * @param data The data to be carried by the message
     * @throws std::runtime_error if message creation fails
     */
    message(const std::string& subject, std::span<const char> data) {
        auto res = natsMsg_Create(&m_message, subject.c_str(), nullptr, data.data(), data.size());
        if (res != NATS_OK) {
            m_message = nullptr;  // Don't destroy, just set null (wasn't created)
            throw std::runtime_error(std::format(
              "Failed to create NATS message: {} ({})",
                natsStatus_GetText(res), static_cast<int>(res)
            ));
        }
    }

    /**
     * @brief Destructor
     */
    ~message() {
        if (m_message != nullptr) {
            natsMsg_Destroy(m_message);
            m_message = nullptr;
        }
    }

    /**
     * @brief Get a pointer to the underlying NATS message
     * @return Pointer to the underlying natsMsg
     */
    auto get() const -> natsMsg* {
        return m_message;
    }

    /**
     * @brief Return a span to the underlying NATS message data
     * @return Span representation of the underlying message data 
     */
    auto data() const -> std::span<const uint8_t> {
        auto msg_data = natsMsg_GetData(m_message);
        if (msg_data == nullptr) {
            return {};
        }
        auto msg_len = natsMsg_GetDataLength(m_message);
        if (msg_len <= 0) {
            return {};
        }
        return {(const uint8_t*)(msg_data), static_cast<std::size_t>(msg_len)};
    }

    /**
     * @brief Get the message's payload length
     * @return Message payload length
     */
    auto size() const -> std::size_t {
        auto msg_len = natsMsg_GetDataLength(m_message);
        if (msg_len <= 0) {
            return {};
        }
        return static_cast<std::size_t>(msg_len);
    }

    /**
     * @brief Get the message's reply subject
     * @return The message's reply subject if it has one, otherwise empty string
     */
    auto reply_subject() const -> std::string {
        if (auto res = natsMsg_GetReply(m_message); res != nullptr) {
            return {res};
        }
        return {};
    }

private:
    natsMsg* m_message{nullptr};

}; // class message

} // namespcae composite::nats
