/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "composite/util/cpu_affinity.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <spdlog/spdlog.h>
#include <sstream>

namespace composite {

auto get_available_cpus() -> std::optional<cpu_set_t> {
    // Try cgroup v2 first, then v1
    const std::vector<std::string> cpuset_paths = {
        "/sys/fs/cgroup/cpuset.cpus",           // cgroup v2
        "/sys/fs/cgroup/cpuset/cpuset.cpus"     // cgroup v1
    };

    for (const auto& path : cpuset_paths) {
        std::ifstream file(path);
        if (file.is_open()) {
            std::string cpuset_str;
            if (std::getline(file, cpuset_str)) {
                cpu_set_t cpuset = parse_cpuset(cpuset_str);
                if (CPU_COUNT(&cpuset) > 0) {
                    spdlog::debug("Read CPU set from {}: {}", path, cpuset_str);
                    return cpuset;
                }
            }
        }
    }

    // Fallback: use sched_getaffinity to get current affinity
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    if (sched_getaffinity(0, sizeof(cpuset), &cpuset) == 0 && CPU_COUNT(&cpuset) > 0) {
        spdlog::debug("Using current process CPU affinity ({} cores)", CPU_COUNT(&cpuset));
        return cpuset;
    }

    return std::nullopt;
}

auto parse_cpuset(const std::string& cpuset_str) -> cpu_set_t {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);

    std::istringstream ss(cpuset_str);
    std::string token;

    while (std::getline(ss, token, ',')) {
        // Remove whitespace
        token.erase(std::remove_if(token.begin(), token.end(), ::isspace), token.end());

        if (token.empty()) {
            continue;
        }

        // Check for range (e.g., "0-3")
        size_t dash_pos = token.find('-');
        if (dash_pos != std::string::npos) {
            int start = std::stoi(token.substr(0, dash_pos));
            int end = std::stoi(token.substr(dash_pos + 1));
            for (int i = start; i <= end; i++) {
                CPU_SET(i, &cpuset);
            }
        } else {
            // Single CPU
            CPU_SET(std::stoi(token), &cpuset);
        }
    }

    return cpuset;
}

auto parse_affinity_config(
    const std::string& affinity_str,
    const std::vector<int>& available_cores
) -> std::optional<cpu_set_t> {
    // Handle special cases
    if (affinity_str == "none" || affinity_str.empty()) {
        return std::nullopt;  // No affinity configuration
    }

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);

    if (affinity_str == "all") {
        // Use all available cores
        for (int physical_core : available_cores) {
            CPU_SET(physical_core, &cpuset);
        }
        return cpuset;
    }

    // Parse the affinity string as logical indices
    std::istringstream ss(affinity_str);
    std::string token;

    while (std::getline(ss, token, ',')) {
        // Remove whitespace
        token.erase(std::remove_if(token.begin(), token.end(), ::isspace), token.end());

        if (token.empty()) {
            continue;
        }

        // Check for range (e.g., "0-3")
        size_t dash_pos = token.find('-');
        if (dash_pos != std::string::npos) {
            int start = std::stoi(token.substr(0, dash_pos));
            int end = std::stoi(token.substr(dash_pos + 1));

            // Map logical indices to physical cores
            for (int logical_idx = start; logical_idx <= end; logical_idx++) {
                if (logical_idx < 0 || static_cast<size_t>(logical_idx) >= available_cores.size()) {
                    spdlog::error("Logical CPU index {} out of range (have {} cores)",
                        logical_idx, available_cores.size());
                    return std::nullopt;
                }
                int physical_core = available_cores[logical_idx];
                CPU_SET(physical_core, &cpuset);
            }
        } else {
            // Single logical index
            int logical_idx = std::stoi(token);
            if (logical_idx < 0 || static_cast<size_t>(logical_idx) >= available_cores.size()) {
                spdlog::error("Logical CPU index {} out of range (have {} cores)",
                    logical_idx, available_cores.size());
                return std::nullopt;
            }
            int physical_core = available_cores[logical_idx];
            CPU_SET(physical_core, &cpuset);
        }
    }

    // Check if any CPUs were set
    if (CPU_COUNT(&cpuset) == 0) {
        spdlog::error("No valid CPUs specified in affinity configuration: '{}'", affinity_str);
        return std::nullopt;
    }

    return cpuset;
}

auto translate_dpdk_eal_args(
    const std::vector<std::string>& eal_args,
    const std::vector<int>& available_cores
) -> std::pair<std::vector<std::string>, std::vector<int>> {
    std::vector<std::string> translated_args;
    std::vector<int> dpdk_logical_cores;

    for (size_t i = 0; i < eal_args.size(); i++) {
        const auto& arg = eal_args[i];

        // Check for -l flag (lcore list)
        if (arg == "-l" && i + 1 < eal_args.size()) {
            translated_args.push_back(arg);
            const std::string& lcore_str = eal_args[++i];

            // Parse logical core string
            std::istringstream ss(lcore_str);
            std::string token;
            std::vector<int> physical_cores;

            while (std::getline(ss, token, ',')) {
                token.erase(std::remove_if(token.begin(), token.end(), ::isspace), token.end());
                if (token.empty()) continue;

                // Check for range (e.g., "0-3")
                size_t dash_pos = token.find('-');
                if (dash_pos != std::string::npos) {
                    int start = std::stoi(token.substr(0, dash_pos));
                    int end = std::stoi(token.substr(dash_pos + 1));
                    for (int logical = start; logical <= end; logical++) {
                        if (static_cast<size_t>(logical) < available_cores.size()) {
                            physical_cores.push_back(available_cores[logical]);
                            dpdk_logical_cores.push_back(logical);
                        } else {
                            spdlog::error("DPDK lcore logical index {} out of range (have {} cores)",
                                logical, available_cores.size());
                        }
                    }
                } else {
                    // Single logical core
                    int logical = std::stoi(token);
                    if (static_cast<size_t>(logical) < available_cores.size()) {
                        physical_cores.push_back(available_cores[logical]);
                        dpdk_logical_cores.push_back(logical);
                    } else {
                        spdlog::error("DPDK lcore logical index {} out of range (have {} cores)",
                            logical, available_cores.size());
                    }
                }
            }

            // Build physical core string
            std::string physical_str;
            for (size_t j = 0; j < physical_cores.size(); j++) {
                if (j > 0) physical_str += ",";
                physical_str += std::to_string(physical_cores[j]);
            }
            translated_args.push_back(physical_str);

            spdlog::debug("Translated DPDK -l {} (logical) -> {} (physical)",
                lcore_str, physical_str);

        } else if (arg == "--lcores" && i + 1 < eal_args.size()) {
            // --lcores format is more complex, e.g., "(0-1)@(0-1)"
            // For now, log a warning and pass through unchanged
            spdlog::warn("DPDK --lcores argument detected. Translation not yet implemented, "
                "passing through as-is. Use -l for automatic translation.");
            translated_args.push_back(arg);
            translated_args.push_back(eal_args[++i]);

        } else {
            // Pass through other arguments unchanged
            translated_args.push_back(arg);
        }
    }

    return {translated_args, dpdk_logical_cores};
}

} // namespace composite
