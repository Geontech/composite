/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "composite/util/export.hpp"

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
COMPOSITE_API auto get_available_cpus() -> std::optional<cpu_set_t>;

/**
 * @brief Physical CPU ids available to this process, captured ONCE on first use.
 *
 * The sorted expansion of get_available_cpus() (cgroup/affinity mask). Shared by every
 * component-creation path — the config loader and POST /app/components — so both resolve a
 * "cpu_affinity" value against the same view of the machine. Empty when the mask could not
 * be read (affinity configuration is then unavailable, matching the loader's behavior).
 */
COMPOSITE_API auto process_available_cores() -> const std::vector<int>&;

/**
 * @brief DPDK/app CPU partition selected from complete physical cores in the process cpuset.
 */
struct dpdk_cpu_plan {
    std::vector<int> dpdk_cores;
    std::vector<int> rx_worker_cores;
    std::vector<int> application_cores;
};

/**
 * @brief Partition the process cpuset into complete physical cores for DPDK and application work.
 *
 * DPDK receives dpdk_physical_core_count complete SMT sibling groups. All remaining complete
 * groups are available to the application, provided at least minimum_application_physical_cores
 * remain. Partial groups are also retained for application work but do not count toward that
 * minimum. NUMA placement is deliberately left to the container orchestrator.
 */
COMPOSITE_API auto plan_dpdk_cpu_cores(const std::vector<int>& available_cores,
                                       size_t dpdk_physical_core_count,
                                       size_t minimum_application_physical_cores) -> std::optional<dpdk_cpu_plan>;

/**
 * @brief Check that an explicit DPDK lcore list includes every available SMT sibling.
 * Returns false if the selected CPUs or their topology are unavailable.
 */
COMPOSITE_API auto dpdk_cpu_allocation_is_smt_safe(const std::vector<int>& dpdk_cores,
                                                   const std::vector<int>& available_cores) -> bool;

/**
 * @brief Select one worker CPU from each complete physical core in an SMT-safe allocation.
 */
COMPOSITE_API auto dpdk_physical_core_representatives(const std::vector<int>& dpdk_cores,
                                                      const std::vector<int>& available_cores)
    -> std::optional<std::vector<int>>;

/**
 * @brief Parse a cpuset string into a cpu_set_t
 * @param cpuset_str String like "0-3,5,7-9" (physical CPU indices)
 * @return cpu_set_t with specified CPUs set
 */
COMPOSITE_API auto parse_cpuset(const std::string& cpuset_str) -> cpu_set_t;

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
COMPOSITE_API auto parse_affinity_config(const std::string& affinity_str, const std::vector<int>& available_cores)
    -> std::optional<cpu_set_t>;

/**
 * @brief Translate DPDK EAL args from logical to physical core indices
 * @param eal_args DPDK EAL arguments with logical core indices
 * @param available_cores Physical core IDs available to the application
 * @return Translated EAL args with physical core IDs, and list of logical DPDK cores
 *
 * Translates -l arguments from logical (0-N) to physical core IDs, allowing portable
 * configs across different K8s core assignments. A -c hexadecimal mask retains EAL's
 * physical CPU numbering and is normalized to -l; every selected bit must be available.
 * An empty returned logical list means no explicit selection was supplied.
 *
 * Example:
 * - Input: ["-l", "0-1"], available_cores=[7,9,51,53]
 * - Output: {["-l", "7,9"], [0,1]}
 *
 * Supported formats:
 * - Separate or attached values: {"-l", "0-3"}, {"-l0,2,4"}, {"-c", "0x101"}, {"-c101"}
 *
 * @throws std::invalid_argument for "--lcores" (its "(lcores)@(cpus)" grammar is not
 *         translated, and passing it through would silently mix physical ids with the
 *         logical ids -l users write — express the list with -l), and for an unparsable
 *         token in an -l list. Missing, empty, duplicate, and invalid masks are also rejected.
 */
COMPOSITE_API auto translate_dpdk_eal_args(const std::vector<std::string>& eal_args, const std::vector<int>& available_cores)
    -> std::pair<std::vector<std::string>, std::vector<int>>;

} // namespace composite
