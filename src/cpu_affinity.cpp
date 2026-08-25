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
#include <stdexcept>

namespace composite {

namespace {

auto read_first_line(const std::string& path) -> std::optional<std::string> {
    std::ifstream file(path);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::string line;
    if (!std::getline(file, line)) {
        return std::nullopt;
    }
    return line;
}

auto try_parse_cpuset(const std::string& cpuset_str) -> std::optional<cpu_set_t> {
    auto cpuset = parse_cpuset(cpuset_str);
    if (CPU_COUNT(&cpuset) > 0) {
        return cpuset;
    }
    return std::nullopt;
}

auto try_read_cpuset_file(const std::string& path, const std::string& label) -> std::optional<cpu_set_t> {
    if (auto line = read_first_line(path)) {
        if (auto cpuset = try_parse_cpuset(*line)) {
            spdlog::debug("Read CPU set from {} ({}): {}", path, label, *line);
            return cpuset;
        }
    }
    return std::nullopt;
}

auto build_cgroup_path(const std::string& base, const std::string& suffix) -> std::string {
    if (suffix.empty() || suffix == "/") {
        return base;
    }
    if (suffix.front() == '/') {
        return base + suffix;
    }
    return base + "/" + suffix;
}

auto try_cgroup_cpuset() -> std::optional<cpu_set_t> {
    auto cgroup_line = read_first_line("/proc/self/cgroup");
    if (!cgroup_line) {
        return std::nullopt;
    }

    std::ifstream file("/proc/self/cgroup");
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) {
            continue;
        }

        auto first_colon = line.find(':');
        if (first_colon == std::string::npos) {
            continue;
        }
        auto second_colon = line.find(':', first_colon + 1);
        if (second_colon == std::string::npos) {
            continue;
        }

        auto controllers = line.substr(first_colon + 1, second_colon - first_colon - 1);
        auto path = line.substr(second_colon + 1);

        if (controllers.empty()) {
            // cgroup v2
            auto base = std::string("/sys/fs/cgroup");
            auto full = build_cgroup_path(base, path);
            if (auto cpuset = try_read_cpuset_file(full + "/cpuset.cpus.effective", "cgroup v2 effective")) {
                return cpuset;
            }
            if (auto cpuset = try_read_cpuset_file(full + "/cpuset.cpus", "cgroup v2")) {
                return cpuset;
            }
        } else if (controllers.find("cpuset") != std::string::npos) {
            // cgroup v1
            auto base = std::string("/sys/fs/cgroup/cpuset");
            auto full = build_cgroup_path(base, path);
            if (auto cpuset = try_read_cpuset_file(full + "/cpuset.cpus", "cgroup v1")) {
                return cpuset;
            }
        }
    }

    return std::nullopt;
}

} // namespace

auto get_available_cpus() -> std::optional<cpu_set_t> {
    // Try the current cgroup first (works for k8s and cgroup v1/v2)
    if (auto cpuset = try_cgroup_cpuset()) {
        return cpuset;
    }

    // Fallback: try root cgroup paths
    if (auto cpuset = try_read_cpuset_file("/sys/fs/cgroup/cpuset.cpus", "root cgroup v2")) {
        return cpuset;
    }
    if (auto cpuset = try_read_cpuset_file("/sys/fs/cgroup/cpuset/cpuset.cpus", "root cgroup v1")) {
        return cpuset;
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

auto parse_affinity_config(const std::string& affinity_str, const std::vector<int>& available_cores)
    -> std::optional<cpu_set_t> {
    // Handle special cases
    if (affinity_str == "none" || affinity_str.empty()) {
        return std::nullopt; // No affinity configuration
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
                    spdlog::error("Logical CPU index {} out of range (have {} cores)", logical_idx,
                                  available_cores.size());
                    return std::nullopt;
                }
                int physical_core = available_cores[logical_idx];
                CPU_SET(physical_core, &cpuset);
            }
        } else {
            // Single logical index
            int logical_idx = std::stoi(token);
            if (logical_idx < 0 || static_cast<size_t>(logical_idx) >= available_cores.size()) {
                spdlog::error("Logical CPU index {} out of range (have {} cores)", logical_idx, available_cores.size());
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

auto process_available_cores() -> const std::vector<int>& {
    // Captured once (magic static): the mask is a process-startup property, and re-reading it
    // per call could hand two creation paths two different views of the machine.
    static const std::vector<int> cores = [] {
        std::vector<int> out;
        if (const auto cpuset_opt = get_available_cpus()) {
            for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
                if (CPU_ISSET(cpu, &(*cpuset_opt))) {
                    out.push_back(cpu);
                }
            }
        }
        return out;
    }();
    return cores;
}

auto translate_dpdk_eal_args(const std::vector<std::string>& eal_args, const std::vector<int>& available_cores)
    -> std::pair<std::vector<std::string>, std::vector<int>> {
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

            // Guarded parse: std::stoi throws on garbage, and this runs outside any config
            // try/catch in the caller's error path — rewrap as invalid_argument naming the
            // token so a typo'd core list fails the load cleanly instead of escaping main().
            const auto parse_core = [](const std::string& text) -> int {
                try {
                    std::size_t consumed = 0;
                    const int v = std::stoi(text, &consumed);
                    if (consumed != text.size()) {
                        throw std::invalid_argument{""};
                    }
                    return v;
                } catch (const std::exception&) {
                    throw std::invalid_argument("invalid DPDK lcore token '" + text + "' in -l list");
                }
            };

            while (std::getline(ss, token, ',')) {
                token.erase(std::remove_if(token.begin(), token.end(), ::isspace), token.end());
                if (token.empty()) continue;

                // Check for range (e.g., "0-3")
                size_t dash_pos = token.find('-');
                if (dash_pos != std::string::npos) {
                    int start = parse_core(token.substr(0, dash_pos));
                    int end = parse_core(token.substr(dash_pos + 1));
                    for (int logical = start; logical <= end; logical++) {
                        if (static_cast<size_t>(logical) < available_cores.size()) {
                            physical_cores.push_back(available_cores[logical]);
                            dpdk_logical_cores.push_back(logical);
                        } else {
                            spdlog::error("DPDK lcore logical index {} out of range (have {} cores)", logical,
                                          available_cores.size());
                        }
                    }
                } else {
                    // Single logical core
                    int logical = parse_core(token);
                    if (static_cast<size_t>(logical) < available_cores.size()) {
                        physical_cores.push_back(available_cores[logical]);
                        dpdk_logical_cores.push_back(logical);
                    } else {
                        spdlog::error("DPDK lcore logical index {} out of range (have {} cores)", logical,
                                      available_cores.size());
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

            spdlog::debug("Translated DPDK -l {} (logical) -> {} (physical)", lcore_str, physical_str);

        } else if (arg == "--lcores" || arg.starts_with("--lcores=")) {
            // REJECTED, not passed through: every other core list in the config is expressed in
            // LOGICAL indices and translated to physical here, but --lcores has its own
            // "(lcores)@(cpus)" grammar this translator does not implement. Passing it through
            // untranslated silently mixed two coordinate systems in one config — the untranslated
            // values were handed to EAL as PHYSICAL ids while -l users wrote logical ones. Fail
            // the load loudly instead; -l covers the supported cases.
            throw std::invalid_argument("DPDK --lcores is not supported: its core ids would bypass the "
                                        "logical->physical translation applied to -l. Express the core list "
                                        "with -l instead.");
        } else {
            // Pass through other arguments unchanged
            translated_args.push_back(arg);
        }
    }

    return {translated_args, dpdk_logical_cores};
}

} // namespace composite
