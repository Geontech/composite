/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "composite/core/metadata.hpp"
#include "composite/core/timestamp.hpp"
#include "input_port.hpp"
#include "port_base.hpp"

#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <typeinfo>
#include <vector>

namespace composite {

/**
 * @brief Forward declaration of output_port template
 *
 * output_port is specialized for immutable_buffer<T> and mutable_buffer<T>.
 */
template <typename BufferType>
class output_port;

/**
 * @brief Output port specialization for immutable buffers
 * @tparam T Element type (e.g., float, int, custom struct)
 *
 * Sends immutable_buffer<T> data to connected input ports. Features:
 * - Zero-copy sharing to immutable input ports (via shared_ptr)
 * - Deep copy when sending to mutable input ports
 * - Fan-out support (one output → many inputs)
 * - Statistics tracking (throughput, packets sent)
 * - Metadata carried atomically with each packet (send_data's optional 3rd arg)
 *
 * **Transfer Optimization:**
 * - immutable → immutable: Zero-copy share (fast)
 * - immutable → mutable: Deep copy (required for exclusive ownership)
 *
 * **Thread Safety:** Wiring/introspection methods (connect, disconnect, is_connected,
 * connection_count, can_send, stats) are thread-safe — the connection list is mutex-protected.
 * **send_data()/send_batch() are SINGLE-PRODUCER**: they must be called from one thread only.
 * The send path reads a lock-free cached connection snapshot (refreshed via a generation counter
 * on topology change), which is mutable per-producer state with no internal locking; concurrent
 * senders on the same output are a data race. (One component worker owns the send path; the pool
 * threads in pipeline_component never send.)
 *
 * **Typical Usage:**
 * @code
 * output_port<immutable_buffer<float>> output{"samples_out"};
 *
 * // In component process():
 * auto buffer = make_immutable<float>(1024);
 * // ... fill buffer ...
 * output.send_data(std::move(buffer), timestamp::now());
 * @endcode
 */
template <typename T>
class output_port<immutable_buffer<T>> : public output_port_base {
public:
    using buffer_type = immutable_buffer<T>; ///< Buffer type for this port
    using value_type = T;                    ///< Element type stored in buffers

    /**
     * @brief Inherit constructor from output_port_base
     */
    using output_port_base::output_port_base;

    /**
     * @brief Default destructor
     */
    ~output_port() override = default;

    /**
     * @brief Get type index for element type T
     * @return std::type_index for type T
     */
    auto element_type() const -> std::type_index override { return std::type_index(typeid(T)); }

    /**
     * @brief Get type identifier for element type T
     * @return Hash code from typeid(T)
     */
    auto element_type_id() const -> std::size_t override { return typeid(T).hash_code(); }

    /**
     * @brief Check if this port uses mutable buffers
     * @return false (immutable_buffer port)
     */
    auto is_mutable() const -> bool override { return false; }

