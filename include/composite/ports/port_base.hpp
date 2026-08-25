/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "composite/core/metadata.hpp"
#include "composite/core/park.hpp" // park_coordinator::signal_data — wake a consumer on EOS/close
#include "port_stats.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

namespace composite {

class park_coordinator; // doorbell: an input port wakes its owning component's worker
class output_port_base; // an input holds a back-pointer to its producer for deregister-on-destroy

/**
 * @brief Abstract base class for all port types
 *
 * Provides common interface for both input and output ports, including:
 * - Port naming
 * - Type identification via type_id hash
 * - Mutability query (immutable vs mutable buffers)
 *
 * This base class enables polymorphic handling of ports in the component system.
 */
class port_base {
public:
    /**
     * @brief Construct a port with a name
     * @param name Port name (used for identification and connection)
     */
    explicit port_base(std::string_view name) : m_name(name) {}

    /**
     * @brief Virtual destructor for polymorphic deletion
     */
    virtual ~port_base() = default;

    /**
     * @brief Get the port name
     * @return Port name as string_view
     */
    auto name() const -> std::string_view { return m_name; }

    /**
     * @brief Get type index for the element type (T in buffer<T>)
     * @return std::type_index for compile-time type information
     *
     * Used for transformation lookup and diagnostic output.
     */
    virtual auto element_type() const -> std::type_index = 0;

    /**
     * @brief Get type identifier for the element type (T in buffer<T>)
     * @return Hash code from typeid(T).hash_code()
     *
     * Used for type safety - ensures connections are between compatible types.
     */
    virtual auto element_type_id() const -> std::size_t = 0;

    /**
     * @brief Check if this port uses mutable buffers
     * @return true for mutable_buffer ports, false for immutable_buffer ports
     *
     * Used to determine optimal transfer strategy (move vs share).
     */
    virtual auto is_mutable() const -> bool = 0;

    /**
     * @brief Register port metrics with the metrics registry
     * @param component_id ID of the owning component
     *
     * Called by component::add_port() to expose port statistics as metrics.
     * Subclasses implement this to register their specific stats.
     */
    virtual auto register_port_metrics(std::string_view component_id) -> void = 0;

protected:
    std::string m_name; ///< Port name for identification
};

/**
 * @brief Base class for input ports with queue management and statistics
 *
 * Provides common functionality for all input port specializations:
 * - Thread-safe queue with configurable depth
 * - Backpressure via is_full() and available_capacity()
 * - Statistics tracking (packets, bytes, throughput)
 * - Metadata latching for stream properties
 * - Overflow handling with callbacks
 *
 * Templated specializations (input_port<immutable_buffer<T>> and
 * input_port<mutable_buffer<T>>) extend this with type-specific functionality.
 */
class input_port_base : public port_base {
public:
    /**
     * @brief Callback invoked when packets are dropped due to queue overflow
     * @param dropped_count Number of packets dropped in this overflow event
     *
     * Useful for logging, alerting, or implementing backoff strategies.
     */
    using overflow_callback = std::function<void(std::size_t dropped_count)>;

    /**
     * @brief Inherit port_base constructor
     */
    using port_base::port_base;

    /// Deregisters from the producer's fan-out (see definition after output_port_base) so an
    /// output that outlives a destroyed input never holds a dangling pointer to it.
    ~input_port_base() noexcept override;

