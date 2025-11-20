/*
 * Copyright (C) 2025 Geon Technologies, LLC
 *
 * This file is part of composite.
 *
 * composite is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
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

#include <cstdint>
#include <string>
#include <vector>

namespace composite::dpdk {

/**
 * @brief Configuration for a single DPDK port
 */
struct port_config {
    uint16_t port_id{0};                        ///< DPDK port ID
    std::string interface{};                    ///< Interface name (e.g., "eth0", "enp1s0f0")
    uint16_t rx_queues{1};                      ///< Number of RX queues to configure
    uint16_t tx_queues{1};                      ///< Number of TX queues to configure
    uint16_t rx_descriptors{1024};              ///< Number of RX descriptors per queue
    uint16_t tx_descriptors{1024};              ///< Number of TX descriptors per queue
    std::string mempool_name{"mbuf_pool"};      ///< Name of mempool for this port
    uint32_t mempool_size{8192};                ///< Number of mbufs in the mempool
    uint16_t mempool_cache_size{256};           ///< Per-core cache size
    uint16_t mbuf_data_room_size{2048};         ///< Size of data room in each mbuf
};

/**
 * @brief Top-level DPDK configuration
 */
struct config {
    std::vector<std::string> eal_args;          ///< EAL initialization arguments
    std::vector<port_config> ports;             ///< Port configurations
};

} // namespace composite::dpdk
