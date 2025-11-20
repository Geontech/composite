/*
 * Copyright (C) 2025 Geon Technologies, LLC
 *
 * This file is part of composite.
 *
 * composite is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * composite is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see http://www.gnu.org/licenses/.
 */

#pragma once

#include <optional>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <vector>

namespace composite {

/**
 * @brief Get CPU affinity for current process from cgroups
 * @return cpu_set_t with CPUs assigned by cgroups/container, or nullopt on failure
 *
 * Tries cgroup v2 first, then v1, then falls back to sched_getaffinity
 */
auto get_available_cpus() -> std::optional<cpu_set_t>;

/**
 * @brief Parse a cpuset string into a cpu_set_t
 * @param cpuset_str String like "0-3,5,7-9" (physical CPU indices)
 * @return cpu_set_t with specified CPUs set
 */
auto parse_cpuset(const std::string& cpuset_str) -> cpu_set_t;

/**
 * @brief Parse a cpu_affinity configuration string and map to physical cores
 * @param affinity_str Configuration string ("all", "none", "0-3", "1,3-4", etc.)
 * @param available_cores Vector of physical CPU IDs available to the application
 * @return cpu_set_t with physical CPUs set, or nullopt if "none" or error
 *
 * Configuration format supports:
 * - "all": Use all available cores
 * - "none" or empty: Don't configure affinity (returns nullopt)
 * - "0-3": Range of logical core indices
 * - "1,3-4": Comma-separated list with ranges
 * - "2,3,4": Comma-separated individual indices
 *
 * Logical indices (0, 1, 2, ...) map to physical cores in available_cores.
 * For example, if available_cores = [7, 9, 51, 53]:
 * - Logical 0 -> Physical 7
 * - Logical 1 -> Physical 9
 * - Logical 2 -> Physical 51
 * - Logical 3 -> Physical 53
 */
auto parse_affinity_config(
    const std::string& affinity_str,
    const std::vector<int>& available_cores
) -> std::optional<cpu_set_t>;

/**
 * @brief Translate DPDK EAL args from logical to physical core indices
 * @param eal_args DPDK EAL arguments with logical core indices
 * @param available_cores Physical core IDs available to the application
 * @return Translated EAL args with physical core IDs, and list of logical DPDK cores
 *
 * Translates -l and --lcores arguments from logical (0-N) to physical core IDs.
 * This allows portable configs that work across different K8s core assignments.
 *
 * Example:
 * - Input: ["-l", "0-1"], available_cores=[7,9,51,53]
 * - Output: {["-l", "7,9"], [0,1]}
 *
 * Supported formats:
 * - "-l 0-3" or "-l 0,2,4"
 * - "--lcores (0-1)@(0-1)" (maps lcore group to physical group)
 */
auto translate_dpdk_eal_args(
    const std::vector<std::string>& eal_args,
    const std::vector<int>& available_cores
) -> std::pair<std::vector<std::string>, std::vector<int>>;

} // namespace composite