    /**
     * @brief Single-producer claim. An input may be fed by at most one output
     * port — the precondition the lock-free SPSC ring rests on. connect() claims
     * the input; a second producer (fan-in) is rejected.
     * @param producer The claiming output port (recorded as a back-pointer so the input can
     *        deregister itself on destruction).
     * @return true if the claim succeeded (was unclaimed), false if already claimed.
     */
    [[nodiscard]] auto claim_producer(output_port_base* producer) -> bool {
        // Serialized against a physical ring resize (input_port::depth()). Without this the
        // resize is a check-then-act: it could read "unclaimed", this CAS could then claim
        // and begin sending, and the resize would replace the storage under the new
        // producer. Both sides are cold (connect / setup-time sizing), so the lock costs
        // nothing on the data path. Held only across the claim itself — the caller takes
        // its own m_mutate_mtx AFTER this returns, so the two never nest here.
        const auto lock = std::scoped_lock{m_resize_mtx};
        bool expected = false;
        if (!m_has_producer.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return false; // fan-in: already claimed
        }
        m_producer.store(producer, std::memory_order_release); // back-pointer for deregister-on-destroy
        return true;
    }
    /// Release the producer claim (on disconnect), permitting a later reconnect. Clears the
    /// back-pointer FIRST so once m_has_producer reads false (a reconnect may win), no stale
    /// producer pointer remains.
    auto release_producer() -> void {
        m_producer_doorbell.store(nullptr, std::memory_order_release); // no producer worker to wake anymore
        m_producer_closed.store(false, std::memory_order_release);     // a reconnected input is not born at-end
        m_producer.store(nullptr, std::memory_order_release);
        m_has_producer.store(false, std::memory_order_release);
    }

    /**
     * @brief Wire this input to its owning component's worker doorbell.
     * @param doorbell The owning component's park_coordinator (or nullptr to detach).
     *
     * Once set, a producer's add_data() can wake the (sleeping) consumer worker on the
     * empty->non-empty edge — turning the worker's NOOP backoff into a data-driven wake.
     * Set by component::add_port(); a free-standing port leaves this null (producers then
     * never signal, which is harmless — the port still queues normally).
     */
    auto set_doorbell(park_coordinator* doorbell) -> void { m_doorbell = doorbell; }

    /**
     * @brief Wire this input to its PRODUCER's worker for backpressure wake (the reverse
     * doorbell). @param doorbell The producing component's park_coordinator (or nullptr).
     *
     * Once set, this input's pop() wakes the (sleeping, AWAIT_OUTPUT) producer worker on the
     * full->not-full edge — so a producer that paced via can_send() resumes the instant a slot
     * frees instead of waiting out its NOOP backoff. Set by component::connect(), cleared on
     * disconnect / producer destruction; atomic because it is set/cleared concurrently with the
     * consumer's pop. A free-standing producer leaves it null (no worker to wake).
     */
    auto set_producer_doorbell(park_coordinator* doorbell) -> void {
        m_producer_doorbell.store(doorbell, std::memory_order_release);
    }

    /// End-of-stream signalling (out-of-band, so the drop-on-full ring can never lose it).
    /// mark_producer_closed() is called by the producer's output_port_base::send_eos() (fan-out)
    /// after its last add_data(); it flags the input closed (release) and wakes a sleeping consumer
    /// so the close is observed promptly. Because at_end() also requires the ring to be drained
    /// (pending()==0), a consumer never reports end-of-stream before consuming every enqueued packet.
    auto mark_producer_closed() -> void {
        m_producer_closed.store(true, std::memory_order_release);
        if (m_doorbell != nullptr) {
            m_doorbell->signal_data();
        } // wake a NOOPing consumer
    }
    /// Clear the end-of-stream latch: the producer is (re)starting and will send data again. Called
    /// by output_port_base::reopen() from the producer component's start path, so a stale EOS from a
    /// prior completed run doesn't make this input report at_end() forever after a restart.
    auto reopen_producer() -> void { m_producer_closed.store(false, std::memory_order_release); }
    /// Whether the producer has signalled end-of-stream (no more data will be enqueued).
    [[nodiscard]] auto producer_closed() const -> bool { return m_producer_closed.load(std::memory_order_acquire); }
    /// End-of-stream reached: producer closed AND the ring is fully drained (occupancy gates it, so
    /// all data-before-EOS has been consumed). A consumer typically returns FINISH once this is true.
    [[nodiscard]] auto at_end() const -> bool { return producer_closed() && pending() == 0; }

