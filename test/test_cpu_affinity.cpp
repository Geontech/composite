/*
 * Copyright (C) 2026 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

// Deterministic CPU topology planning and EAL CPU-selector normalization.
//
// 0.5.1 regressions pinned here:
//  - "--lcores" is REJECTED (std::invalid_argument), not passed through: its core ids skip the
//    logical->physical translation applied to -l, which silently mixed two coordinate systems
//    in one config (the -l list logical, the --lcores list physical).
//  - an unparsable token in an -l list throws std::invalid_argument naming the token, instead
//    of std::stoi escaping through main() to std::terminate.

#include "composite/util/cpu_affinity.hpp"
#include "cpu_topology.hpp"

#include <cstdio>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

auto check(bool ok, const char* what) -> void {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

auto throws_invalid_argument(const std::vector<std::string>& eal_args, const std::vector<int>& cores,
                             std::string* what_out = nullptr) -> bool {
    try {
        (void)composite::translate_dpdk_eal_args(eal_args, cores);
        return false;
    } catch (const std::invalid_argument& e) {
        if (what_out != nullptr) {
            *what_out = e.what();
        }
        return true;
    }
}

} // namespace

int main() {
    const std::vector<int> cores{7, 9, 51, 53};

    { // Deterministic SMT topology, independent of the test runner's CPU allocation.
        const std::map<int, std::vector<int>> topology{
            {0, {0, 8}}, {8, {0, 8}}, {1, {1, 9}}, {9, {1, 9}}, {2, {2, 10}}, {10, {2, 10}}};
        const auto siblings = [&](int cpu) -> std::optional<std::vector<int>> {
            const auto it = topology.find(cpu);
            return it == topology.end() ? std::nullopt : std::optional{it->second};
        };
        const std::vector<int> full{0, 1, 2, 8, 9, 10};
        const auto plan = composite::detail::plan_dpdk_cpu_cores(full, 2, 1, siblings);
        check(plan && plan->dpdk_cores == std::vector<int>({0, 1, 8, 9}) &&
                  plan->rx_worker_cores == std::vector<int>({0, 1}) &&
                  plan->application_cores == std::vector<int>({2, 10}),
              "two RX queues reserve two whole cores and leave a whole application core");
        const auto partial = composite::detail::plan_dpdk_cpu_cores({0, 1, 2, 8, 9}, 1, 1, siblings);
        check(partial && partial->application_cores == std::vector<int>({1, 2, 9}),
              "partial cores remain available to application work alongside complete cores");
        check(!composite::detail::plan_dpdk_cpu_cores({0, 1, 2, 8}, 1, 1, siblings),
              "partial cores do not satisfy the minimum complete application core count");
        const auto no_minimum = composite::detail::plan_dpdk_cpu_cores({0, 1, 2, 8}, 1, 0, siblings);
        check(no_minimum && no_minimum->application_cores == std::vector<int>({1, 2}),
              "zero minimum still retains partial application cores");
        check(!composite::detail::plan_dpdk_cpu_cores({}, 1, 1, siblings), "empty allocation is rejected");
        check(!composite::detail::plan_dpdk_cpu_cores(full, 0, 1, siblings),
              "zero RX queues require an explicit selection");
        check(!composite::detail::plan_dpdk_cpu_cores(full, 4, 0, siblings), "insufficient RX cores are rejected");
        check(!composite::detail::plan_dpdk_cpu_cores(full, 1, std::numeric_limits<size_t>::max(), siblings),
              "minimum core count cannot overflow the capacity check");
        check(!composite::detail::plan_dpdk_cpu_cores({3}, 1, 0, siblings), "missing topology fails closed");
        const auto reps = composite::detail::dpdk_physical_core_representatives({9, 0, 8, 1, 0}, full, siblings);
        check(reps && *reps == std::vector<int>({0, 1}), "one sorted representative per selected physical core");
        check(!composite::detail::dpdk_physical_core_representatives({0}, full, siblings),
              "explicit selection cannot leave an available SMT sibling for application work");
        check(composite::detail::dpdk_physical_core_representatives({0}, {0, 1}, siblings).has_value(),
              "explicit selection need not include SMT siblings outside its cpuset");
        check(!composite::detail::dpdk_physical_core_representatives({3}, {3}, siblings),
              "explicit selection rejects missing topology");
        check(!composite::detail::dpdk_physical_core_representatives({8}, {0}, siblings),
              "explicit selection rejects unavailable CPUs");
    }
    {
        const auto non_smt = [](int cpu) -> std::optional<std::vector<int>> { return std::vector<int>{cpu}; };
        const auto plan = composite::detail::plan_dpdk_cpu_cores({7, 3, 5}, 1, 1, non_smt);
        check(plan && plan->dpdk_cores == std::vector<int>({3}) &&
                  plan->application_cores == std::vector<int>({5, 7}), "non-SMT CPUs each count as a complete core");
        const auto invalid = [](int) -> std::optional<std::vector<int>> { return std::vector<int>{}; };
        check(!composite::detail::plan_dpdk_cpu_cores({0}, 1, 0, invalid), "empty sibling topology is rejected");
        check(!composite::detail::dpdk_physical_core_representatives({0}, {0}, invalid),
              "representatives also reject empty sibling topology");
        check(!composite::dpdk_cpu_allocation_is_smt_safe({-1}, {-1}),
              "public SMT validator fails closed when sysfs topology is unavailable");
        check(!composite::dpdk_physical_core_representatives({-1}, {-1}),
              "public representative selection has the same sysfs failure policy");
    }

    {
        const auto [args, logical] = composite::translate_dpdk_eal_args({"-c", "0x280", "--no-pci"}, cores);
        check(args == std::vector<std::string>({"-l", "7,9", "--no-pci"}) && logical == std::vector<int>({0, 1}),
              "physical -c mask normalizes to a single EAL list and records logical reservations");
        const auto attached = composite::translate_dpdk_eal_args({"-c280"}, cores);
        check(attached.first == std::vector<std::string>({"-l", "7,9"}), "attached -c mask is recognized");
        const auto list = composite::translate_dpdk_eal_args({"-l0,2"}, cores);
        check(list.first == std::vector<std::string>({"-l", "7,51"}), "attached -l list uses logical indices");
        const auto high_mask = composite::translate_dpdk_eal_args({"-c10000000000000000"}, {64});
        check(high_mask.first == std::vector<std::string>({"-l", "64"}), "masks support physical CPUs above bit 63");
        for (const auto& args_to_reject : std::vector<std::vector<std::string>>{
                 {"-l"}, {"-l", ""}, {"-l", "2-1"}, {"-l", "99"}, {"-c"}, {"-c", "0"},
                 {"-c", "0x"}, {"-c", "xyz"}, {"-c", "1"}, {"-l", "0", "-c", "80"},
                 {"-c80", "-c280"}, {"-l0", "-l1"}}) {
            check(throws_invalid_argument(args_to_reject, cores),
                  "invalid/empty/duplicate explicit selection is rejected");
        }
    }

    { // -l range translation
        const auto [args, logical] = composite::translate_dpdk_eal_args({"-l", "0-1"}, cores);
        check(args == std::vector<std::string>({"-l", "7,9"}), "-l 0-1 translates to physical 7,9");
        check(logical == std::vector<int>({0, 1}), "-l 0-1 records logical cores 0,1");
    }
    { // -l comma list
        const auto [args, logical] = composite::translate_dpdk_eal_args({"-l", "0,2"}, cores);
        check(args == std::vector<std::string>({"-l", "7,51"}), "-l 0,2 translates to physical 7,51");
        check(logical == std::vector<int>({0, 2}), "-l 0,2 records logical cores 0,2");
    }
    { // unrelated args pass through untouched
        const auto [args, logical] = composite::translate_dpdk_eal_args({"--no-huge", "-n", "4"}, cores);
        check(args == std::vector<std::string>({"--no-huge", "-n", "4"}), "unrelated args pass through");
        check(logical.empty(), "unrelated args claim no cores");
    }
    { // out-of-range logical index: logged and skipped, valid ones kept (existing behavior)
        const auto [args, logical] = composite::translate_dpdk_eal_args({"-l", "0,9"}, cores);
        check(args == std::vector<std::string>({"-l", "7"}), "out-of-range logical index is dropped");
        check(logical == std::vector<int>({0}), "in-range logical index is kept");
    }
    { // --lcores is rejected, not silently passed through in a different coordinate system
        check(throws_invalid_argument({"--lcores", "(0-1)@(0-1)"}, cores),
              "--lcores with a value throws invalid_argument");
        check(throws_invalid_argument({"--lcores"}, cores), "bare --lcores throws invalid_argument");
        check(throws_invalid_argument({"--lcores=(0-1)@(0-1)"}, cores),
              "single-token --lcores=<map> throws invalid_argument");
    }
    { // garbage -l token throws (used to be an unguarded std::stoi escaping to std::terminate)
        std::string what;
        check(throws_invalid_argument({"-l", "0-x"}, cores, &what), "-l with a garbage range endpoint throws");
        check(what.find('x') != std::string::npos, "the error names the offending token");
        check(throws_invalid_argument({"-l", "1abc"}, cores), "-l with a trailing-garbage token throws");
    }

    if (g_failures != 0) {
        std::fprintf(stderr, "%d cpu-affinity check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ALL CPU-AFFINITY TESTS PASSED\n");
    return 0;
}
