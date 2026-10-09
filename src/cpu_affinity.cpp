/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "composite/util/cpu_affinity.hpp"
#include "cpu_topology.hpp"

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
            for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
                if (CPU_ISSET(cpu, &(*cpuset_opt))) {
                    out.push_back(cpu);
                }
            }
        }
        return out;
    }();
    return cores;
}

namespace {

auto read_cpu_siblings(int cpu) -> std::optional<std::vector<int>> {
    const auto line = read_first_line("/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
                                      "/topology/thread_siblings_list");
    if (!line) return std::nullopt;
    try {
        const auto cpuset = parse_cpuset(*line);
        std::vector<int> siblings;
        for (int id = 0; id < CPU_SETSIZE; ++id) {
            if (CPU_ISSET(id, &cpuset)) siblings.push_back(id);
        }
        return siblings;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace

auto dpdk_cpu_allocation_is_smt_safe(const std::vector<int>& dpdk_cores,
                                     const std::vector<int>& available_cores) -> bool {
    return dpdk_physical_core_representatives(dpdk_cores, available_cores).has_value();
}

auto dpdk_physical_core_representatives(const std::vector<int>& dpdk_cores,
                                        const std::vector<int>& available_cores)
    -> std::optional<std::vector<int>> {
    return detail::dpdk_physical_core_representatives(dpdk_cores, available_cores, read_cpu_siblings);
}

auto plan_dpdk_cpu_cores(const std::vector<int>& available_cores,
                         size_t dpdk_physical_core_count,
                         size_t minimum_application_physical_cores) -> std::optional<dpdk_cpu_plan> {
    return detail::plan_dpdk_cpu_cores(available_cores, dpdk_physical_core_count,
                                      minimum_application_physical_cores, read_cpu_siblings);
}

auto translate_dpdk_eal_args(const std::vector<std::string>& eal_args, const std::vector<int>& available_cores)
    -> std::pair<std::vector<std::string>, std::vector<int>> {
    std::vector<std::string> translated_args;
    std::vector<int> dpdk_logical_cores;
    bool has_selection = false;
    for (size_t i = 0; i < eal_args.size(); ++i) {
        const auto& arg = eal_args[i];
        if (arg == "--lcores" || arg.starts_with("--lcores=")) {
            throw std::invalid_argument("DPDK --lcores is not supported: its core ids would bypass the "
                                        "logical->physical translation applied to -l. Express the core list "
                                        "with -l instead.");
        }
        const bool is_list = arg.starts_with("-l");
        const bool is_mask = arg.starts_with("-c");
        if (!is_list && !is_mask) {
            translated_args.push_back(arg);
            continue;
        }
        if (has_selection) {
            throw std::invalid_argument("Specify only one DPDK CPU selection (-l or -c)");
        }
        has_selection = true;
        if (arg.size() == 2 && i + 1 == eal_args.size()) {
            throw std::invalid_argument("Missing value for DPDK " + arg);
        }
        const std::string value = arg.size() == 2 ? eal_args[++i] : arg.substr(2);
        std::vector<int> physical_cores;
        if (is_mask) {
            // -c has historically passed through to EAL: its bits name PHYSICAL CPUs.
            // Normalize to -l without changing that meaning, and recover logical indices
            // so the CLI can validate SMT safety and reserve the same CPUs from application work.
            auto hex = value;
            if (hex.starts_with("0x") || hex.starts_with("0X")) hex.erase(0, 2);
            if (hex.empty()) throw std::invalid_argument("Empty DPDK -c core mask");
            for (size_t digit = 0; digit < hex.size(); ++digit) {
                const char ch = hex[hex.size() - 1 - digit];
                const int nibble = ch >= '0' && ch <= '9' ? ch - '0'
                                 : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10
                                 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
                if (nibble < 0) throw std::invalid_argument("Invalid DPDK -c core mask '" + value + "'");
                for (size_t bit = 0; bit < 4; ++bit) {
                    if (!(nibble & (1 << bit))) continue;
                    if (digit >= CPU_SETSIZE / 4) {
                        throw std::invalid_argument("DPDK -c core mask exceeds CPU_SETSIZE");
                    }
                    const int cpu = static_cast<int>(digit * 4 + bit);
                    const auto it = std::find(available_cores.begin(), available_cores.end(), cpu);
                    if (it == available_cores.end()) {
                        throw std::invalid_argument("DPDK -c selects unavailable physical CPU " + std::to_string(cpu));
                    }
                    physical_cores.push_back(cpu);
                    dpdk_logical_cores.push_back(static_cast<int>(std::distance(available_cores.begin(), it)));
                }
            }
        } else {
            const auto parse_core = [](const std::string& text) -> int {
                try {
                    std::size_t consumed = 0;
                    const int v = std::stoi(text, &consumed);
                    if (v < 0 || consumed != text.size()) throw std::invalid_argument("");
                    return v;
                } catch (const std::exception&) {
                    throw std::invalid_argument("invalid DPDK lcore token '" + text + "' in -l list");
                }
            };
            const auto add_core = [&](int logical) {
                if (static_cast<size_t>(logical) < available_cores.size()) {
                    physical_cores.push_back(available_cores[logical]);
                    dpdk_logical_cores.push_back(logical);
                } else {
                    spdlog::error("DPDK lcore logical index {} out of range (have {} cores)", logical,
                                  available_cores.size());
                }
            };
            std::istringstream ss(value);
            std::string token;
            while (std::getline(ss, token, ',')) {
                token.erase(std::remove_if(token.begin(), token.end(), ::isspace), token.end());
                if (token.empty()) continue;
                const auto dash = token.find('-');
                if (dash == std::string::npos) {
                    add_core(parse_core(token));
                } else {
                    const int start = parse_core(token.substr(0, dash));
                    const int end = parse_core(token.substr(dash + 1));
                    if (end < start) throw std::invalid_argument("Descending DPDK -l range '" + token + "'");
                    // Bound expansion by the available logical indices (also avoids INT_MAX overflow).
                    for (size_t logical = static_cast<size_t>(start);
                         logical <= static_cast<size_t>(end) && logical < available_cores.size(); ++logical) {
                        add_core(static_cast<int>(logical));
                    }
                    if (static_cast<size_t>(end) >= available_cores.size()) add_core(end);
                }
            }
        }
        if (physical_cores.empty()) {
            throw std::invalid_argument("DPDK CPU selection contains no available CPUs");
        }
        std::string physical_list;
        for (const int cpu : physical_cores) {
            if (!physical_list.empty()) physical_list += ",";
            physical_list += std::to_string(cpu);
        }
        translated_args.insert(translated_args.end(), {"-l", physical_list});
    }
    return {translated_args, dpdk_logical_cores};
}

} // namespace composite