    /**
     * @brief Number of packets currently queued (consumer-side observation).
     * @return Current ring occupancy.
     *
     * Used by the worker's doorbell re-check (component::any_input_has_data) to catch data
     * that arrived between the worker's last NOOP and its arming of the doorbell.
     */
    [[nodiscard]] virtual auto pending() const -> std::size_t = 0;

    /**
     * @brief Get the queue depth (soft limit).
     * @return Maximum number of packets the ring will hold. 0 disables the port
     *         (all packets dropped — used to pause an input).
     */
    auto depth() const -> std::size_t { return m_depth.load(std::memory_order_relaxed); }

    /**
     * @brief Set the queue depth (soft limit). Virtual so the ring-backed
     *        input_port also (re)sizes its ring (at setup) when this changes.
     * @param value Maximum queue capacity (0 = disabled port).
     */
    virtual auto depth(std::size_t value) -> void {
        m_depth.store(value, std::memory_order_relaxed);
        if (m_queue_capacity_gauge) {
            m_queue_capacity_gauge->set(static_cast<double>(value));
        }
    }

    /**
     * @brief Get port statistics
     * @return Reference to port statistics structure
     */
    auto stats() const -> const port_stats& { return m_stats; }

    /**
     * @brief Reset statistics counters
     */
    auto reset_stats() -> void { m_stats.reset(); }

    /**
     * @brief Check if port queue is full
     * @return true if queue is at capacity
     */
    virtual auto is_full() const -> bool = 0;

    /**
     * @brief Get available capacity in queue
     * @return Number of packets that can be queued before reaching depth limit
     */
    virtual auto available_capacity() const -> std::size_t = 0;

    /**
     * @brief Set overflow callback for dropped packets
     * @param callback Function to call when packets are dropped. A batch overflow invokes it once
     *                 with the aggregate rejected-packet count.
     */
    auto set_overflow_callback(overflow_callback callback) -> void {
        const auto lock = std::scoped_lock{m_mtx};
        m_overflow_callback = std::move(callback);
    }

    /**
     * @brief Number of times the overflow callback threw and was contained.
     *
     * The callback is user code invoked on the producer's send path, so an exception from
     * it is contained rather than unwound into the producer's worker (see
     * input_port::record_drop). A non-zero count means the drop notification is unreliable
     * — the drop itself is still counted in packets_dropped().
     */
    [[nodiscard]] auto overflow_callback_errors() const -> std::uint64_t {
        return m_overflow_callback_errors.load(std::memory_order_relaxed);
    }

    /**
     * @brief Register port metrics with the metrics registry
     * @param component_id ID of the owning component
     */
    auto register_port_metrics(std::string_view component_id) -> void override {
        m_stats.register_metrics(component_id, m_name, "input");

        // Register input-port-specific gauges
        auto& registry = metrics::registry::instance();
        metrics::labels_t labels = {
            {"component_id", std::string{component_id}}, {"port_name", std::string{m_name}}, {"port_type", "input"}};

        m_queue_depth_gauge = &registry.get_or_create_gauge("composite.port.queue_depth",
                                                            "Current number of packets in the queue", "1", labels);

        m_queue_capacity_gauge =
            &registry.get_or_create_gauge("composite.port.queue_capacity", "Configured queue depth limit", "1", labels);

        // Set initial capacity value
        m_queue_capacity_gauge->set(static_cast<double>(m_depth.load(std::memory_order_relaxed)));
    }

protected:
    /**
     * @brief Update the queue depth gauge (called by derived classes)
     * @param current_size Current number of items in the queue
     */
    auto update_queue_depth_metric(std::size_t current_size) -> void {
        if (m_queue_depth_gauge) {
            m_queue_depth_gauge->set(static_cast<double>(current_size));
        }
    }

    // The producer output owns the deregister arbitration (it touches m_producer under its
    // own m_mutate_mtx in its destructor); give it access.
    friend class output_port_base;

