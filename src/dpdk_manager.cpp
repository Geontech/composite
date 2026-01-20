/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifdef COMPOSITE_USE_DPDK

#include "composite/dpdk/manager.hpp"

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <vector>
#include <pthread.h>
#include <sched.h>
#include <string>

namespace composite::dpdk {

auto manager::instance() -> manager& {
    static manager instance;
    return instance;
}

auto manager::initialize(const dpdk::config& config) -> bool {
    if (m_initialized) {
        spdlog::warn("DPDK already initialized");
        return true;
    }

    spdlog::info("Initializing DPDK...");

    auto cleanup_partial = [this]() {
        for (uint16_t port_id : m_configured_ports) {
            int ret = rte_eth_dev_stop(port_id);
            if (ret != 0) {
                spdlog::warn("Failed to stop DPDK port {}: {}", port_id, rte_strerror(-ret));
            }
            ret = rte_eth_dev_close(port_id);
            if (ret != 0) {
                spdlog::warn("Failed to close DPDK port {}: {}", port_id, rte_strerror(-ret));
            }
        }
        m_mempools.clear();
        m_interface_to_port.clear();
        m_configured_ports.clear();
        int ret = rte_eal_cleanup();
        if (ret != 0) {
            spdlog::warn("DPDK EAL cleanup returned: {}", ret);
        }
    };

    // Initialize EAL
    if (!init_eal(config.eal_args)) {
        spdlog::error("Failed to initialize DPDK EAL");
        return false;
    }

    // Log available ports
    uint16_t nb_ports = rte_eth_dev_count_avail();
    spdlog::info("DPDK found {} available ports", nb_ports);

    // Configure each port
    for (const auto& port_config : config.ports) {
        if (!configure_port(port_config)) {
            spdlog::error("Failed to configure DPDK port {} ({})",
                         port_config.port_id, port_config.interface);
            cleanup_partial();
            return false;
        }

        spdlog::info("Configured DPDK port {} (interface={}, rx_queues={}, mempool={})",
                     port_config.port_id,
                     port_config.interface,
                     port_config.rx_queues,
                     port_config.mempool_name);
    }

    m_initialized = true;
    spdlog::info("DPDK initialization complete");
    return true;
}

auto manager::shutdown() -> void {
    if (!m_initialized) {
        return;
    }

    spdlog::info("Shutting down DPDK...");

    // Stop and close all configured ports
    for (uint16_t port_id : m_configured_ports) {
        int ret = rte_eth_dev_stop(port_id);
        if (ret != 0) {
            spdlog::warn("Failed to stop DPDK port {}: {}", port_id, rte_strerror(-ret));
        }

        ret = rte_eth_dev_close(port_id);
        if (ret != 0) {
            spdlog::warn("Failed to close DPDK port {}: {}", port_id, rte_strerror(-ret));
        }
    }

    // Note: mempools are freed automatically by DPDK on cleanup
    m_mempools.clear();
    m_mempool_configs.clear();
    m_interface_to_port.clear();
    m_configured_ports.clear();

    // Clean up EAL
    int ret = rte_eal_cleanup();
    if (ret != 0) {
        spdlog::warn("DPDK EAL cleanup returned: {}", ret);
    }

    m_initialized = false;
    spdlog::info("DPDK shutdown complete");
}

auto manager::init_eal(const std::vector<std::string>& eal_args) -> bool {
    // Build arg storage first to prevent reallocation
    std::vector<std::string> arg_storage;

    // First argument is program name
    arg_storage.push_back("composite-cli");

    // Add user-provided EAL arguments
    for (const auto& arg : eal_args) {
        arg_storage.push_back(arg);
    }

    // Now build argv array with stable pointers
    std::vector<char*> argv;
    for (auto& arg : arg_storage) {
        argv.push_back(const_cast<char*>(arg.c_str()));
    }

    // Log the EAL arguments
    std::string eal_args_str;
    for (const auto* arg : argv) {
        eal_args_str += std::string(arg) + " ";
    }
    spdlog::debug("DPDK EAL args: {}", eal_args_str);

    // Initialize EAL
    int ret = rte_eal_init(static_cast<int>(argv.size()), argv.data());
    if (ret < 0) {
        spdlog::error("DPDK EAL initialization failed: {}", rte_strerror(rte_errno));
        return false;
    }

    spdlog::debug("DPDK EAL initialized, {} arguments consumed", ret);

    return true;
}

auto manager::get_dpdk_lcores() const -> std::vector<int> {
    if (!m_initialized) {
        spdlog::warn("DPDK not initialized, cannot query lcores");
        return {};
    }

    std::vector<int> lcores;
    unsigned int lcore_id;

    // Iterate through all DPDK lcores
    RTE_LCORE_FOREACH(lcore_id) {
        lcores.push_back(static_cast<int>(lcore_id));
    }

    return lcores;
}

auto manager::list_available_ports() const -> std::vector<port_summary> {
    std::vector<port_summary> ports;
    uint16_t nb_ports = rte_eth_dev_count_avail();
    ports.reserve(nb_ports);

    for (uint16_t port_id = 0; port_id < nb_ports; ++port_id) {
        if (!rte_eth_dev_is_valid_port(port_id)) {
            continue;
        }

        rte_eth_dev_info dev_info;
        int ret = rte_eth_dev_info_get(port_id, &dev_info);
        if (ret != 0) {
            spdlog::warn("Failed to get device info for port {}: {}",
                port_id, rte_strerror(-ret));
            continue;
        }

        port_summary summary{};
        summary.port_id = port_id;
        summary.driver_name = dev_info.driver_name ? dev_info.driver_name : "unknown";
        summary.max_rx_queues = dev_info.max_rx_queues;
        summary.max_tx_queues = dev_info.max_tx_queues;
        summary.socket_id = rte_eth_dev_socket_id(port_id);
        ports.push_back(std::move(summary));
    }

    return ports;
}

auto manager::configure_port(const port_config& config) -> bool {
    uint16_t port_id = config.port_id;

    // Validate port exists
    if (!rte_eth_dev_is_valid_port(port_id)) {
        spdlog::error("DPDK port {} is not valid", port_id);
        return false;
    }

    // Get device info
    rte_eth_dev_info dev_info;
    int ret = rte_eth_dev_info_get(port_id, &dev_info);
    if (ret != 0) {
        spdlog::error("Failed to get device info for port {}: {}",
                     port_id, rte_strerror(-ret));
        return false;
    }

    // Log device information
    spdlog::debug("Port {}: driver={}, max_rx_queues={}, max_tx_queues={}",
                  port_id, dev_info.driver_name,
                  dev_info.max_rx_queues, dev_info.max_tx_queues);

    // Validate queue counts
    if (config.rx_queues > dev_info.max_rx_queues) {
        spdlog::error("Port {} requested {} RX queues but only {} supported",
                     port_id, config.rx_queues, dev_info.max_rx_queues);
        return false;
    }

    if (config.tx_queues > dev_info.max_tx_queues) {
        spdlog::error("Port {} requested {} TX queues but only {} supported",
                     port_id, config.tx_queues, dev_info.max_tx_queues);
        return false;
    }

    // Create mempool for this port
    rte_mempool* mempool = create_mempool(config);
    if (!mempool) {
        spdlog::error("Failed to create mempool for port {}", port_id);
        return false;
    }

    // Configure port with default settings
    rte_eth_conf port_conf = {};
    port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
    port_conf.rxmode.mtu = 9000;  // Jumbo frames for large UDP payloads
    port_conf.txmode.mq_mode = RTE_ETH_MQ_TX_NONE;

    ret = rte_eth_dev_configure(port_id, config.rx_queues, config.tx_queues, &port_conf);
    if (ret != 0) {
        spdlog::error("Failed to configure port {}: {}", port_id, rte_strerror(-ret));
        return false;
    }

    // Setup RX queues (use driver defaults via nullptr)
    for (uint16_t q = 0; q < config.rx_queues; q++) {
        ret = rte_eth_rx_queue_setup(
            port_id,
            q,
            config.rx_descriptors,
            rte_eth_dev_socket_id(port_id),
            nullptr,  // Use driver default RX config
            mempool
        );

        if (ret != 0) {
            spdlog::error("Failed to setup RX queue {} on port {}: {}",
                         q, port_id, rte_strerror(-ret));
            return false;
        }
    }

    // Setup TX queues
    for (uint16_t q = 0; q < config.tx_queues; q++) {
        ret = rte_eth_tx_queue_setup(
            port_id,
            q,
            config.tx_descriptors,
            rte_eth_dev_socket_id(port_id),
            nullptr  // Use default TX config
        );

        if (ret != 0) {
            spdlog::error("Failed to setup TX queue {} on port {}: {}",
                         q, port_id, rte_strerror(-ret));
            return false;
        }
    }

    // Start the port
    ret = rte_eth_dev_start(port_id);
    if (ret != 0) {
        spdlog::error("Failed to start port {}: {}", port_id, rte_strerror(-ret));
        return false;
    }
    spdlog::info("Port {} started with hardware MAC filtering (promiscuous=off, allmulticast=off)", port_id);

    // Store port information for later lookup
    port_info info;
    info.port_id = port_id;
    info.interface_name = config.interface;
    info.num_rx_queues = config.rx_queues;
    info.queue_allocated.resize(config.rx_queues, false);
    info.mempool = mempool;

    m_interface_to_port[config.interface] = info;
    m_configured_ports.push_back(port_id);

    return true;
}

auto manager::create_mempool(const port_config& config) -> rte_mempool* {
    // Check if mempool already exists
    rte_mempool* existing = rte_mempool_lookup(config.mempool_name.c_str());
    if (existing) {
        auto it = m_mempool_configs.find(config.mempool_name);
        if (it != m_mempool_configs.end()) {
            const auto& existing_cfg = it->second;
            if (existing_cfg.mempool_size != config.mempool_size ||
                existing_cfg.mempool_cache_size != config.mempool_cache_size ||
                existing_cfg.mbuf_data_room_size != config.mbuf_data_room_size) {
                spdlog::warn(
                    "Mempool '{}' already exists with different settings "
                    "(size={}, cache={}, room={}); requested size={}, cache={}, room={}",
                    config.mempool_name,
                    existing_cfg.mempool_size,
                    existing_cfg.mempool_cache_size,
                    existing_cfg.mbuf_data_room_size,
                    config.mempool_size,
                    config.mempool_cache_size,
                    config.mbuf_data_room_size);
            }
        } else {
            spdlog::warn("Reusing existing mempool '{}' without known config; "
                         "settings are not verified", config.mempool_name);
            m_mempool_configs[config.mempool_name] = {
                config.mempool_size,
                config.mempool_cache_size,
                config.mbuf_data_room_size
            };
        }
        spdlog::debug("Reusing existing mempool '{}'", config.mempool_name);
        m_mempools[config.mempool_name] = existing;
        return existing;
    }

    // Create new mempool
    rte_mempool* pool = rte_pktmbuf_pool_create(
        config.mempool_name.c_str(),
        config.mempool_size,
        config.mempool_cache_size,
        0,  // private data size
        config.mbuf_data_room_size,
        rte_socket_id()
    );

    if (!pool) {
        spdlog::error("Failed to create mempool '{}': {}",
                     config.mempool_name, rte_strerror(rte_errno));
        return nullptr;
    }

    m_mempools[config.mempool_name] = pool;
    m_mempool_configs[config.mempool_name] = {
        config.mempool_size,
        config.mempool_cache_size,
        config.mbuf_data_room_size
    };
    spdlog::debug("Created mempool '{}' with {} mbufs (data_room={})",
                  config.mempool_name, config.mempool_size, config.mbuf_data_room_size);

    return pool;
}

auto manager::get_port_id_for_interface(const std::string& interface_name) const
    -> std::optional<uint16_t> {
    auto it = m_interface_to_port.find(interface_name);
    if (it != m_interface_to_port.end()) {
        return it->second.port_id;
    }
    return std::nullopt;
}

auto manager::get_mempool(const std::string& name) const -> rte_mempool* {
    auto it = m_mempools.find(name);
    return (it != m_mempools.end()) ? it->second : nullptr;
}

auto manager::is_port_configured(uint16_t port_id) const -> bool {
    return std::find(m_configured_ports.begin(), m_configured_ports.end(), port_id)
           != m_configured_ports.end();
}

auto manager::is_queue_available(const std::string& interface_name, uint16_t queue_id) const
    -> bool {
    auto it = m_interface_to_port.find(interface_name);
    if (it == m_interface_to_port.end()) {
        return false;
    }

    const auto& port = it->second;
    if (queue_id >= port.num_rx_queues) {
        return false;
    }

    return !port.queue_allocated[queue_id];
}

auto manager::allocate_queue(const std::string& interface_name, uint16_t queue_id) -> bool {
    auto it = m_interface_to_port.find(interface_name);
    if (it == m_interface_to_port.end()) {
        spdlog::error("Interface '{}' not found in DPDK configuration", interface_name);
        return false;
    }

    auto& port = it->second;

    if (queue_id >= port.num_rx_queues) {
        spdlog::error("Queue {} does not exist on interface '{}' (has {} queues)",
                     queue_id, interface_name, port.num_rx_queues);
        return false;
    }

    if (port.queue_allocated[queue_id]) {
        spdlog::error("Queue {} on interface '{}' is already allocated",
                     queue_id, interface_name);
        return false;
    }

    port.queue_allocated[queue_id] = true;
    spdlog::debug("Allocated queue {} on interface '{}'", queue_id, interface_name);
    return true;
}

auto manager::allocate_next_available_queue(const std::string& interface_name)
    -> std::optional<uint16_t> {
    auto it = m_interface_to_port.find(interface_name);
    if (it == m_interface_to_port.end()) {
        spdlog::error("Interface '{}' not found in DPDK configuration", interface_name);
        return std::nullopt;
    }

    auto& port = it->second;

    // Find first unallocated queue
    for (uint16_t q = 0; q < port.num_rx_queues; q++) {
        if (!port.queue_allocated[q]) {
            port.queue_allocated[q] = true;
            spdlog::debug("Auto-allocated queue {} on interface '{}'", q, interface_name);
            return q;
        }
    }

    spdlog::error("No available queues on interface '{}' (all {} queues allocated)",
                 interface_name, port.num_rx_queues);
    return std::nullopt;
}

auto manager::release_queue(const std::string& interface_name, uint16_t queue_id) -> void {
    auto it = m_interface_to_port.find(interface_name);
    if (it == m_interface_to_port.end()) {
        spdlog::warn("Attempted to release queue on unknown interface '{}'", interface_name);
        return;
    }

    auto& port = it->second;

    if (queue_id >= port.num_rx_queues) {
        spdlog::warn("Attempted to release invalid queue {} on interface '{}'",
                    queue_id, interface_name);
        return;
    }

    if (!port.queue_allocated[queue_id]) {
        spdlog::warn("Attempted to release queue {} on interface '{}' which was not allocated",
                    queue_id, interface_name);
        return;
    }

    port.queue_allocated[queue_id] = false;
    spdlog::debug("Released queue {} on interface '{}'", queue_id, interface_name);
}

} // namespace composite

#endif // COMPOSITE_USE_DPDK
