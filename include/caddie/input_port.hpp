#pragma once

#include "port.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <vector>
#include <typeinfo>

namespace caddie {

template <typename T>
class output_port;

template <typename T>
class input_port : public port {
    static constexpr int WAIT_DURATION{3}; // seconds
public:
    using value_type = T;
    using transfer_type = std::unique_ptr<value_type>;

    explicit input_port(std::string_view name) : port(name) {}

    ~input_port() override {
        m_eos = true;
        m_data_cv.notify_all();
    }

    auto depth() const -> std::size_t {
        return m_depth;
    }

    auto depth(std::size_t value) -> void {
        const auto lock = std::scoped_lock{m_data_mtx};
        m_depth = value;
    }

    auto size() -> std::size_t {
        const auto lock = std::scoped_lock{m_data_mtx};
        return m_queue.size();
    }

    auto clear() -> void {
        const auto lock = std::scoped_lock{m_data_mtx};
        m_queue.clear();
    }

    auto type_id() const noexcept -> std::size_t override {
        return typeid(T).hash_code();
    }

    auto get_data() -> transfer_type {
        using namespace std::chrono_literals;
        auto lock = std::unique_lock{m_data_mtx};
        m_data_cv.wait_for(lock, WAIT_DURATION*1s, [this]{ return !m_queue.empty() || m_eos; });
        if (!m_queue.empty()) {
            auto retval = std::move(m_queue.front());
            m_queue.pop_front();
            return retval;
        }
        return {nullptr};
    }

    auto eos() const noexcept -> bool {
        return m_eos;
    }

private:
    friend class output_port<T>;

    auto add_data(transfer_type data) -> void {
        const auto lock = std::scoped_lock{m_data_mtx};
        if (m_queue.size() < m_depth) {
            m_queue.emplace_back(std::move(data));
            m_data_cv.notify_one();
        }
    }

    auto eos(bool value) -> void {
        m_eos = value;
    }

    std::deque<transfer_type> m_queue;
    std::size_t m_depth{std::numeric_limits<std::size_t>::max()};
    std::mutex m_data_mtx;
    std::condition_variable m_data_cv;
    std::atomic_bool m_eos{false};

}; // class input_port

} // namespace caddie