    /**
     * @brief Send data (and optional metadata) to all connected input ports
     * @param buffer Immutable buffer to send
     * @param ts Timestamp associated with the data
     * @param md Shared metadata describing this packet (nullptr = none) — travels
     *           atomically WITH the data to every connected port (no separate latch,
     *           no race). Producers should build the instance once per metadata
     *           CHANGE (see composite::make_metadata) and pass the same pointer for
     *           every packet in between; attachment is a refcount bump, and fan-out
     *           receivers share the one instance.
     *
     * Delivers buffer to all connected input ports with optimal transfer strategy:
     * - For immutable inputs: Zero-copy share via buffer.share()
     * - For mutable inputs: Deep copy to new mutable_buffer
     *
     * Transfer optimization (mirrors the mutable path): the sole / last receiver gets the
     * buffer + metadata pointer by MOVE (not even a refcount bump); earlier fan-out
     * receivers bump the metadata refcount and share the buffer.
     *
     * Updates statistics (packets, bytes, throughput) and checks queue capacity.
     * If an input port is full, the packet is dropped at that port (not here).
     */
    auto send_data(buffer_type buffer, timestamp ts, composite::metadata_ptr md = nullptr) -> void {
        // Update outgoing statistics
        m_stats.record_transfer(buffer.size() * sizeof(T));

        // Lock-free snapshot of the fan-out list (mutated only via connect/disconnect).
        const auto& ports = producer_snapshot(); // single-producer send path: cached, lock-free in steady state

        if (ports->empty()) {
            return;
        }

        if (ports->size() == 1) {
            auto* port = ports->front();
            if (port == nullptr) {
                return;
            }
            if (port->is_mutable()) {
                // immutable → mutable: deep copy (mutable needs independent storage)
                auto* mutable_port = static_cast<input_port<mutable_buffer<T>>*>(port);
                auto vec = std::make_unique<std::vector<T>>(buffer.begin(), buffer.end());
                mutable_port->add_data(mutable_buffer<T>{std::move(vec)}, ts, std::move(md));
            } else {
                // immutable → immutable, sole receiver: move the buffer + metadata (no copy)
                auto* immutable_port = static_cast<input_port<immutable_buffer<T>>*>(port);
                immutable_port->add_data(std::move(buffer), ts, std::move(md));
            }
            return;
        }

        // Fan-out: all receivers share the one metadata instance (refcount bumps); the
        // last gets the pointer by move.
        for (std::size_t i = 0; i + 1 < ports->size(); ++i) {
            auto* port = (*ports)[i];
            if (port == nullptr) {
                continue;
            }
            if (port->is_mutable()) {
                auto* mutable_port = static_cast<input_port<mutable_buffer<T>>*>(port);
                auto vec = std::make_unique<std::vector<T>>(buffer.begin(), buffer.end());
                mutable_port->add_data(mutable_buffer<T>{std::move(vec)}, ts, md);
            } else {
                auto* immutable_port = static_cast<input_port<immutable_buffer<T>>*>(port);
                immutable_port->add_data(buffer.share(), ts, md);
            }
        }
        // Last receiver: move the buffer (immutable) + metadata.
        auto* last_port = ports->back();
        if (last_port != nullptr) {
            if (last_port->is_mutable()) {
                auto* mutable_port = static_cast<input_port<mutable_buffer<T>>*>(last_port);
                auto vec = std::make_unique<std::vector<T>>(buffer.begin(), buffer.end());
                mutable_port->add_data(mutable_buffer<T>{std::move(vec)}, ts, std::move(md));
            } else {
                auto* immutable_port = static_cast<input_port<immutable_buffer<T>>*>(last_port);
                immutable_port->add_data(std::move(buffer), ts, std::move(md));
            }
        }
    }

    /**
     * @brief Convenience overload: accepts a plain metadata value and wraps it in a
     *        shared instance for this one send. Prefer the metadata_ptr overload on
     *        hot paths — this one allocates per call.
     */
    auto send_data(buffer_type buffer, timestamp ts, std::optional<composite::metadata> md) -> void {
        send_data(std::move(buffer), ts,
                  md.has_value() ? composite::make_metadata(std::move(*md)) : composite::metadata_ptr{});
    }

    /**
     * @brief Send a batch of buffers (sharing one timestamp/metadata) to connected
     *        inputs. With a single immutable consumer this is one amortized
     *        add_batch (single ring publish); otherwise it falls back to per-buffer
     *        send_data (fan-out / mutable targets). Buffers are consumed.
     */
    auto send_batch(std::span<buffer_type> bufs, timestamp ts, composite::metadata_ptr md = nullptr) -> void {
        const auto& ports = producer_snapshot(); // single-producer send path: cached, lock-free in steady state
        if (ports->size() == 1 && ports->front() != nullptr && !ports->front()->is_mutable()) {
            auto* ip = static_cast<input_port<immutable_buffer<T>>*>(ports->front());
            std::size_t bytes = 0;
            for (const auto& b : bufs) {
                bytes += b.size() * sizeof(T);
            }
            m_stats.record_transfer(bytes, bufs.size());
            ip->add_batch(bufs, ts, std::move(md));
            return;
        }
        for (auto& b : bufs) {
            send_data(std::move(b), ts, md);
        }
    }

