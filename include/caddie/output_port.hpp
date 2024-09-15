#pragma once

#include "port.hpp"
#include "input_port.hpp"
#include "timestamp.hpp"

#include <ranges>
#include <string_view>
#include <typeinfo>

namespace caddie {

template <typename T>
class output_port : public port {
public:
    using value_type = T;
    using buffer_type = std::unique_ptr<value_type>;

    explicit output_port(std::string_view name) : port(name) {}

    auto type_id() const noexcept -> std::size_t override {
        return typeid(T).hash_code();
    }

    auto send_data(buffer_type data, timestamp ts) -> void {
        for (auto i : std::views::iota(size_t{0}, m_connected_ports.size())) {
            if (auto port = m_connected_ports.at(i); port != nullptr) {
                if (i == m_connected_ports.size() - 1) {
                    // last port, move incoming
                    port->add_data({std::move(data), ts});
                } else {
                    // make a copy of the incoming data
                    auto data_copy = std::make_unique<value_type>(*data);
                    port->add_data({std::move(data_copy), ts});
                }
            }
        }
    }

    auto connect(port* port) -> void override {
        m_connected_ports.emplace_back(static_cast<input_port<T>*>(port));
    }

    auto disconnect() -> void {
        m_connected_ports.clear();
    }

    auto is_connected() const -> bool {
        return !m_connected_ports.empty();
    }

    void eos(bool value) const {
        for (auto port : m_connected_ports) {
            if (port) {
                port->eos(value);
            }
        }
    }

private:
    std::vector<input_port<T>*> m_connected_ports;

}; // class output_port

} // namespace caddie
