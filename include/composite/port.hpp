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