    mutable std::mutex m_mtx; ///< Guards the overflow-callback set/read (not the ring)
    /// Serializes the producer CLAIM against a physical ring resize (input_port::depth()),
    /// which must not replace storage a producer is about to write. Only claim_producer()
    /// (which SETS the claim) needs it: a concurrent release can at worst make a resize read
    /// "still claimed" and conservatively decline to grow. Cold path on both sides.
    mutable std::mutex m_resize_mtx;        ///< mutable: the const introspection readers take it too
    std::atomic_bool m_has_producer{false}; ///< single-producer claim (set on connect)
    std::atomic<output_port_base*> m_producer{
        nullptr}; ///< back-pointer to the producer (for deregister-on-destroy); cold, never read on the send path
    std::atomic<std::size_t> m_depth{1024}; ///< queue depth soft limit (0 = disabled)
    mutable port_stats m_stats;             ///< Statistics tracking
    overflow_callback m_overflow_callback;  ///< Callback for dropped packets
    std::atomic<std::uint64_t> m_overflow_callback_errors{
        0}; ///< contained throws from m_overflow_callback (see overflow_callback_errors())
    metrics::gauge<double>* m_queue_depth_gauge{nullptr};    ///< Current queue depth gauge
    metrics::gauge<double>* m_queue_capacity_gauge{nullptr}; ///< Queue capacity gauge
    park_coordinator* m_doorbell{nullptr}; ///< consumer worker to wake on empty->non-empty edge (read doorbell); set
                                           ///< once in add_port before start (no concurrent write)
    std::atomic<park_coordinator*> m_producer_doorbell{
        nullptr}; ///< producer worker to wake on full->not-full edge (reverse doorbell); set/cleared on
                  ///< connect/disconnect concurrent with pop -> atomic
    std::atomic_bool m_producer_closed{
        false}; ///< EOS: producer signalled end-of-stream (set by send_eos, cleared on release_producer); out-of-band
                ///< so the drop-on-full ring can't lose it

}; // class input_port_base

/**
 * @brief Base class for output ports with connection management
 *
 * Provides common functionality for all output port specializations:
 * - Connection management (connect, disconnect, query)
 * - Fan-out support (one output → many inputs)
 * - Metadata broadcasting to all connected inputs
 * - Statistics tracking
 * - Backpressure checking via can_send()
 *
 * Templated specializations (output_port<immutable_buffer<T>> and
 * output_port<mutable_buffer<T>>) extend this with type-specific data transfer.
 */
class output_port_base : public port_base {
public:
    /**
     * @brief Inherit port_base constructor
     */
    using port_base::port_base;

    /**
     * @brief Destructor: clears the back-pointer in every still-connected input so that an
     * input outliving this output never dereferences this (freed) output in its own
     * destructor. Pairs with ~input_port_base, which removes itself from this fan-out when
     * the input dies first. The atomic CAS on each input's m_producer arbitrates which side
     * owns the edge teardown (see input_port_base / the dtor comments).
     *
     * NOTE: safe for SEQUENTIAL teardown in either destruction order (the supported model —
     * application teardown destroys components on one thread, and stop() has joined every
     * worker so no send is in flight). Destroying the two endpoints CONCURRENTLY on different
     * threads, or destroying a still-RUNNING peer mid-send, remains the caller's contract.
     */
    ~output_port_base() override {
        const auto lock = std::scoped_lock{m_mutate_mtx};
        const auto cur = m_connected.load(std::memory_order_relaxed);
        for (auto* port : *cur) {
            if (port == nullptr) {
                continue;
            }
            // Claim the edge from the output end. If we win (the input has not already
            // exchanged it away in its own dtor), clear its claim so a survivor can reconnect.
            output_port_base* expected = this;
            if (port->m_producer.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel)) {
                port->m_has_producer.store(false, std::memory_order_release);
                // Clear the EOS latch too, exactly as release_producer() does on an ordinary
                // disconnect. Without this, a source that sent EOS and was then destroyed leaves
                // the surviving input latched closed: reconnected to a NEW producer, at_end()
                // goes true the moment the ring drains and the consumer auto-FINISHes before the
                // new producer has sent anything. The two teardown paths must agree.
                port->m_producer_closed.store(false, std::memory_order_release);
            }
            // Clear the reverse-doorbell pointer too: our owning component's park (which it
            // pointed at) dies with us, so the surviving input must not signal it from pop().
            port->m_producer_doorbell.store(nullptr, std::memory_order_release);
        }
        // No need to rebuild m_connected — this output is being destroyed; the vector dies with it.
    }