    /// Convenience overload: wraps a plain metadata value (allocates per call).
    auto send_batch(std::span<buffer_type> bufs, timestamp ts, std::optional<composite::metadata> md) -> void {
        send_batch(bufs, ts, md.has_value() ? composite::make_metadata(std::move(*md)) : composite::metadata_ptr{});
    }

}; // class output_port<immutable_buffer<T>>

/**
 * @brief Output port specialization for mutable buffers
 * @tparam T Element type (e.g., float, int, custom struct)
 *
 * Sends mutable_buffer<T> data to connected input ports. Features:
 * - Move semantics for single connections (zero-copy, optimal)
 * - Smart fan-out: copies to all but last receiver, move to last
 * - Automatic promotion to immutable when sending to immutable inputs
 * - Fan-out support (one output → many inputs)
 * - Statistics tracking (throughput, packets sent)
 *
 * **Transfer Optimization (Single Output):**
 * - mutable → mutable: Move (optimal, zero-copy)
 * - mutable → immutable: Promote to immutable via to_immutable()
 *
 * **Transfer Optimization (Fan-Out):**
 * - First N-1 receivers: Deep copy (required for independent ownership)
 * - Last receiver: Move original buffer (saves one copy)
 *
 * **Thread Safety:** Wiring/introspection methods (connect, disconnect, is_connected,
 * connection_count, can_send, stats) are thread-safe — the connection list is mutex-protected.
 * **send_data()/send_batch() are SINGLE-PRODUCER**: they must be called from one thread only.
 * The send path reads a lock-free cached connection snapshot (refreshed via a generation counter
 * on topology change), which is mutable per-producer state with no internal locking; concurrent
 * senders on the same output are a data race. (One component worker owns the send path; the pool
 * threads in pipeline_component never send.)
 *
 * **Typical Usage:**
 * @code
 * output_port<mutable_buffer<float>> output{"samples_out"};
 *
 * // In component process():
 * auto buffer = make_mutable<float>(1024);
 * // ... fill buffer ...
 * output.send_data(std::move(buffer), timestamp::now());
 * @endcode
 */
template <typename T>
class output_port<mutable_buffer<T>> : public output_port_base {
public:
    using buffer_type = mutable_buffer<T>; ///< Buffer type for this port
    using value_type = T;                  ///< Element type stored in buffers

    /**
     * @brief Inherit constructor from output_port_base
     */
    using output_port_base::output_port_base;

    /**
     * @brief Default destructor
     */
    ~output_port() override = default;

    /**
     * @brief Get type index for element type T
     * @return std::type_index for type T
     */
    auto element_type() const -> std::type_index override { return std::type_index(typeid(T)); }

    /**
     * @brief Get type identifier for element type T
     * @return Hash code from typeid(T)
     */
    auto element_type_id() const -> std::size_t override { return typeid(T).hash_code(); }

    /**
     * @brief Check if this port uses mutable buffers
     * @return true (mutable_buffer port)
     */
    auto is_mutable() const -> bool override { return true; }

    /**
     * @brief Send mutable buffer to connected input ports
     * @param buffer Mutable buffer to send (consumed via move)
     * @param ts Timestamp associated with the data
     *
     * Implements optimized fan-out strategy:
     *
     * **Single Connection:**
     * - mutable → mutable: Move buffer directly (zero-copy)
     * - mutable → immutable: Promote to immutable via to_immutable()
     *
     * **Multiple Connections (Fan-Out):**
     * - First N-1 connections: Deep copy via buffer.copy()
     * - Last connection: Move original buffer (saves one copy)
     * - Automatic type conversion (mutable ↔ immutable) as needed
     *
     * This strategy minimizes copies while maintaining correctness -
     * each receiver gets independent ownership of the data.
     */
    auto send_data(buffer_type buffer, timestamp ts, composite::metadata_ptr md = nullptr) -> void {
        // Update statistics
        m_stats.record_transfer(buffer.size() * sizeof(T));

        // Lock-free snapshot of the fan-out list (mutated only via connect/disconnect).
        const auto& ports = producer_snapshot(); // single-producer send path: cached, lock-free in steady state

        if (ports->empty()) {
            return;
        };

        if (ports->size() == 1) {
            auto* port = ports->front();
            if (port == nullptr) {
                return;
            };

            if (port->is_mutable()) {
                // Mutable to mutable: direct move
                auto* mutable_port = static_cast<input_port<mutable_buffer<T>>*>(port);
                mutable_port->add_data(std::move(buffer), ts, std::move(md));
            } else {
                // Mutable to immutable: promote
                auto* immutable_port = static_cast<input_port<immutable_buffer<T>>*>(port);
                immutable_port->add_data(std::move(buffer).to_immutable(), ts, std::move(md));
            }
        } else {
            // Fan-out: handle multiple outputs (all share the one metadata instance)
            for (std::size_t i = 0; i < ports->size() - 1; ++i) {
                auto* port = (*ports)[i];
                if (port == nullptr) {
                    continue;
                };

                if (port->is_mutable()) {
                    auto* mutable_port = static_cast<input_port<mutable_buffer<T>>*>(port);
                    mutable_port->add_data(buffer.copy(), ts, md);
                } else {
                    auto* immutable_port = static_cast<input_port<immutable_buffer<T>>*>(port);
                    immutable_port->add_data(buffer.copy().to_immutable(), ts, md);
                }
            }

            // Last output: move the buffer (and the metadata)
            auto* last_port = ports->back();
            if (last_port != nullptr) {
                if (last_port->is_mutable()) {
                    auto* mutable_port = static_cast<input_port<mutable_buffer<T>>*>(last_port);
                    mutable_port->add_data(std::move(buffer), ts, std::move(md));
                } else {
                    auto* immutable_port = static_cast<input_port<immutable_buffer<T>>*>(last_port);
                    immutable_port->add_data(std::move(buffer).to_immutable(), ts, std::move(md));
                }
            }
        }
    }

