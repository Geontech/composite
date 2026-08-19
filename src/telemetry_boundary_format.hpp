/*
 * Copyright (C) 2026 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Histogram bucket-boundary formatting for the OTLP bridge.
 *
 * Lives in src/ rather than include/: it is NOT installed and NOT part of the frozen public
 * surface, but tests add src/ to their include path, so the uniqueness property below can be
 * asserted against the real implementation rather than a copy of it. That matters because a
 * boundary-label collision is invisible from outside — two colliding series both register
 * successfully, and one silently overwrites the other only when Observe() keys measurements by
 * attribute set.
 */

#pragma once

#include <array>
#include <charconv>
#include <string>
#include <system_error>

namespace composite::telemetry::detail {

/// Shortest decimal string that round-trips to exactly @p value.
///
/// std::to_string is fixed 6-decimal and std::format("{:g}") defaults to 6 significant digits;
/// both render distinct nearby doubles identically (1e-7 and 2e-7 both become "0.000000";
/// 1.0 and the next representable double both become "1"). to_chars emits the fewest digits that
/// recover the exact double, so two different doubles can never produce the same label — while
/// ordinary bounds still read as "1", "5", "10" rather than 17-digit noise.
inline auto format_boundary(double value) -> std::string {
    std::array<char, 32> buf{};
    const auto res = std::to_chars(buf.data(), buf.data() + buf.size(), value);
    if (res.ec != std::errc{}) {
        return std::to_string(value); // unreachable for finite doubles at this buffer size
    }
    return std::string(buf.data(), res.ptr);
}

} // namespace composite::telemetry::detail
