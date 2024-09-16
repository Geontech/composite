#pragma once

#include <string>

namespace composite {

class port {
public:
    explicit port(std::string_view name) :
      m_name(name) {
    }

    virtual ~port() = default;

    auto name() const noexcept -> std::string {
        return m_name;
    }

    virtual auto type_id() const noexcept -> std::size_t = 0;

    virtual auto connect(port* port) -> void {
        // to be implemented by derived class
    }

private:
    std::string m_name;

}; // class port

} // namespace composite