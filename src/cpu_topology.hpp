/*
 * Copyright (C) 2026 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "composite/util/cpu_affinity.hpp"

#include <algorithm>
#include <set>
#include <spdlog/spdlog.h>

namespace composite::detail {

// The reader returns physical sibling IDs, or nullopt for unavailable/invalid topology.
// Keeping this boundary independent of sysfs lets tests exercise arbitrary CPU allocations.
template <typename ReadSiblings>
auto dpdk_physical_core_representatives(const std::vector<int>& dpdk_cores, const std::vector<int>& available_cores,
                                        ReadSiblings read_siblings) -> std::optional<std::vector<int>> {
    const std::set<int> available(available_cores.begin(), available_cores.end());
    const std::set<int> selected(dpdk_cores.begin(), dpdk_cores.end());
    std::set<int> grouped;
    std::vector<int> representatives;
    for (const int cpu : selected) {
        if (grouped.contains(cpu)) continue;
        const auto siblings = read_siblings(cpu);
        if (!available.contains(cpu) || !siblings ||
            std::find(siblings->begin(), siblings->end(), cpu) == siblings->end()) {
            spdlog::error("Cannot determine usable SMT topology for DPDK CPU {}", cpu);
            return std::nullopt;
        }
        for (const int sibling : *siblings) {
            if (!available.contains(sibling)) continue;
            if (!selected.contains(sibling)) {
                spdlog::error("DPDK CPU {} is missing available SMT sibling {}", cpu, sibling);
                return std::nullopt;
            }
            grouped.insert(sibling);
        }
        representatives.push_back(cpu);
    }
    return representatives;
}

template <typename ReadSiblings>
auto plan_dpdk_cpu_cores(const std::vector<int>& available_cores, size_t dpdk_physical_core_count,
                         size_t minimum_application_physical_cores, ReadSiblings read_siblings)
    -> std::optional<dpdk_cpu_plan> {
    if (available_cores.empty() || dpdk_physical_core_count == 0) {
        spdlog::error("Cannot plan DPDK CPUs: an available CPU set and at least one RX queue are required");
        return std::nullopt;
    }
    const std::set<int> available(available_cores.begin(), available_cores.end());
    std::set<int> grouped;
    std::vector<std::vector<int>> core_groups;
    dpdk_cpu_plan plan;
    for (const int cpu : available) {
        if (grouped.contains(cpu)) continue;
        const auto siblings = read_siblings(cpu);
        if (!siblings || std::find(siblings->begin(), siblings->end(), cpu) == siblings->end()) {
            spdlog::error("Cannot determine SMT topology for CPU {}", cpu);
            return std::nullopt;
        }
        std::vector<int> group;
        bool complete = true;
        for (const int sibling : *siblings) {
            if (available.contains(sibling)) {
                group.push_back(sibling);
            } else {
                complete = false;
            }
        }
        std::sort(group.begin(), group.end());
        group.erase(std::unique(group.begin(), group.end()), group.end());
        grouped.insert(group.begin(), group.end());
        if (complete) {
            core_groups.push_back(std::move(group));
        } else {
            // Partial cores cannot satisfy either complete-core reservation, but remain useful
            // to the application and must not be lost from its affinity mask.
            plan.application_cores.insert(plan.application_cores.end(), group.begin(), group.end());
        }
    }
    // Subtraction avoids overflow for callers of the public size_t API.
    if (dpdk_physical_core_count > core_groups.size() ||
        minimum_application_physical_cores > core_groups.size() - dpdk_physical_core_count) {
        spdlog::error("DPDK CPU plan requires {} complete physical core(s) for receive queues and {} for "
                      "application work, but the process cpuset contains only {} complete core(s)",
                      dpdk_physical_core_count, minimum_application_physical_cores, core_groups.size());
        return std::nullopt;
    }
    for (size_t i = 0; i < core_groups.size(); ++i) {
        const auto& group = core_groups[i];
        if (i < dpdk_physical_core_count) {
            plan.rx_worker_cores.push_back(group.front());
            plan.dpdk_cores.insert(plan.dpdk_cores.end(), group.begin(), group.end());
        } else {
            plan.application_cores.insert(plan.application_cores.end(), group.begin(), group.end());
        }
    }
    std::sort(plan.dpdk_cores.begin(), plan.dpdk_cores.end());
    std::sort(plan.application_cores.begin(), plan.application_cores.end());
    return plan;
}

} // namespace composite::detail
