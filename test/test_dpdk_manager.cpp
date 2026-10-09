/*
 * Copyright (C) 2026 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

// Exercise real EAL and manager queue assignments without PCI devices or hugepages.
// net_ring is shipped by the Rocky 9 DPDK package used in CI; net_null is not.
#include "composite/dpdk/manager.hpp"
#include "composite/util/cpu_affinity.hpp"

#include <rte_lcore.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

int main() {
    const auto& available = composite::process_available_cores(); // capture before EAL changes affinity
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    if (sched_getaffinity(0, sizeof(affinity), &affinity) != 0) return 77;
    std::vector<int> usable;
    for (const int cpu : available) {
        if (cpu < RTE_MAX_LCORE && CPU_ISSET(cpu, &affinity)) usable.push_back(cpu);
    }
    auto plan = composite::plan_dpdk_cpu_cores(usable, 2, 0);
    if (!plan) plan = composite::plan_dpdk_cpu_cores(usable, 1, 0);
    if (!plan) {
        std::puts("SKIP: EAL test needs at least one complete physical core within RTE_MAX_LCORE");
        return 77;
    }
    std::string cpu_list;
    for (const int cpu : plan->dpdk_cores) {
        if (!cpu_list.empty()) cpu_list += ",";
        cpu_list += std::to_string(cpu);
    }

    char runtime_template[] = "/tmp/composite-dpdk-test-XXXXXX";
    const char* runtime = mkdtemp(runtime_template);
    assert(runtime != nullptr);
    assert(setenv("XDG_RUNTIME_DIR", runtime, 1) == 0);
    const auto prefix = std::filesystem::path(runtime).filename().string();
    composite::dpdk::config config;
    config.eal_args = {
        "-l",   cpu_list,           "--no-huge", "--no-shconf", "--no-pci", "--no-telemetry", "--file-prefix",
        prefix, "--vdev=net_ring0", "-m",        "64"};
    composite::dpdk::port_config port;
    port.interface = "virtual-rx";
    port.rx_queues = static_cast<uint16_t>(plan->rx_worker_cores.size() + 1);
    port.mempool_size = 2047;
    config.ports.push_back(port);

    // Drive the real CLI as well as the manager: translator-only tests would miss a
    // regression where main() prepends an automatic -l before looking for an explicit -c.
    std::string mask(static_cast<size_t>(plan->dpdk_cores.back() / 4 + 1), '0');
    for (const int cpu : plan->dpdk_cores) {
        auto& digit = mask[mask.size() - 1 - static_cast<size_t>(cpu / 4)];
        const int value = digit <= '9' ? digit - '0' : digit - 'a' + 10;
        digit = "0123456789abcdef"[value | (1 << (cpu % 4))];
    }
    auto cli_args = config.eal_args;
    cli_args[0] = "-c";
    cli_args[1] = mask;
    const auto config_path = std::filesystem::path(runtime) / "app.json";
    const auto log_path = std::filesystem::path(runtime) / "cli.log";
    // Missing components deliberately exits immediately AFTER DPDK initialization,
    // without opening a REST socket. The explicit selection overrides this large minimum.
    const nlohmann::json app{
        {"dpdk",
         {{"enabled", true},
          {"eal_args", cli_args},
          {"minimum_application_physical_cores", 65535},
          {"ports",
           {{{"interface", port.interface}, {"rx_queues", port.rx_queues}, {"mempool_size", port.mempool_size}}}}}}};
    std::ofstream(config_path) << app;
    const pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        const int log = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (log < 0 || dup2(log, STDOUT_FILENO) < 0 || dup2(log, STDERR_FILENO) < 0) _exit(126);
        close(log);
        execl(TEST_DPDK_CLI_PATH, TEST_DPDK_CLI_PATH, config_path.c_str(), nullptr);
        _exit(127);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    std::ifstream log(log_path);
    const std::string output{std::istreambuf_iterator<char>{log}, std::istreambuf_iterator<char>{}};
    if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_FAILURE ||
        output.find("DPDK initialized with 1 port(s)") == std::string::npos ||
        output.find("config file missing 'components' field") == std::string::npos) {
        std::fprintf(stderr, "CLI core-mask regression failed:\n%s\n", output.c_str());
        std::filesystem::remove_all(runtime);
        return 1;
    }

    auto& manager = composite::dpdk::manager::instance();
    assert(!manager.get_rx_worker_core(port.interface, 0));
    assert(manager.initialize(config));
    for (uint16_t queue = 0; queue < port.rx_queues; ++queue) {
        assert(manager.get_rx_worker_core(port.interface, queue) ==
               plan->rx_worker_cores[queue % plan->rx_worker_cores.size()]);
    }
    assert(!manager.get_rx_worker_core(port.interface, port.rx_queues));
    assert(!manager.get_rx_worker_core("unknown", 0));
    manager.shutdown();
    assert(!manager.get_rx_worker_core(port.interface, 0));
    std::filesystem::remove_all(runtime);
    std::puts("DPDK virtual-NIC queue assignments and shutdown passed");
}
