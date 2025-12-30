/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "errors.hpp"

#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace composite::properties {

/**
 * @brief A single segment of a property path
 *
 * Examples:
 * - "foo" -> name="foo", index=nullopt, is_append=false
 * - "foo[5]" -> name="foo", index=5, is_append=false
 * - "foo[]" -> name="foo", index=nullopt, is_append=true
 */
struct path_segment {
    std::string name;
    std::optional<std::size_t> index;  // For list access: foo[5]
    bool is_append{false};             // For append: foo[]

    [[nodiscard]]
    auto has_index() const noexcept -> bool {
        return index.has_value();
    }

    [[nodiscard]]
    auto is_simple() const noexcept -> bool {
        return !index.has_value() && !is_append;
    }
};

/**
 * @brief Parsed property path supporting dot notation and indexing
 *
 * Supported formats:
 * - "property" -> simple property access
 * - "struct.field" -> nested property access
 * - "list[0]" -> list element access
 * - "list[]" -> list append operation
 * - "struct_list[0].field" -> nested access into list element
 */
class property_path {
public:
    /**
     * @brief Parse a property path string
     * @throws property_error on invalid path syntax
     */
    static auto parse(std::string_view path) -> property_path {
        auto result = property_path{};
        auto current = std::string{};
        auto i = std::size_t{0};

        while (i < path.size()) {
            auto c = path[i];

            if (c == '.') {
                if (!current.empty()) {
                    result.add_segment(current);
                    current.clear();
                }
                ++i;
            } else if (c == '[') {
                // Found a bracket - parse index
                auto name = current;
                current.clear();
                ++i;

                // Find closing bracket
                auto bracket_start = i;
                while (i < path.size() && path[i] != ']') {
                    ++i;
                }

                if (i >= path.size()) {
                    throw property_error(std::format("unclosed bracket in path: {}", path));
                }

                auto index_str = path.substr(bracket_start, i - bracket_start);
                ++i; // skip ']'

                if (index_str.empty()) {
                    // Append operation: foo[]
                    result.add_append_segment(name);
                } else {
                    // Indexed access: foo[5]
                    try {
                        auto index = std::stoul(std::string{index_str});
                        result.add_indexed_segment(name, index);
                    } catch (...) {
                        throw property_error(
                            std::format("invalid index '{}' in path: {}", index_str, path));
                    }
                }
            } else {
                current += c;
                ++i;
            }
        }

        if (!current.empty()) {
            result.add_segment(current);
        }

        if (result.m_segments.empty()) {
            throw property_error(std::format("empty property path: {}", path));
        }

        return result;
    }

    [[nodiscard]]
    auto segments() const noexcept -> const std::vector<path_segment>& {
        return m_segments;
    }

    [[nodiscard]]
    auto is_simple() const noexcept -> bool {
        return m_segments.size() == 1 && m_segments[0].is_simple();
    }

    [[nodiscard]]
    auto head() const -> const path_segment& {
        return m_segments.front();
    }

    [[nodiscard]]
    auto has_tail() const noexcept -> bool {
        return m_segments.size() > 1;
    }

    /// Create a path from all segments after the first
    [[nodiscard]]
    auto tail() const -> property_path {
        auto result = property_path{};
        for (auto i = std::size_t{1}; i < m_segments.size(); ++i) {
            result.m_segments.push_back(m_segments[i]);
        }
        return result;
    }

    [[nodiscard]]
    auto to_string() const -> std::string {
        auto result = std::string{};
        for (auto i = std::size_t{0}; i < m_segments.size(); ++i) {
            if (i > 0) {
                result += '.';
            }
            result += m_segments[i].name;
            if (m_segments[i].is_append) {
                result += "[]";
            } else if (m_segments[i].index.has_value()) {
                result += std::format("[{}]", *m_segments[i].index);
            }
        }
        return result;
    }

private:
    std::vector<path_segment> m_segments;

    auto add_segment(std::string_view name) -> void {
        m_segments.push_back(path_segment{std::string{name}, std::nullopt, false});
    }

    auto add_indexed_segment(std::string_view name, std::size_t index) -> void {
        m_segments.push_back(path_segment{std::string{name}, index, false});
    }

    auto add_append_segment(std::string_view name) -> void {
        m_segments.push_back(path_segment{std::string{name}, std::nullopt, true});
    }
};

} // namespace composite::properties
