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

#include "input_port.hpp"
#ifdef COMPOSITE_USE_NATS
#include "nats/client.hpp"
#endif
#include "port.hpp"
#include "timestamp.hpp"

#include <algorithm>
#include <ranges>
#include <string_view>
#include <typeinfo>

namespace composite {

template <traits::smart_ptr T>
requires std::ranges::contiguous_range<typename T::element_type>
class output_port : public port {
public:
    using value_type = typename T::element_type;
    using buffer_type = T;

    explicit output_port(std::string_view name) : port(name) {}

    ~output_port() override = default;

    auto type_id() const noexcept -> std::size_t override {
        return typeid(value_type).hash_code();
    }

    auto is_unique_type() const noexcept -> bool override {
        return traits::is_unique_ptr_v<T>;
    }

    auto send_data(buffer_type data, timestamp ts) -> void {
#ifdef COMPOSITE_USE_NATS
        // Send to NATS subjects
        for (const auto& subject : m_nats_subjects) {
            m_nats_client->publish(
                subject,
                std::as_bytes(std::span{std::ranges::data(*data), std::ranges::size(*data)})
            );
        }
#endif
        if constexpr (traits::is_unique_ptr_v<T>) {
            handle_unique(std::move(data), ts);
        } else { // shared_ptr
            handle_shared(data, ts);
        }
    }

    auto connect(port* port) -> void override {
        m_connected_ports.emplace_back(port);
        // sort with unique_ptr ports at the back
        std::ranges::sort(m_connected_ports, [](const auto a, const auto b) { 
            return (!a->is_unique_type() && b->is_unique_type());
        });
    }

#ifdef COMPOSITE_USE_NATS
    auto connect(std::string_view url, std::string_view subject) -> bool override {
        if (m_nats_client != nullptr) {
            return false;
        }
        m_nats_client = std::make_unique<nats::client>(std::string{url});
        if (!m_nats_client->is_connected()) {
            return false;
        }
        m_nats_subjects.emplace_back(subject);
        return true;
    }
#endif

    auto disconnect() -> void {
        m_connected_ports.clear();
    }

    auto is_connected() const -> bool {
        return !m_connected_ports.empty();
    }

private:
    std::vector<port*> m_connected_ports;
#ifdef COMPOSITE_USE_NATS
    std::unique_ptr<nats::client> m_nats_client;
    std::vector<std::string> m_nats_subjects;
#endif

    auto handle_unique(buffer_type data, timestamp ts) const -> void {
        auto shared_data = std::shared_ptr<value_type>{};
        for (auto i = std::size_t{}; i < m_connected_ports.size(); ++i) {
            auto& port = m_connected_ports.at(i);
            if (port == nullptr) {
                continue;
            }
            if (port->is_unique_type()) { // u -> u
                auto dst = static_cast<input_port<T>*>(port);
                if (i == m_connected_ports.size() - 1) {
                    // last port, move incoming
                    dst->add_data({std::move(data), ts});
                } else {
                    // make a copy of the incoming data
                    dst->add_data({std::make_unique<value_type>(*data), ts});
                }
            } else { // u -> s
                auto dst = static_cast<input_port<std::shared_ptr<value_type>>*>(port);
                if (i == m_connected_ports.size() - 1) {
                    dst->add_data({std::shared_ptr<value_type>{data.release()}, ts});
                } else {
                    if (shared_data == nullptr) {
                        shared_data = std::make_shared<value_type>(*data);
                    }
                    dst->add_data({shared_data, ts});
                }
            }
        }
    }

    auto handle_shared(buffer_type data, timestamp ts) const -> void {
        for (auto& port : m_connected_ports) {
            if (port == nullptr) {
                continue;
            }
            if (port->is_unique_type()) {
                auto dst = static_cast<input_port<std::unique_ptr<value_type>>*>(port);
                if (m_connected_ports.size() == 1) {
                    dst->add_data({std::make_unique<value_type>(std::move(*data)), ts});
                } else {
                    dst->add_data({std::make_unique<value_type>(*data), ts});
                }
            } else {
                auto dst = static_cast<input_port<T>*>(port);
                dst->add_data({data, ts});
            }
        }
    }

}; // class output_port

} // namespace composite
