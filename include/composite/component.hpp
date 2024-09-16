#pragma once

#include "input_port.hpp"
#include "lifecycle.hpp"
#include "output_port.hpp"
#include "port_set.hpp"
#include "property_set.hpp"

#include <string>
#include <string_view>
#include <thread>

namespace composite {

enum class retval : int {
    NORMAL,
    NOOP,
    FINISH,
    NO_YIELD
}; // enum class retval

class component : public lifecycle {
    static constexpr int DEFAULT_DELAY{1000000};

public:
    explicit component(std::string_view name) :
      m_name(name),
      m_id(m_name) {
        add_property("thread_delay", &m_delay);
    }

    ~component() override = default;

    auto name() const noexcept -> std::string {
        return m_name;
    }

    auto id() const noexcept -> std::string {
        return m_id;
    }

    auto id(std::string_view id) -> void {
        m_id = id;
    }

    auto initialize() -> void override {
        // To be implemented by subclasses
    }

    auto start() -> void override {
        m_thread = std::jthread(&component::thread_func, this);
    }

    auto stop() -> void override {
        m_thread.request_stop();
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    virtual auto process() -> retval = 0;

    auto add_port(port* port) {
        m_port_set.add_port(port);
    }

    auto get_port(std::string_view name) -> port* {
        return m_port_set.get_port(name);
    }

    auto connect(
      std::string_view output_port_name,
      component* other,
      std::string_view input_port_name
    ) -> bool {
        auto out_port = get_port(output_port_name);
        if (out_port == nullptr) {
            return false;
        }
        if (other == nullptr) {
            return false;
        }
        auto in_port = other->get_port(input_port_name);
        if (in_port == nullptr) {
            return false;
        }
        if (out_port->type_id() != in_port->type_id()) {
            return false;
        }
        out_port->connect(in_port);
        return true;
    }

    template <typename T>
    auto add_property(std::string_view name, T* prop) -> void {
        m_prop_set.add_property(name, prop);
    }

    template <typename T>
    auto set_property(std::string_view name, T value) -> void {
        m_prop_set.set_property(name, value);
    }

    template <typename T>
    auto get_property(std::string_view name) const -> T {
        return m_prop_set.get_property<T>(name);
    }

private:
    std::string m_name;
    std::string m_id;
    std::jthread m_thread;
    std::chrono::nanoseconds m_delay{DEFAULT_DELAY};
    port_set m_port_set;
    property_set m_prop_set;

    auto thread_func(std::stop_token token) -> void {
        while (!token.stop_requested()) {
            auto res = process();
            if (res == retval::NOOP) {
                std::this_thread::sleep_for(m_delay);
            } else if (res == retval::FINISH) {
                break;
            } else if (res == retval::NORMAL) {
                std::this_thread::yield();
            }
        }
    }

}; // class component

} // namespace composite