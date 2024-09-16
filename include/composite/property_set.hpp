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

#include <any>
#include <map>
#include <string>
#include <string_view>

namespace composite {

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

} // namespace composite
