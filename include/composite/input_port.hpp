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

#include "metadata.hpp"
#include "port.hpp"
#include "timestamp.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <limits>
#include <optional>
#include <ranges>
#include <tuple>
#include <typeinfo>

namespace composite {

template <traits::smart_ptr T>
requires std::ranges::contiguous_range<typename T::element_type>
class output_port;

template <traits::smart_ptr T>
requires std::ranges::contiguous_range<typename T::element_type>
class input_port : public input_port_base {
    static constexpr int WAIT_DURATION{1}; // seconds
public:
    using value_type = typename T::element_type;
    using buffer_type = T;

    using input_port_base::input_port_base;

    ~input_port() override = default;

    auto size() const -> std::size_t {
        const auto lock = std::scoped_lock{m_mtx};
        return m_queue.size();
    }

    auto clear() -> void {
        const auto lock = std::scoped_lock{m_mtx};
        m_queue.clear();
    }

    auto type_id() const noexcept -> std::size_t override {
        return typeid(value_type).hash_code();
    }

    auto is_unique_type() const noexcept -> bool override {
        return traits::is_unique_ptr_v<T>;
    }

    auto get_data() -> std::tuple<buffer_type, composite::timestamp, std::optional<composite::metadata>> {
        using namespace std::chrono_literals;
        auto lock = std::unique_lock{m_mtx};
        m_cv.wait_for(lock, WAIT_DURATION*1s, [this]{ return !m_queue.empty(); });
        if (!m_queue.empty()) {
            auto retval = std::move(m_queue.front());
            m_queue.pop_front();
            return retval;
        }
        return {};
    }

private:
    auto add_data(buffer_type data, timestamp ts) -> void {
        const auto lock = std::scoped_lock{m_mtx};
        if (m_queue.size() < m_depth) {
            m_queue.emplace_back(std::make_tuple(std::move(data), ts, m_metadata));
            m_metadata.reset();
            m_cv.notify_one();
        }
    }

    std::deque<std::tuple<buffer_type, composite::timestamp, std::optional<composite::metadata>>> m_queue;

    friend class output_port<std::unique_ptr<value_type>>;
    friend class output_port<std::shared_ptr<value_type>>;

}; // class input_port

} // namespace composite