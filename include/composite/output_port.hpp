/*
 * Copyright (C) 2024 Geon Technologies, LLC
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

#include "port.hpp"
#include "input_port.hpp"
#include "timestamp.hpp"

#include <algorithm>
#include <ranges>
#include <string_view>
#include <thread>
#include <typeinfo>

namespace composite {

template <traits::smart_ptr T>
class output_port : public port {
public:
    using value_type = typename T::element_type;
    using buffer_type = T;
    using timestamp_type = timestamp;

    explicit output_port(std::string_view name) : port(name) {}

    ~output_port() override {
        m_thread.request_stop();
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    auto type_id() const noexcept -> std::size_t override {
        return typeid(value_type).hash_code();
    }

    auto is_unique_type() const noexcept -> bool override {
        return traits::is_unique_ptr_v<T>;
    }

    auto send_data(buffer_type data, timestamp_type ts) -> void {
        if (m_thread.joinable()) {
            const auto lock = std::scoped_lock{m_data_mtx};
            m_queue.emplace_back(std::make_tuple(std::move(data), ts));
            m_data_cv.notify_one();
        } else if constexpr (traits::is_unique_ptr_v<T>) {
            if (m_connected_ports.size() == 1) {
                if (auto dst_port = m_connected_ports.front(); dst_port != nullptr) {
                    if (dst_port->is_unique_type()) { // u -> u
                        auto dst = static_cast<input_port<T>*>(dst_port);
                        dst->add_data({std::move(data), ts});
                    } else { // u -> s
                        auto dst = static_cast<input_port<std::shared_ptr<value_type>>*>(dst_port);
                        dst->add_data({std::shared_ptr<value_type>{data.release()}, ts});
                    }
                }
            } else { // u -> s,s,...
                auto shared_data = std::shared_ptr<value_type>{data.release()};
                for (auto port : m_connected_ports) {
                    if (port != nullptr) {
                        auto dst = static_cast<input_port<std::shared_ptr<value_type>>*>(port);
                        dst->add_data({shared_data, ts});
                    }
                }
            }
        } else { // shared_ptr; s -> s || s -> s,s,...
            for (auto& port : m_connected_ports) {
                if (port != nullptr) {
                    auto dst = static_cast<input_port<T>*>(port);
                    dst->add_data({data, ts});
                }
            }
        }
    }

    auto connect(port* port) -> void override {
        m_connected_ports.emplace_back(port);
        // sort with unique_ptr ports at the back
        std::ranges::sort(m_connected_ports, [](const auto a, const auto b) { 
            return (!a->is_unique_type() && b->is_unique_type());
        });
        // start thread if required
        auto thread_required = (traits::is_shared_ptr_v<T> && port->is_unique_type()) ||
          (m_connected_ports.size() > 1 && port->is_unique_type());
        if (thread_required && !m_thread.joinable()) {
            m_thread = std::jthread(&output_port::thread_func, this);
        }
    }

    auto disconnect() -> void {
        m_connected_ports.clear();
        m_thread.request_stop();
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    auto is_connected() const -> bool {
        return !m_connected_ports.empty();
    }

    void eos(bool value) const {
        for (auto port : m_connected_ports) {
            if (port) {
                if (port->is_unique_type()) {
                    static_cast<input_port<std::unique_ptr<value_type>>*>(port)->eos(value);
                } else { // shared_ptr
                    static_cast<input_port<std::shared_ptr<value_type>>*>(port)->eos(value);
                }
            }
        }
    }

private:
    std::vector<port*> m_connected_ports;
    std::jthread m_thread;
    std::deque<std::tuple<buffer_type, timestamp_type>> m_queue;
    std::mutex m_data_mtx;
    std::condition_variable m_data_cv;

    auto thread_func(std::stop_token token) -> void {
        while (!token.stop_requested()) {
            using namespace std::chrono_literals;
            auto lock = std::unique_lock{m_data_mtx};
            m_data_cv.wait_for(lock, 1s, [this]{ return !m_queue.empty(); });
            if (!m_queue.empty()) {
                auto [data, ts] = std::move(m_queue.front());
                m_queue.pop_front();
                lock.unlock();
                // Determine structure of fan-out
                auto all_unique = true;
                for (const auto port : m_connected_ports) {
                    if (port != nullptr && !port->is_unique_type()) {
                        all_unique = false;
                        break;
                    }
                }
                // What type am I?
                if constexpr (traits::is_unique_ptr_v<T>) {
                    if (all_unique) { // u -> u,u,...
                        for (auto i : std::views::iota(size_t{0}, m_connected_ports.size())) {
                            if (auto port = m_connected_ports.at(i); port != nullptr) {
                                auto dst = static_cast<input_port<T>*>(port);
                                if (i == m_connected_ports.size() - 1) {
                                    // last port, move incoming
                                    dst->add_data({std::move(data), ts});
                                } else {
                                    // make a copy of the incoming data
                                    auto data_copy = std::make_unique<value_type>(*data);
                                    dst->add_data({std::move(data_copy), ts});
                                }
                            }
                        }
                    } else { // u -> s,...,u,...
                        auto shared_data = std::make_shared<value_type>(*data);
                        for (auto i : std::views::iota(size_t{0}, m_connected_ports.size())) {
                            if (auto port = m_connected_ports.at(i); port != nullptr) {
                                if (port->is_unique_type()) {
                                    auto dst = static_cast<input_port<T>*>(port);
                                    if (i == m_connected_ports.size() - 1) {
                                        // last port, move incoming
                                        dst->add_data({std::move(data), ts});
                                    } else {
                                        // make a copy of the incoming data
                                        auto data_copy = std::make_unique<value_type>(*data);
                                        dst->add_data({std::move(data_copy), ts});
                                    }
                                } else { // shared_ptr
                                    auto dst = static_cast<input_port<std::shared_ptr<value_type>>*>(port);
                                    dst->add_data({shared_data, ts});
                                }
                            }
                        }
                    }
                } else { // shared_ptr
                    auto unique_data = std::make_unique<value_type>(*data);
                    if (all_unique) { // s -> u || s -> u,u,...
                        for (auto i : std::views::iota(size_t{0}, m_connected_ports.size())) {
                            if (auto port = m_connected_ports.at(i); port != nullptr) {
                                auto dst = static_cast<input_port<std::unique_ptr<value_type>>*>(port);
                                if (i == m_connected_ports.size() - 1) {
                                    // last port, move incoming
                                    dst->add_data({std::move(unique_data), ts});
                                } else {
                                    // make a copy of the incoming data
                                    auto data_copy = std::make_unique<value_type>(*unique_data);
                                    dst->add_data({std::move(data_copy), ts});
                                }
                            }
                        }
                    } else { // s -> s,...,u,...
                        for (auto i : std::views::iota(size_t{0}, m_connected_ports.size())) {
                            if (auto port = m_connected_ports.at(i); port != nullptr) {
                                if (port->is_unique_type()) {
                                    auto dst = static_cast<input_port<std::unique_ptr<value_type>>*>(port);
                                    if (i == m_connected_ports.size() - 1) {
                                        // last port, move incoming
                                        dst->add_data({std::move(unique_data), ts});
                                    } else {
                                        // make a copy of the incoming data
                                        auto data_copy = std::make_unique<value_type>(*unique_data);
                                        dst->add_data({std::move(data_copy), ts});
                                    }
                                } else { // shared_ptr
                                    auto dst = static_cast<input_port<T>*>(port);
                                    dst->add_data({data, ts});
                                }
                            }
                        }
                    }
                }
            }
        }
    }

}; // class output_port

} // namespace composite
