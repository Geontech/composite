/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
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
#include <format>
#include <map>
#include <sstream>
#include <string>

namespace composite {

enum class data_type {
    signed_integer,
    unsigned_integer,
    floating_point
}; // enum class data_type

[[nodiscard]]
inline
auto to_string(data_type type) -> std::string {
    switch (type) {
        case data_type::signed_integer: return "signed_integer";
        case data_type::unsigned_integer: return "unsigned_integer";
        case data_type::floating_point: return "floating_point";
        default: break;
    }
    return "unknown";
}

[[nodiscard]]
inline 
auto to_string(std::endian e) -> std::string {
    if (e == std::endian::little) { return "little"; }
    if (e == std::endian::big) { return "big"; }
    return "native";
}

class data_format {
public:
    bool is_complex{false};
    data_type type{};
    uint32_t bit_width{};
    std::endian endianness{std::endian::native};

    auto operator<=>(const data_format& other) const {
        if (auto cmp = is_complex <=> other.is_complex; cmp != 0) {
            return cmp;
        }
        if (auto cmp = static_cast<int>(type) <=> static_cast<int>(other.type); cmp != 0) {
            return cmp;
        }
        if (auto cmp = bit_width <=> other.bit_width; cmp != 0) {
            return cmp;
        }
        return static_cast<int>(endianness) <=> static_cast<int>(other.endianness);
    }

    auto operator==(const data_format& other) const -> bool {
        return is_complex == other.is_complex &&
               type == other.type &&
               bit_width == other.bit_width &&
               endianness == other.endianness;
    }

    [[nodiscard]]
    inline
    auto to_string() const -> std::string {
        return std::format(
            "data_format:\n"
            "  complex   : {}\n"
            "  type      : {}\n"
            "  bit_width : {}\n"
            "  endianness: {}\n",
            is_complex ? "true" : "false",
            composite::to_string(type),
            bit_width,
            composite::to_string(endianness)
        );
    }

    friend 
    auto operator<<(std::ostream& os, const data_format& df) -> std::ostream& {
        os << df.to_string();
        return os;
    }

}; // class data_format

/**
 * @brief Metadata describing properties of a data stream
 *
 * Metadata is sent alongside data packets through ports to describe the
 * characteristics and properties of the data stream. It is commonly used
 * for signal processing applications to convey information about sample
 * rates, center frequencies, and other stream properties.
 *
 * **Key Fields:**
 * - `format`: Data format information (type, bit width, endianness, complexity)
 * - `center_frequency`: Center frequency in Hz (for RF/signal processing)
 * - `bandwidth`: Signal bandwidth in Hz
 * - `sample_rate`: Sampling rate in samples/sec
 * - `eos`: End-of-stream flag indicating last packet
 * - `annotations`: Extensible key-value pairs for custom metadata
 *
 * **Usage Pattern:**
 * @code
 * // Create and send metadata before data
 * metadata md;
 * md.sample_rate = 1e6;          // 1 MHz
 * md.center_frequency = 2.4e9;    // 2.4 GHz
 * md.bandwidth = 20e6;            // 20 MHz
 * md.format.type = data_type::floating_point;
 * md.format.bit_width = 32;
 * md.annotations["source"] = "antenna_1";
 *
 * output_port.send_metadata(md);
 * output_port.send_data(buffer, timestamp);
 * @endcode
 *
 * **Propagation Rules:**
 * - Metadata is sent via `output_port::send_metadata()`
 * - Metadata is "latched" by input ports until data arrives
 * - Metadata is bundled with the next data packet received
 * - Components can forward, modify, or generate new metadata
 * - Metadata without subsequent data is discarded
 *
 * @see output_port_base::send_metadata()
 * @see input_port::get_data()
 */
class metadata {
public:
    data_format format;
    double center_frequency{};
    double bandwidth{};
    double sample_rate{};
    bool eos{false};
    std::map<std::string, std::string> annotations;

    auto operator<=>(const metadata&) const = default;
    auto operator==(const metadata&) const -> bool = default;

    [[nodiscard]]
    inline
    auto to_string() const -> std::string {
        auto s = std::format(
            "metadata:\n"
            "  data_format:\n"
            "    complex   : {}\n"
            "    type      : {}\n"
            "    bit_width : {}\n"
            "    endianness: {}\n"
            "  center_frequency: {:.3f} Hz\n"
            "  bandwidth       : {:.3f} Hz\n"
            "  sample_rate     : {:.3f} samples/sec\n"
            "  eos             : {}\n"
            "  annotations:\n",
            format.is_complex ? "true" : "false",
            composite::to_string(format.type),
            format.bit_width,
            composite::to_string(format.endianness),
            center_frequency,
            bandwidth,
            sample_rate,
            eos ? "true" : "false"
        );

        if (annotations.empty()) {
            s += "    (none)\n";
        } else {
            for (const auto& [key, value] : annotations) {
                s += std::format("    {}: {}\n", key, value);
            }
        }

        return s;
    }

    friend 
    auto operator<<(std::ostream& os, const metadata& md) -> std::ostream& {
        os << md.to_string();
        return os;
    }

}; // class metadata

} // namespace composite