    /**
     * @brief Convenience overload: accepts a plain metadata value and wraps it in a
     *        shared instance for this one send. Prefer the metadata_ptr overload on
     *        hot paths — this one allocates per call.
     */
    auto send_data(buffer_type buffer, timestamp ts, std::optional<composite::metadata> md) -> void {
        send_data(std::move(buffer), ts,
                  md.has_value() ? composite::make_metadata(std::move(*md)) : composite::metadata_ptr{});
    }

    /**
     * @brief Send a batch of mutable buffers (sharing one timestamp/metadata) with
     *        one amortized add_batch to a single consumer (moved for mutable,
     *        promoted for immutable); per-buffer send_data fallback for fan-out.
     *        Buffers are consumed.
     */
    auto send_batch(std::span<buffer_type> bufs, timestamp ts, composite::metadata_ptr md = nullptr) -> void {
        const auto& ports = producer_snapshot(); // single-producer send path: cached, lock-free in steady state
        if (ports->size() == 1 && ports->front() != nullptr) {
            auto* port = ports->front();
            std::size_t bytes = 0;
            for (const auto& b : bufs) {
                bytes += b.size() * sizeof(T);
            }
            m_stats.record_transfer(bytes, bufs.size());
            if (port->is_mutable()) {
                auto* ip = static_cast<input_port<mutable_buffer<T>>*>(port);
                ip->add_batch(bufs, ts, std::move(md));
            } else {
                auto* ip = static_cast<input_port<immutable_buffer<T>>*>(port);
                m_immutable_batch_scratch.clear();
                m_immutable_batch_scratch.reserve(bufs.size());
                for (std::size_t i = 0; i < bufs.size(); ++i) {
                    auto packet_md = (i + 1 == bufs.size()) ? std::move(md) : md;
                    m_immutable_batch_scratch.emplace_back(std::move(bufs[i]).to_immutable(), ts, std::move(packet_md));
                }
                ip->add_batch(
                    std::span<typename input_port<immutable_buffer<T>>::queue_type>(m_immutable_batch_scratch));
                m_immutable_batch_scratch.clear(); // release any rejected suffix immediately
            }
            return;
        }
        for (auto& b : bufs) {
            send_data(std::move(b), ts, md);
        }
    }

    /// Convenience overload: wraps a plain metadata value (allocates per call).
    auto send_batch(std::span<buffer_type> bufs, timestamp ts, std::optional<composite::metadata> md) -> void {
        send_batch(bufs, ts, md.has_value() ? composite::make_metadata(std::move(*md)) : composite::metadata_ptr{});
    }

private:
    // Mutable->immutable conversion needs packet descriptors of the target
    // type. Retain the scratch allocation across calls; the common exact-type
    // paths above write directly into the destination ring.
    std::vector<typename input_port<immutable_buffer<T>>::queue_type> m_immutable_batch_scratch;

}; // output_port<mutable_buffer<T>>

} // namespace composite
