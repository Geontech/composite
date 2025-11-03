/*
 * Copyright (C) 2025 Geon Technologies, LLC
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
 * along with this program. If not, see http://www.gnu.org/licenses/.
 */

#pragma once

#include "property_metadata.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace composite::properties {

struct path_segment {
    std::string name;
    std::optional<std::size_t> index;  // For list access: foo[5] or foo[] (nullopt = append)
    bool is_append{false};             // True for foo[]

    auto has_index() const -> bool { return index.has_value(); }
    auto is_simple() const -> bool { return !index.has_value() && !is_append; }
}; // struct path_segment

class property_path {
public:
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
                    throw properties_error(std::format("unclosed bracket in path: {}", path));
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
                        throw properties_error(std::format("invalid index '{}' in path: {}", index_str, path));
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
            throw properties_error(std::format("empty property path: {}", path));
        }

        return result;
    }

    auto segments() const -> const std::vector<path_segment>& {
        return m_segments;
    }

    auto is_simple() const -> bool {
        return m_segments.size() == 1 && m_segments[0].is_simple();
    }

    auto simple_name() const -> const std::string& {
        if (!is_simple()) {
            throw properties_error("path is not simple");
        }
        return m_segments[0].name;
    }

    auto first() const -> const path_segment& {
        return m_segments[0];
    }

    auto has_nested() const -> bool {
        return m_segments.size() > 1;
    }

    // Create a path from all segments after the first
    auto tail() const -> property_path {
        if (m_segments.size() <= 1) {
            throw properties_error("no tail segments");
        }

        auto result = property_path{};
        for (auto i = std::size_t{1}; i < m_segments.size(); ++i) {
            result.m_segments.push_back(m_segments[i]);
        }
        return result;
    }

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

}; // class property_path

} // namespace composite::properties
