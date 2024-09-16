#pragma once

#include "component.hpp"
#include "lifecycle.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace composite {

class application : public lifecycle {
public:
    using component_ptr = std::shared_ptr<component>;

    explicit application(std::string_view name) :
      m_name(name) {
    }

    auto name() const noexcept -> const std::string& {
        return m_name;
    }

    auto initialize() -> void override {
        for (auto& component : m_components) {
            component->initialize();
        }
    }

    auto start() -> void override {
        for (auto& component : m_components) {
            component->start();
        }
    }

    auto stop() -> void override {
        for (auto& component : m_components) {
            component->stop();
        }
    }

    auto add_component(component_ptr comp) -> void {
        m_components.emplace_back(comp);
    }

    auto get_component(std::string_view id) const -> component* {
        for (const auto& component : m_components) {
            if (component->id() == id) {
                return component.get();
            }
        }
        return nullptr;
    }

    auto clear() -> void {
        m_components.clear();
    }

private:
    std::string m_name;
    std::vector<component_ptr> m_components;

}; // class application

} // namespace composite