    /**
     * @brief Called by ~input_port_base when an input is destroyed: remove it from this
     * output's fan-out so the (surviving) output never holds a dangling pointer to it.
     * Arbitrated by an exchange on the input's back-pointer under m_mutate_mtx — exactly one
     * of {this method, ~output_port_base} performs the removal for a given edge.
     */
    auto detach_consumer_on_destroy(input_port_base* in) -> void {
        const auto lock = std::scoped_lock{m_mutate_mtx};
        output_port_base* expected = this;
        if (!in->m_producer.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel)) {
            return; // the output's own dtor (or a disconnect) already cleared this edge
        }
        auto next = std::make_shared<connection_list>(*m_connected.load(std::memory_order_relaxed));
        std::erase(*next, in);
        m_connected.store(std::shared_ptr<const connection_list>(std::move(next)), std::memory_order_release);
        m_generation.fetch_add(1, std::memory_order_release); // invalidate the producer's cached snapshot
    }

    /**
     * @brief Connect this output to an input port.
     * @param port Pointer to the input port to connect to.
     * @return true on success; false if @p port is null or it already has a
     *         producer (fan-in is rejected — an input may have at most one
     *         producer, which keeps its lock-free SPSC ring sound by construction).
     *
     * Fan-out (this output → many distinct inputs) is supported; each of those
     * inputs is claimed by this single output, so each still has one producer.
     *
     * **Note:** Type safety is enforced in the templated component::connect().
     */
    /// The fan-out list. Read lock-free by the send path and by introspection via
    /// an atomic snapshot; mutated copy-on-write under m_mutate_mtx (connect/
    /// disconnect are rare). The worker thus iterates the connection list with no
    /// lock on the hot path.
    using connection_list = std::vector<input_port_base*>;

    auto connect(input_port_base* port) -> bool {
        if (port == nullptr) {
            return false;
        }
        // Element-type compatibility is checked HERE (before claiming the input),
        // not only in the templated component::connect(): this base method is
        // public and reached directly by tests and by port_set::get_port<...base>
        // callers. send_data() static_casts the input to input_port<...<T>> based
        // solely on is_mutable(); a mismatched T is undefined behavior. Reject the
        // connect instead of corrupting memory on the first send. Compared via
        // type_index (exact), not element_type_id(): hash_code() is permitted to
        // collide, and a collision here IS that undefined behavior.
        if (this->element_type() != port->element_type()) {
            return false;
        }
        if (!port->claim_producer(this)) {
            return false; // fan-in (input already fed by another output)
        }
        const auto lock = std::scoped_lock{m_mutate_mtx};
        auto next = std::make_shared<connection_list>(*m_connected.load(std::memory_order_relaxed));
        next->push_back(port);
        m_connected.store(std::shared_ptr<const connection_list>(std::move(next)), std::memory_order_release);
        m_generation.fetch_add(1, std::memory_order_release); // invalidate the producer's cached snapshot
        return true;
    }

    /**
     * @brief Disconnect from a specific input port (releases its producer claim).
     * @return true if port was connected and is now disconnected, false otherwise
     */
    auto disconnect(input_port_base* port) -> bool {
        const auto lock = std::scoped_lock{m_mutate_mtx};
        auto next = std::make_shared<connection_list>(*m_connected.load(std::memory_order_relaxed));
        auto it = std::find(next->begin(), next->end(), port);
        if (it == next->end()) {
            return false;
        }
        next->erase(it);
        m_connected.store(std::shared_ptr<const connection_list>(std::move(next)), std::memory_order_release);
        m_generation.fetch_add(1, std::memory_order_release); // invalidate the producer's cached snapshot
        if (port != nullptr) {
            port->release_producer();
        }
        return true;
    }

    /**
     * @brief Disconnect from all connected input ports (releasing their claims).
     * @return Number of ports that were disconnected
     */
    auto disconnect() -> std::size_t {
        const auto lock = std::scoped_lock{m_mutate_mtx};
        auto cur = m_connected.load(std::memory_order_relaxed);
        const auto count = cur->size();
        for (auto* port : *cur) {
            if (port != nullptr) {
                port->release_producer();
            }
        }
        m_connected.store(std::make_shared<const connection_list>(), std::memory_order_release);
        m_generation.fetch_add(1, std::memory_order_release); // invalidate the producer's cached snapshot
        return count;
    }

    /// Signal end-of-stream to every connected input: no more data will be sent from this output.
    /// Each consumer reaches at_end() once it drains its ring. Called from the producer worker
    /// (single producer) after the last send_data(); reads an atomic snapshot so it is safe against
    /// a concurrent disconnect. Idempotent. Out-of-band (a flag, not a ring packet), so the
    /// drop-on-full ring can never lose the signal.
    auto send_eos() -> void {
        auto cur = snapshot();
        for (auto* port : *cur) {
            if (port != nullptr) {
                port->mark_producer_closed();
            }
        }
    }

    /// Re-open every connected input (clear its end-of-stream latch): the producer is (re)starting
    /// and will send data again. Mirrors send_eos(); called from the producer component's start path
    /// so a stale EOS from a prior completed run doesn't leave downstream stuck reporting at_end().
    auto reopen() -> void {
        auto cur = snapshot();
        for (auto* port : *cur) {
            if (port != nullptr) {
                port->reopen_producer();
            }
        }
    }

    /// @return true if at least one input port is connected.
    auto is_connected() const -> bool { return !snapshot()->empty(); }

    /// @return true if connected to the specified input port.
    auto is_connected_to(const input_port_base* port) const -> bool {
        auto cur = snapshot();
        return std::find(cur->begin(), cur->end(), port) != cur->end();
    }

    /// @return number of active connections.
    auto connection_count() const -> std::size_t { return snapshot()->size(); }

    /// @return connected input port names (for debugging/introspection).
    auto connected_ports() const -> std::vector<std::string> {
        auto cur = snapshot();
        std::vector<std::string> names;
        names.reserve(cur->size());
        for (const auto* port : *cur) {
            if (port != nullptr) {
                names.emplace_back(port->name());
            }
        }
        return names;
    }

    /**
     * @brief Get port statistics
     * @return Reference to port statistics structure
     */
    auto stats() const -> const port_stats& { return m_stats; }

    /**
     * @brief Reset statistics counters
     */
    auto reset_stats() -> void { m_stats.reset(); }

    /// @return true if at least one connected input port can ACCEPT a send without the producer
    /// having to block (i.e. the send would enqueue OR would drop-on-a-paused-port, but not stall).
    /// A paused input (depth()==0, set by disable/pause_input_ports) discards on send by design, so it
    /// does NOT constitute backpressure — treating it as "full" would wrongly stall a can_send-pacing
    /// producer against a disabled consumer forever. A genuinely full port (depth>0, ring at capacity)
    /// IS backpressure. Introspection path (REST threads): spinlock-backed atomic<shared_ptr> snapshot.
    auto can_send() const -> bool {
        auto cur = snapshot();
        for (const auto* port : *cur) {
            if (port != nullptr && (port->depth() == 0 || !port->is_full())) {
                return true;
            }
        }
        return false;
    }

    /// SEND-path can_send(): same result as can_send() (incl. the paused-port rule above), but
    /// lock-free in steady state via producer_snapshot() (only re-loads the atomic<shared_ptr> when
    /// the topology generation changed). Caller MUST be the single producer thread — same invariant as
    /// producer_snapshot()/send_data. Use this, not can_send(), on a component's hot worker loop so
    /// backpressure pacing doesn't reintroduce the per-iteration connection-snapshot tax.
    [[nodiscard]] auto producer_can_send() const -> bool {
        const auto& cur = producer_snapshot();
        for (const auto* port : *cur) {
            if (port != nullptr && (port->depth() == 0 || !port->is_full())) {
                return true;
            }
        }
        return false;
    }

    /// SEND-path "is anything connected?" — lock-free, single-producer (see producer_can_send).
    [[nodiscard]] auto producer_is_connected() const -> bool { return !producer_snapshot()->empty(); }

    /**
     * @brief Register port metrics with the metrics registry
     * @param component_id ID of the owning component
     */
    auto register_port_metrics(std::string_view component_id) -> void override {
        m_stats.register_metrics(component_id, m_name, "output");
    }

