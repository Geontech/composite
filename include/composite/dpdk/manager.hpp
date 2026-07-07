/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#ifdef COMPOSITE_USE_DPDK

#include "composite/util/export.hpp"
#include "config.hpp"
#include "port_registry.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Forward declarations to avoid pulling in DPDK headers
struct rte_mempool;

namespace composite::dpdk {

/**
 * @brief Singleton manager for DPDK initialization and resource allocation
 *
 * The manager handles DPDK Environment Abstraction Layer (EAL) initialization,
 * port configuration, mempool management, and queue allocation for components.
 *
 * Key responsibilities:
 * - Initialize DPDK EAL before components start
 * - Configure network ports and RX/TX queues
 * - Create and manage mempools
 * - Map interface names to DPDK port IDs
 * - Allocate queues to components (auto-assign or explicit)
 * - Clean up DPDK resources on shutdown
 *
 * Usage:
 * 1. Framework initializes DPDK via initialize() during application startup
 * 2. Components query port IDs via get_port_id_for_interface()
 * 3. Components allocate queues via allocate_queue() or allocate_next_available_queue()
 * 4. Components retrieve mempools via get_mempool()
 * 5. Framework shuts down DPDK via shutdown() after components stop
 */
class manager {
public:
    /**
     * @brief Get the singleton instance
     *
     * COMPOSITE_API ensures this symbol is exported from the composite library
     * and visible to all shared libraries, ensuring a single shared instance.
     */
    COMPOSITE_API
    static auto instance() -> manager&;

    /**
     * @brief Initialize DPDK EAL and configure ports
     *
     * This must be called before any components attempt to use DPDK.
     * Typically called during application initialization.
     *
     * @param config DPDK configuration including EAL args and port configs
     * @return true if initialization succeeded, false otherwise
     */
    COMPOSITE_API
    auto initialize(const dpdk::config& config) -> bool;

    /**
     * @brief Shutdown DPDK and release all resources
     *
     * This should be called after all components have stopped.
     * Typically called during application shutdown.
     */
    COMPOSITE_API
    auto shutdown() -> void;

    /**
     * @brief Check if DPDK has been initialized
     *
     * @return true if DPDK is initialized and ready for use
     */
    COMPOSITE_API
    auto is_initialized() const -> bool { return m_initialized.load(std::memory_order_acquire); }

    /**
     * @brief Get physical CPU cores used by DPDK as lcores
     *
     * Must be called after initialize(). Returns the actual physical CPU IDs
     * that DPDK is using for lcores, regardless of how they were configured.
     *
     * @return Vector of physical CPU core IDs used by DPDK
     */
    COMPOSITE_API
    auto get_dpdk_lcores() const -> std::vector<int>;

    /**
     * @brief Get DPDK port ID for a given interface name
     *
     * Maps Linux interface names (e.g., "eth0") to DPDK port IDs.
     *
     * @param interface_name Interface name (must match dpdk::port_config::interface)
     * @return port_id if interface is configured, std::nullopt otherwise
     */
    COMPOSITE_API
    auto get_port_id_for_interface(const std::string& interface_name) const -> std::optional<uint16_t>;

    /**
     * @brief Get mempool by name
     *
     * @param name Mempool name (must match dpdk::port_config::mempool_name)
     * @return Pointer to mempool, or nullptr if not found
     */
    COMPOSITE_API
    auto get_mempool(const std::string& name) const -> rte_mempool*;

    /**
     * @brief Check if a port has been configured
     *
     * @param port_id DPDK port ID
     * @return true if port is configured and started
     */
    COMPOSITE_API
    auto is_port_configured(uint16_t port_id) const -> bool;

    /**
     * @brief Check if a specific queue is available for allocation
     *
     * @param interface_name Interface name
     * @param queue_id Queue ID to check
     * @return true if queue exists and is not already allocated
     */
    COMPOSITE_API
    auto is_queue_available(const std::string& interface_name, uint16_t queue_id) const -> bool;

    /**
     * @brief Allocate a specific queue on an interface
     *
     * Marks the queue as in-use so other components cannot allocate it.
     *
     * @param interface_name Interface name
     * @param queue_id Queue ID to allocate
     * @return true if allocation succeeded, false if queue unavailable
     */
    COMPOSITE_API
    auto allocate_queue(const std::string& interface_name, uint16_t queue_id) -> bool;

