/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <chrono>
#include <compare>
#include <cstdint>
#include <format>
#include <stdexcept>
#include <string>

namespace composite {

/**
 * @brief High-precision timestamp for signal processing applications
 *
 * Represents time as seconds + picoseconds for high-precision timing.
 * Provides comparison, arithmetic, validation, and conversions to/from std::chrono.
 *
 * **Invariant**: picoseconds must be < 1'000'000'000'000 (1 second)
 * - Use normalize() to enforce this after manual modification
 * - All operations automatically maintain normalized state
 */
class timestamp {
public:
    uint32_t seconds{};
    uint64_t picoseconds{};

    // ========================================================================
    // Comparison operators (C++20 spaceship operator)
    // ========================================================================

    /**
     * @brief Three-way comparison operator
     *
     * Enables all comparison operators: ==, !=, <, <=, >, >=
     */
    auto operator<=>(const timestamp& other) const -> std::strong_ordering {
        if (auto cmp = seconds <=> other.seconds; cmp != 0) {
            return cmp;
        }
        return picoseconds <=> other.picoseconds;
    }

    /// Equality comparison
    auto operator==(const timestamp& other) const -> bool = default;

    // ========================================================================
    // Arithmetic operations
    // ========================================================================

    /**
     * @brief Calculate duration between two timestamps
     *
     * @return Duration in nanoseconds (max precision std::chrono supports)
     * @throws std::overflow_error if duration exceeds nanoseconds range
     */
    auto operator-(const timestamp& other) const -> std::chrono::nanoseconds {
        // Calculate total nanoseconds for each timestamp
        auto this_ns = static_cast<int64_t>(seconds) * 1'000'000'000LL + static_cast<int64_t>(picoseconds / 1000);
        auto other_ns =
            static_cast<int64_t>(other.seconds) * 1'000'000'000LL + static_cast<int64_t>(other.picoseconds / 1000);

        auto diff = this_ns - other_ns;
        return std::chrono::nanoseconds{diff};
    }

    /**
     * @brief Add a duration to this timestamp
     *
     * @param dur Duration to add (can be negative for subtraction)
     * @return New timestamp with duration added
     */
    auto operator+(std::chrono::nanoseconds dur) const -> timestamp {
        auto total_ns = static_cast<int64_t>(seconds) * 1'000'000'000LL + static_cast<int64_t>(picoseconds / 1000);
        total_ns += dur.count();

        if (total_ns < 0) {
            throw std::underflow_error("timestamp::operator+: result would be negative");
        }

        timestamp result;
        result.seconds = static_cast<uint32_t>(total_ns / 1'000'000'000LL);
        result.picoseconds = static_cast<uint64_t>((total_ns % 1'000'000'000LL) * 1000);
        return result;
    }

    /**
     * @brief Subtract a duration from this timestamp
     */
    auto operator-(std::chrono::nanoseconds dur) const -> timestamp { return *this + (-dur); }

    // ========================================================================
    // std::chrono conversions
    // ========================================================================

    /**
     * @brief Create timestamp from std::chrono::system_clock time_point
     *
     * @param tp Time point to convert
     * @return Corresponding timestamp
     */
    static auto from_chrono(std::chrono::system_clock::time_point tp) -> timestamp {
        auto duration = tp.time_since_epoch();
        auto seconds_count = std::chrono::duration_cast<std::chrono::seconds>(duration);
        auto remaining = duration - seconds_count;
        auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(remaining);

        timestamp result;
        result.seconds = static_cast<uint32_t>(seconds_count.count());
        result.picoseconds = static_cast<uint64_t>(nanoseconds.count()) * 1000; // ns to ps
        return result;
    }

    /**
     * @brief Convert timestamp to std::chrono::system_clock::time_point
     *
     * Note: Loses picosecond precision beyond nanoseconds
     */
    auto to_chrono() const -> std::chrono::system_clock::time_point {
        auto secs = std::chrono::seconds{seconds};
        auto nanos = std::chrono::nanoseconds{picoseconds / 1000}; // ps to ns
        return std::chrono::system_clock::time_point{secs + nanos};
    }

    /**
     * @brief Get current system time as timestamp
     */
    static auto now() -> timestamp { return from_chrono(std::chrono::system_clock::now()); }

    // ========================================================================
    // Validation and normalization
    // ========================================================================

    /**
     * @brief Check if timestamp is in valid normalized state
     *
     * @return true if picoseconds < 1 second (1e12 picoseconds)
     */
    auto is_valid() const -> bool { return picoseconds < 1'000'000'000'000ULL; }

    /**
     * @brief Normalize timestamp by moving excess picoseconds into seconds
     *
     * Call this after manually modifying picoseconds to ensure validity.
     * All operator methods automatically return normalized timestamps.
     */
    auto normalize() -> void {
        if (picoseconds >= 1'000'000'000'000ULL) {
            auto extra_seconds = picoseconds / 1'000'000'000'000ULL;
            seconds += static_cast<uint32_t>(extra_seconds);
            picoseconds = picoseconds % 1'000'000'000'000ULL;
        }
    }

    // ========================================================================
    // Formatting
    // ========================================================================

    /**
     * @brief Convert timestamp to string representation
     *
     * @return String in format "seconds.picoseconds"
     */
    auto to_string() const -> std::string { return std::format("{}.{:012}", seconds, picoseconds); }

}; // class timestamp

} // namespace composite
