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

#include <bit>
#include <cstdint>
#include <map>
#include <string>

namespace composite {

enum class data_type {
    i8, u8,
    i16, u16,
    i32, u32,
    f32, f64
}; // enum class data_type

class data_format {
public:
    bool is_complex{false};
    data_type type{};
    std::endian endianness{std::endian::native};

}; // class data_format

class metadata {
public:
    data_format format;
    float sample_rate{};
    std::map<std::string, std::string> tags;

}; // class metadata

} // namespace composite
