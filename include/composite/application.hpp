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
