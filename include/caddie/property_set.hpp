#pragma once

#include <any>
#include <map>
#include <string>
#include <string_view>

namespace caddie {

class property_set {
public:
    template <typename T>
    auto add_property(std::string_view name, T* prop) -> void {
        m_properties.try_emplace(std::string{name}, prop);
    }

    template <typename T>
    auto set_property(std::string_view name, T value) -> void {
        if (m_properties.contains(std::string{name})) {
            *(*std::any_cast<T*>(&(m_properties.at(std::string{name})))) = value;
        }
    }

    template <typename T>
    auto get_property(std::string_view name) const -> T
    {
        return *std::any_cast<T*>(m_properties.at(std::string{name}));
    }

private:
    std::map<std::string, std::any> m_properties;

}; // class property_set

} // namespace caddie
