#pragma once

#include "component.hpp"
#include "lifecycle.hpp"

#include <memory>
#include <vector>

namespace caddie {

class application : public lifecycle {
public:
    using component_ptr = std::shared_ptr<component>;

    explicit application(std::string_view name) :
      m_name(name) {
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

    auto get_component(std::string_view name) const -> component* {
        for (const auto& component : m_components) {
            if (component->name() == name) {
                return component.get();
            }
        }
        return nullptr;
    }

private:
    std::string m_name;
    std::vector<component_ptr> m_components;

}; // class application

} // namespace caddie
