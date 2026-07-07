/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */
 
#pragma once

#include <bit>
#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

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
 * // Build metadata ONCE (or when a field actually changes), then attach the same
 * // shared instance to every packet — attachment is a refcount bump, not a copy.
 * metadata md;
 * md.sample_rate = 1e6;          // 1 MHz
 * md.center_frequency = 2.4e9;    // 2.4 GHz
 * md.bandwidth = 20e6;            // 20 MHz
 * md.format.type = data_type::floating_point;
 * md.format.bit_width = 32;
 * md.annotations["source"] = "antenna_1";
 * auto md_ptr = std::make_shared<const metadata>(std::move(md));
 *
 * output_port.send_data(buffer, timestamp, md_ptr);   // every packet: refcount bump only
 * @endcode
 *
 * **Propagation Rules:**
 * - Metadata travels atomically with its data packet (the third argument to
 *   `output_port::send_data()`), so it cannot be mis-associated with a
 *   different packet under concurrent producers (no latch, no race).
 * - It is carried as a `shared_ptr<const metadata>`: producers rebuild the
 *   instance only when a field changes, all packets in between share it, and
 *   consumers can detect "unchanged" by pointer identity instead of a deep
 *   compare. Fan-out receivers share one instance.
 * - It is delivered together with the buffer in `input_port::get_data()`'s tuple.
 * - A convenience `send_data` overload still accepts a plain
 *   `std::optional<metadata>` value and wraps it; use the shared form on hot paths.
 *
 * @see output_port::send_data()
 * @see input_port::get_data()
 */
/**
 * @brief A typed metadata annotation value: bool, integer, double, or string.
 *
 * Replaces the old string-only annotation so numeric metadata (sample counts,
 * gains, flags) is carried as its real type rather than stringified and reparsed.
 * Implicitly constructs from bool/integers/double/string (and `const char*`), so
 * `md.annotations["gain"] = 2.5;` / `= 42` / `= true` / `= "iq"` all work, and
 * comparing to a string literal still compiles.
 */
class annotation_value {
public:
    using variant_type = std::variant<bool, std::int64_t, double, std::string>;

    annotation_value() : m_v(std::string{}) {}
    annotation_value(bool b) : m_v(b) {}                                   // NOLINT(google-explicit-constructor)
    annotation_value(std::int64_t i) : m_v(i) {}                           // NOLINT
    annotation_value(int i) : m_v(static_cast<std::int64_t>(i)) {}         // NOLINT
    annotation_value(std::uint64_t u) : m_v(static_cast<std::int64_t>(u)) {}  // NOLINT
    annotation_value(double d) : m_v(d) {}                                 // NOLINT
    annotation_value(const char* s) : m_v(std::string{s}) {}              // NOLINT
    annotation_value(std::string s) : m_v(std::move(s)) {}                 // NOLINT

    auto operator<=>(const annotation_value&) const = default;
    auto operator==(const annotation_value&) const -> bool = default;

    [[nodiscard]] auto value() const -> const variant_type& { return m_v; }
    template <typename T> [[nodiscard]] auto get() const -> T { return std::get<T>(m_v); }
    template <typename T> [[nodiscard]] auto holds() const -> bool { return std::holds_alternative<T>(m_v); }

    [[nodiscard]] auto to_string() const -> std::string {
        return std::visit([](const auto& v) -> std::string {
            using V = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<V, bool>) { return v ? "true" : "false"; }
            else if constexpr (std::is_same_v<V, std::string>) { return v; }
            else { return std::format("{}", v); }
        }, m_v);
    }

private:
    variant_type m_v;
};

/// nlohmann ADL hooks so annotations (and maps of them) round-trip JSON with their
/// real type — a bool/integer/double/string, not a stringified value.
inline auto to_json(nlohmann::json& j, const annotation_value& v) -> void {
    std::visit([&j](const auto& x) { j = x; }, v.value());
}
inline auto from_json(const nlohmann::json& j, annotation_value& v) -> void {
    if (j.is_boolean()) { v = annotation_value(j.get<bool>()); }
    else if (j.is_number_integer() || j.is_number_unsigned()) { v = annotation_value(j.get<std::int64_t>()); }
    else if (j.is_number_float()) { v = annotation_value(j.get<double>()); }
    else if (j.is_string()) { v = annotation_value(j.get<std::string>()); }
    else { v = annotation_value{}; }  // null / unsupported -> default (empty string)
}

class metadata {
public:
    data_format format;
    double center_frequency{};
    double bandwidth{};
    double sample_rate{};
    bool eos{false};
    std::map<std::string, annotation_value> annotations;

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
                s += std::format("    {}: {}\n", key, value.to_string());
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

/// How metadata travels through ports: one immutable instance shared by every packet
/// (and every fan-out receiver) until the producer publishes a changed one. nullptr
/// means "no metadata on this packet".
using metadata_ptr = std::shared_ptr<const metadata>;

/// Wrap a metadata value for sending. Producers should call this once per CHANGE and
/// reuse the returned pointer for every subsequent packet, not once per packet.
[[nodiscard]]
inline auto make_metadata(metadata md) -> metadata_ptr {
    return std::make_shared<const metadata>(std::move(md));
}

} // namespace composite