    /**
     * @brief Automatically allocate the next available queue on an interface
     *
     * Finds the first unallocated queue and marks it as in-use.
     *
     * @param interface_name Interface name
     * @return queue_id if a queue was available, std::nullopt if all queues allocated
     */
    COMPOSITE_API
    auto allocate_next_available_queue(const std::string& interface_name) -> std::optional<uint16_t>;

    /**
     * @brief Release a previously allocated queue
     *
     * Marks the queue as available for other components.
     * Typically called during component shutdown.
     *
     * @param interface_name Interface name
     * @param queue_id Queue ID to release
     */
    COMPOSITE_API
    auto release_queue(const std::string& interface_name, uint16_t queue_id) -> void;

    /**
     * @brief RAII variant of allocate_queue()
     *
     * Returns a queue_lease that releases the queue automatically when destroyed,
     * so a component cannot leak a queue on an early return / exception between
     * allocation and release. Prefer this over allocate_queue()/release_queue().
     *
     * @param interface_name Interface name
     * @param queue_id Queue ID to allocate
     * @return A valid lease on success; a falsy lease if DPDK is not initialized
     *         or the queue is unavailable.
     */
    COMPOSITE_API
    auto lease_queue(const std::string& interface_name, uint16_t queue_id) -> queue_lease;

    /**
     * @brief RAII variant of allocate_next_available_queue()
     *
     * @param interface_name Interface name
     * @return A valid lease owning the first free queue; a falsy lease if DPDK is
     *         not initialized or all queues are allocated.
     */
    COMPOSITE_API
    auto lease_next_available_queue(const std::string& interface_name) -> queue_lease;

    /**
     * @brief Get number of available ports
     *
     * @return Number of configured DPDK ports
     */
    COMPOSITE_API
    auto get_port_count() const -> std::size_t { return m_registry.port_count(); }

    /**
     * @brief Summary information about an available DPDK port
     */
    struct port_summary {
        uint16_t port_id;
        std::string driver_name;
        uint16_t max_rx_queues;
        uint16_t max_tx_queues;
        int socket_id;
    };

    /**
     * @brief List available DPDK ports detected by EAL
     *
     * @return Vector of port summaries
     */
    COMPOSITE_API
    auto list_available_ports() const -> std::vector<port_summary>;

    // Disable copy/move (singleton)
    manager(const manager&) = delete;
    manager(manager&&) = delete;
    auto operator=(const manager&) -> manager& = delete;
    auto operator=(manager&&) -> manager& = delete;

private:
    manager() = default;
    ~manager() = default;

    /**
     * @brief Initialize DPDK EAL
     *
     * @param eal_args Command-line style arguments for EAL
     * @return true if EAL initialization succeeded
     */
    auto init_eal(const std::vector<std::string>& eal_args) -> bool;

    /**
     * @brief Configure a single DPDK port
     *
     * Sets up RX/TX queues, creates mempool, and starts the port.
     *
     * @param config Port configuration
     * @return true if configuration succeeded
     */
    auto configure_port(const port_config& config) -> bool;

    /**
     * @brief Create a mempool for a port
     *
     * @param config Port configuration containing mempool parameters
     * @return Pointer to created mempool, or nullptr on failure
     */
    auto create_mempool(const port_config& config) -> rte_mempool*;

    struct mempool_config {
        uint32_t mempool_size;
        uint16_t mempool_cache_size;
        uint16_t mbuf_data_room_size;
    };

    // All interface/queue/mempool bookkeeping AND the lock that serializes it live
    // in the EAL-free port_registry, which is compiled and TSan-unit-tested in the
    // normal build (test/test_dpdk_registry.cpp) — the concurrent queue-allocation
    // check-then-act that had the data race is verified there, without a NIC.
    // This manager is the thin EAL adapter that delegates to it. m_initialized is
    // atomic so is_initialized() reads it without the registry lock.
    std::atomic<bool> m_initialized{false};
    port_registry m_registry;
    std::map<std::string, mempool_config> m_mempool_configs; ///< Mempool name → config (init-time only)

}; // class manager

} // namespace composite::dpdk

#endif // COMPOSITE_USE_DPDK
