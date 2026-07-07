/*
 * Copyright (C) 2024-2026 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

// EAL-free bookkeeping for the DPDK manager: the interface->port / queue-bitmap /
// mempool maps and the lock that serializes them. Deliberately has NO DPDK
// dependency (rte_mempool is only ever a forward-declared pointer), so it is
// compiled and unit-tested (incl. under ThreadSanitizer) in the normal build —
// unlike dpdk_manager.cpp, which only builds under COMPOSITE_USE_DPDK. The
// concurrency-critical check-then-act queue allocation that had the data
// race lives here and is now testable without a NIC. The manager is a thin EAL
// adapter that delegates all bookkeeping to this class.

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// rte_mempool is referenced only as an opaque pointer — forward declaration keeps
// this header free of any DPDK include.
struct rte_mempool;

namespace composite::dpdk {

class queue_lease; // RAII auto-release handle, defined after port_registry below

class port_registry {
public:
    struct port_info {
        uint16_t port_id{};
        uint16_t num_rx_queues{};
        std::vector<bool> queue_allocated; ///< per-queue allocation flags
        rte_mempool* mempool{nullptr};
    };

    /// Record a configured port. Rejects (returns false) a duplicate interface
    /// name or a duplicate port_id — the previous code silently overwrote the
    /// interface entry, losing the first port's queue bookkeeping.
    auto register_port(const std::string& interface, uint16_t port_id, uint16_t num_rx_queues, rte_mempool* mempool)
        -> bool {
        std::scoped_lock lk{m_mtx};
        if (m_interface_to_port.contains(interface)) {
            return false;
        }
        for (const auto& [iface, info] : m_interface_to_port) {
            if (info.port_id == port_id) {
                return false;
            }
        }
        m_interface_to_port.emplace(
            interface, port_info{port_id, num_rx_queues, std::vector<bool>(num_rx_queues, false), mempool});
        m_configured_ports.push_back(port_id);
        return true;
    }

    /// Record a named mempool. Rejects a duplicate name.
    auto register_mempool(const std::string& name, rte_mempool* mempool) -> bool {
        std::scoped_lock lk{m_mtx};
        return m_mempools.emplace(name, mempool).second;
    }

    [[nodiscard]] auto get_port_id_for_interface(const std::string& interface) const -> std::optional<uint16_t> {
        std::scoped_lock lk{m_mtx};
        auto it = m_interface_to_port.find(interface);
        return it == m_interface_to_port.end() ? std::nullopt : std::optional{it->second.port_id};
    }

    [[nodiscard]] auto get_mempool(const std::string& name) const -> rte_mempool* {
        std::scoped_lock lk{m_mtx};
        auto it = m_mempools.find(name);
        return it == m_mempools.end() ? nullptr : it->second;
    }

    [[nodiscard]] auto is_port_configured(uint16_t port_id) const -> bool {
        std::scoped_lock lk{m_mtx};
        for (auto id : m_configured_ports) {
            if (id == port_id) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] auto is_queue_available(const std::string& interface, uint16_t queue_id) const -> bool {
        std::scoped_lock lk{m_mtx};
        auto it = m_interface_to_port.find(interface);
        if (it == m_interface_to_port.end() || queue_id >= it->second.num_rx_queues) {
            return false;
        }
        return !it->second.queue_allocated[queue_id];
    }

    /// Allocate a specific queue. Returns false if the interface/queue is invalid
    /// or already allocated. Check-then-set is atomic under the lock.
    auto allocate_queue(const std::string& interface, uint16_t queue_id) -> bool {
        std::scoped_lock lk{m_mtx};
        auto it = m_interface_to_port.find(interface);
        if (it == m_interface_to_port.end() || queue_id >= it->second.num_rx_queues) {
            return false;
        }
        if (it->second.queue_allocated[queue_id]) {
            return false;
        }
        it->second.queue_allocated[queue_id] = true;
        return true;
    }

    /// Allocate the first free queue on an interface. Atomic scan-and-set.
    auto allocate_next_available_queue(const std::string& interface) -> std::optional<uint16_t> {
        std::scoped_lock lk{m_mtx};
        auto it = m_interface_to_port.find(interface);
        if (it == m_interface_to_port.end()) {
            return std::nullopt;
        }
        auto& info = it->second;
        for (uint16_t q = 0; q < info.num_rx_queues; ++q) {
            if (!info.queue_allocated[q]) {
                info.queue_allocated[q] = true;
                return q;
            }
        }
        return std::nullopt;
    }

    /// Allocate a specific queue and return an RAII handle that releases it on
    /// destruction. The returned lease is falsy (operator bool == false) if the
    /// queue could not be allocated. Prefer this over allocate_queue() so a queue
    /// is never leaked on an early return / exception between allocate and release.
    [[nodiscard]] auto lease_queue(const std::string& interface, uint16_t queue_id) -> queue_lease;

    /// Allocate the first free queue on an interface as an RAII lease (falsy if
    /// none free / interface unknown).
    [[nodiscard]] auto lease_next_available_queue(const std::string& interface) -> queue_lease;

    /// Release a previously-allocated queue. Returns true if it was allocated.
    auto release_queue(const std::string& interface, uint16_t queue_id) -> bool {
        std::scoped_lock lk{m_mtx};
        auto it = m_interface_to_port.find(interface);
        if (it == m_interface_to_port.end() || queue_id >= it->second.num_rx_queues) {
            return false;
        }
        if (!it->second.queue_allocated[queue_id]) {
            return false;
        }
        it->second.queue_allocated[queue_id] = false;
        return true;
    }

    [[nodiscard]] auto port_count() const -> std::size_t {
        std::scoped_lock lk{m_mtx};
        return m_interface_to_port.size();
    }

    /// Snapshot of configured port ids (for the manager's EAL stop/close loop).
    [[nodiscard]] auto configured_port_ids() const -> std::vector<uint16_t> {
        std::scoped_lock lk{m_mtx};
        return m_configured_ports;
    }

    /// Drop all bookkeeping (shutdown / failed-init cleanup). Does NOT touch DPDK.
    auto clear() -> void {
        std::scoped_lock lk{m_mtx};
        m_interface_to_port.clear();
        m_mempools.clear();
        m_configured_ports.clear();
    }

private:
    mutable std::mutex m_mtx;
    std::map<std::string, port_info> m_interface_to_port;
    std::map<std::string, rte_mempool*> m_mempools;
    std::vector<uint16_t> m_configured_ports;
};

/// Move-only RAII handle for an allocated RX queue. Releases the queue back to its
/// port_registry on destruction (unless moved-from or explicitly reset()). This is
/// the leak-proof way for a component to hold a queue for its lifetime: the queue
/// is freed deterministically when the component (and thus its lease) is destroyed,
/// even on an error path that never reaches an explicit release_queue() call.
class queue_lease {
public:
    queue_lease() = default; ///< empty / falsy lease (allocation failed)

    queue_lease(port_registry* registry, std::string interface, uint16_t queue_id)
        : m_registry(registry), m_interface(std::move(interface)), m_queue_id(queue_id), m_held(true) {}

    queue_lease(const queue_lease&) = delete;
    auto operator=(const queue_lease&) -> queue_lease& = delete;

    queue_lease(queue_lease&& other) noexcept { *this = std::move(other); }

    auto operator=(queue_lease&& other) noexcept -> queue_lease& {
        if (this != &other) {
            reset(); // release anything we currently hold before taking over
            m_registry = other.m_registry;
            m_interface = std::move(other.m_interface);
            m_queue_id = other.m_queue_id;
            m_held = other.m_held;
            other.m_held = false;
            other.m_registry = nullptr;
        }
        return *this;
    }

    ~queue_lease() { reset(); }

    /// Release the held queue now (idempotent; a no-op if empty/moved-from).
    auto reset() -> void {
        if (m_held && m_registry != nullptr) {
            m_registry->release_queue(m_interface, m_queue_id);
        }
        m_held = false;
        m_registry = nullptr;
    }

    [[nodiscard]] auto valid() const -> bool { return m_held; }
    explicit operator bool() const { return m_held; }
    [[nodiscard]] auto queue_id() const -> uint16_t { return m_queue_id; }
    [[nodiscard]] auto interface() const -> const std::string& { return m_interface; }

private:
    port_registry* m_registry{nullptr};
    std::string m_interface;
    uint16_t m_queue_id{0};
    bool m_held{false};
};

inline auto port_registry::lease_queue(const std::string& interface, uint16_t queue_id) -> queue_lease {
    return allocate_queue(interface, queue_id) ? queue_lease{this, interface, queue_id} : queue_lease{};
}

inline auto port_registry::lease_next_available_queue(const std::string& interface) -> queue_lease {
    auto q = allocate_next_available_queue(interface);
    return q ? queue_lease{this, interface, *q} : queue_lease{};
}

} // namespace composite::dpdk