protected:
    /// Thread-safe snapshot of the current fan-out list — for INTROSPECTION (REST threads:
    /// is_connected/connection_count/can_send/...). This is an std::atomic<shared_ptr> load,
    /// which is NOT lock-free in libstdc++ (it takes an internal spinlock), so it is kept off
    /// the steady-state send path (see producer_snapshot).
    auto snapshot() const -> std::shared_ptr<const connection_list> {
        return m_connected.load(std::memory_order_acquire);
    }

    /// SEND-path snapshot — caller MUST be the single producer thread (the component's
    /// worker; the same single-producer invariant the downstream SPSC rings already require).
    /// Steady state reads only m_generation (a lock-free atomic<uint64_t>) and returns the
    /// cached shared_ptr by reference (no refcount bump, no atomic<shared_ptr> spinlock); the
    /// non-lock-free m_connected load happens only when connect/disconnect bumped the
    /// generation (rare). Not safe to call concurrently from multiple threads (mutable cache).
    auto producer_snapshot() const -> const std::shared_ptr<const connection_list>& {
        const auto gen = m_generation.load(std::memory_order_acquire);
        if (gen != m_producer_cached_gen) {
            m_producer_cached = m_connected.load(std::memory_order_acquire); // cold path: topology changed
            m_producer_cached_gen = gen;
        }
        return m_producer_cached;
    }

    std::atomic<std::shared_ptr<const connection_list>> m_connected{
        std::make_shared<const connection_list>()}; ///< COW fan-out list (lock-free read)
    std::atomic<std::uint64_t> m_generation{0};     ///< bumped on connect/disconnect; gates the producer cache
    std::mutex m_mutate_mtx;                        ///< serializes connect/disconnect (rare)
    mutable port_stats m_stats;                     ///< Statistics tracking (bytes, packets, throughput)
    // Producer-side cache (touched only by the single producer thread via producer_snapshot).
    mutable std::shared_ptr<const connection_list> m_producer_cached;
    mutable std::uint64_t m_producer_cached_gen{
        static_cast<std::uint64_t>(-1)}; // != initial generation -> first call loads

}; // class output_port_base

// Defined here (after output_port_base is complete) because it calls into the producer.
// Deregister from the producer's fan-out so an output that outlives this input never holds a
// dangling pointer to it — the reviewer-flagged forever-dangling UAF, now closed for the
// supported (sequential, stop-then-destroy) teardown in BOTH destruction orders:
//  - input destroyed first: the producer is alive; detach_consumer_on_destroy() removes us.
//  - output destroyed first: ~output_port_base already CAS-cleared our m_producer, so we load
//    null and never touch the freed output.
// Truly-concurrent cross-thread destruction of the two endpoints, and destroying a RUNNING
// peer mid-send, remain the caller's contract (the framework's model is application-owned
// stop-then-destroy on a single teardown thread; managed disconnect already parks the
// producer).
inline input_port_base::~input_port_base() noexcept {
    if (output_port_base* p = m_producer.load(std::memory_order_acquire)) {
        p->detach_consumer_on_destroy(this);
    }
}

} // namespace composite
