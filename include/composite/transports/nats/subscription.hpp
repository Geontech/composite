/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <nats/nats.h>
#include <queue>
#include <span>
#include <string>

#include "message.hpp"

namespace composite::nats {

class subscription {
    using message_ptr = std::unique_ptr<message>;

public:
    static constexpr auto BLOCKING = std::chrono::milliseconds{INTMAX_MAX};

    /**
     * @brief Constructor
     */
    subscription() = default;

    /**
     * @brief Constructor
     * @param subscription NATS subscription to be managed
     */
    explicit subscription(natsSubscription* subscription) : m_subscription(subscription) {}

    /**
     * @brief Destructor
     */
    ~subscription() {
        m_done = true;
        m_msg_cv.notify_all();
        if (m_subscription != nullptr) {
            natsSubscription_Destroy(m_subscription);
            m_subscription = nullptr;
        }
    }

    /**
     * @brief Get a pointer to the underlying NATS subscription
     * @return Pointer to the underlying natsSubscription
     */
    auto get() -> natsSubscription* {
        return m_subscription;
    }

    /**
     * @brief Get a pointer to the underlying NATS subscription
     * @return Pointer to the underlying natsSubscription
     */
    auto get() const -> natsSubscription* {
        return m_subscription;
    }

    auto next_msg(std::chrono::milliseconds timeout=BLOCKING) -> message_ptr {
        using namespace std::chrono_literals;
        auto lock = std::unique_lock{m_msg_mtx};
        if (timeout == BLOCKING) {
            m_msg_cv.wait(lock, [this]{ return !m_msg_queue.empty() || m_done; });
        } else {
            m_msg_cv.wait_for(lock, timeout, [this]{ return !m_msg_queue.empty(); });
        }
        if (m_msg_queue.empty()) {
            return {nullptr};
        }
        auto retval = std::move(m_msg_queue.front());
        m_msg_queue.pop();
        return retval;
    }

    static
    auto on_msg(natsConnection* conn, natsSubscription* sub, natsMsg* msg, void* ctx) -> void {
        if (auto self = static_cast<subscription*>(ctx); self != nullptr) {
            if (msg != nullptr) {
                self->add_msg(std::make_unique<message>(msg));
            }
        }
    }

private:
    natsSubscription* m_subscription{nullptr};
    bool m_done{false};
    std::mutex m_msg_mtx;
    std::condition_variable m_msg_cv;
    std::queue<message_ptr> m_msg_queue;

    auto add_msg(message_ptr&& data) -> void {
        if (data == nullptr) {
            return;
        }
        if (data->data().empty()) {
            return;
        }
        const auto lock = std::scoped_lock{m_msg_mtx};
        m_msg_queue.push(std::move(data));
        m_msg_cv.notify_one();
    }

}; // class subscription

} // namespace composite::nats
