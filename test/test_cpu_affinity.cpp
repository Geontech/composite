/*
 * Copyright (C) 2026 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

// translate_dpdk_eal_args: logical->physical core translation for DPDK EAL args.
//
// 0.5.1 regressions pinned here:
//  - "--lcores" is REJECTED (std::invalid_argument), not passed through: its core ids skip the
//    logical->physical translation applied to -l, which silently mixed two coordinate systems
//    in one config (the -l list logical, the --lcores list physical).
//  - an unparsable token in an -l list throws std::invalid_argument naming the token, instead
//    of std::stoi escaping through main() to std::terminate.

#include "composite/util/cpu_affinity.hpp"

#include <cstdio>
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
