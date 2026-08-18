/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "composite/metrics/metrics.hpp"
#include "composite/ports/input_port.hpp"
#include "composite/ports/output_port.hpp"
#include "composite/ports/port_set.hpp"
#include "composite/properties/property_set.hpp"
#include "lifecycle.hpp"
#include "park.hpp"

#include "composite/core/logger.hpp"
#include <atomic>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>

namespace composite {

enum class retval : int {
    NORMAL,
    NOOP,
    FINISH,
    NO_YIELD,
    // reverse doorbell: process() returns this INSTEAD of NOOP when it could not proceed
    // specifically because a downstream output is full (it paced via can_send()). It declares
    // the wait reason the framework cannot otherwise infer, so the worker idles on the doorbell
    // and re-checks can_send() (correct + non-spinning: process() returns NORMAL the moment a
    // slot frees) — and is woken the instant a consumer drains, instead of waiting out m_delay.
    AWAIT_OUTPUT
}; // enum class retval

/// Why a component's worker loop terminated on its own (as opposed to an external stop()).
enum class finish_reason {
    none,      ///< not finished — still running, or stopped externally (never self-terminated)
    completed, ///< process() returned FINISH — orderly, successful completion (e.g. source hit EOF)
    error      ///< process() threw — the worker aborted on an exception
}; // enum class finish_reason

/// Human-readable name for a finish_reason (used in status/introspection).
inline auto to_string(finish_reason r) -> std::string_view {
    switch (r) {
    case finish_reason::completed:
        return "completed";
    case finish_reason::error:
        return "error";
    case finish_reason::none:
        break;
    }
    return "none";
}

class component : public lifecycle {
    static constexpr uint32_t DEFAULT_DELAY{1000000};
    // sched_yield() on every NORMAL is a per-packet syscall (~hundreds of ns even when
    // nothing else is runnable). Yield once per this many consecutive NORMALs instead — a
    // throughput win on saturated streams while still ceding the core periodically for
    // fairness on an oversubscribed core. Tunable via the "yield_interval" property; 1
    // restores yield-every-packet, a large value approximates NO_YIELD. The park_point()
    // at loop-top — not the yield — is what keeps a property write responsive, so reducing
    // the yield cadence does not delay reconfiguration.
    static constexpr uint32_t DEFAULT_YIELD_INTERVAL{32};

public:
    struct connection {
        std::pair<std::string, std::string> output;
        std::pair<std::string, std::string> input;
    };

    /**
     * @brief RAII member that stops the worker before derived members are destroyed
     *
     * For a HEAP-allocated component, prefer composite::make_component<T>() — its
     * deleter stops the component before ~T even begins, which protects the whole
     * destruction sequence rather than just the members declared after this one.
     * auto_stop remains the tool for stack-allocated components (and belt-and-braces
     * in components that may be constructed either way).
     *
     * Declare as the **LAST data member** of a concrete component:
     * @code
     * class my_comp : public composite::component {
     *     // ... ports, properties, derived state ...
     *     composite::component::auto_stop m_auto_stop{*this};  // MUST be last
     * };
     * @endcode
     * Its destructor runs first (members destruct in reverse declaration order),
     * joining the worker while derived state is still alive — closing the
     * destruction-order use-after-free where ~component would otherwise join the
     * worker after the derived members it touches in process() are gone.
     */
    class auto_stop {
    public:
        explicit auto_stop(component& owner) noexcept : m_owner(&owner) {}
        auto_stop(const auto_stop&) = delete;
        auto_stop& operator=(const auto_stop&) = delete;
        ~auto_stop() { m_owner->stop_contained(); } // idempotent, and must not throw from a dtor
    private:
        component* m_owner;
    };

    virtual ~component() override {
        stop_contained(); // a throwing stop (park timeout / staged reaction) must not unwind here
        // Deregister all of this component's metrics (lifecycle + ports + user) by
        // its identity label, so a reload doesn't leak series toward the registry
        // cap and OTel drops the instruments. The registry outlives components.
        metrics::registry::instance().remove_by_label("component_id", m_id);
        if (m_logger) {
            m_logger->flush();
        }
    }

    auto id() const noexcept -> const std::string& { return m_id; }

    auto initialize() -> void override {
        // To be implemented by subclasses
    }

    /// start()/stop() are final: they ARE the park/worker machinery, and one lifecycle path
    /// (the enabled-reconcile used by application::start() and RUNTIME `enabled` writes) calls
    /// the private *_locked helpers directly — so an override would run on the direct call but
    /// silently NOT on a reconcile, leaving subclass resources half-managed. Subclasses hook
    /// on_worker_start()/on_worker_stop() instead, which run on EVERY start/stop path.
    auto start() -> void final {
        std::scoped_lock life{m_lifecycle_mtx};
        start_locked();
    }

    auto stop() -> void final {
        bool had_worker = false;
        {
            std::scoped_lock life{m_lifecycle_mtx};
            had_worker = m_thread.has_value();
            stop_locked();
        }
        // Drain a reaction staged just before the stop AFTER releasing m_lifecycle_mtx
        // (so an on_apply that touches the lifecycle cannot self-deadlock). Only when we
        // actually stopped a worker — guards against running a stale reaction during
        // ~component (auto_stop already stopped + drained while the derived was alive).
        // CONTAINED: stop() runs from destructors and from application teardown loops, so
        // a wedged-worker park timeout here must not unwind out of it.
        if (had_worker) {
            run_reactions_contained();
        }
    }

    /**
     * @brief Ask the worker to exit, without waiting for it to do so. Idempotent.
     *
     * **Does not wait for worker exit — but is not guaranteed to be instantaneous.** It takes
     * the lifecycle lock (untimed) and invokes the on_park_requested() wake hook, which is user
     * code. The default hook does not block, but a component whose hook is slow makes this slow
     * too, and that cost lands inside a caller's shutdown budget. It is "does not join the
     * worker", not "returns immediately".
     *
     * Latches the stop request and nudges the worker awake. The component has NOT stopped when
     * this returns — pair it with try_stop() to collect the result.
     *
     * Deliberately PUBLIC, not an implementation detail of application: signal-then-collect is
     * what makes a shared-deadline shutdown possible, and an embedder orchestrating its own
     * shutdown ordering (rather than using composite::application) needs the same primitive.
     * Calling this WITHOUT a following try_stop()/stop() leaves the component stop-requested
     * but un-torn-down — always pair it.
     */
    auto request_stop() -> void {
        std::scoped_lock life{m_lifecycle_mtx};
        if (m_thread.has_value()) {
            request_worker_exit_locked();
        }
    }

    /**
     * @brief Bounded stop, for callers that must not hang.
     * @return true if the component is stopped (torn down exactly as stop() leaves it);
     *         false if it is NOT stopped — which covers three distinct outcomes: the lifecycle
     *         lock was unavailable within the budget, the worker did not exit, or the worker
     *         exited but a property write was still in flight. The caller's obligation is the
     *         same in all three (do not destroy it; retry), which is why this is a bool rather
     *         than a status enum — but do not report a false as "the worker is still running",
     *         because it may not be.
     *
     * **What the budget covers:** acquiring the lifecycle lock, waiting for the worker to
     * exit, and waiting out any property write still in flight. All three are deadline-driven
     * against one shared @p timeout.
     *
     * **What it CANNOT cover:** the synchronous user hooks it must call — on_park_requested()
     * on the way in, and (once the worker has exited) on_worker_stop() during teardown. There
     * is no way to bound a synchronous callback without abandoning it mid-execution, which is
     * exactly the use-after-free this API exists to avoid. Both are contained and reported, and
     * the default on_park_requested() does not block; a component that blocks in either is
     * violating the process()/hook promptness contract and will be named in the log.
     *
     * **A false return means: do not destroy this component.** ~component joins the worker and
     * that join cannot be abandoned, so dropping the last reference to a component that did not
     * stop trades a bounded wait for an unbounded one in a destructor. Keep it alive and retry,
     * or report it and leave it running.
     *
     * Retrying is safe and complete from every bail-out point: the stop request stays latched,
     * park state still accurately reflects a live-or-exiting worker, and nothing has been torn
     * down. A later try_stop()/stop() joins instantly and finishes the teardown; the
     * resource-reap is exchange-guarded, so it cannot double-run.
     */
    template <typename Rep, typename Period>
    [[nodiscard]] auto try_stop(std::chrono::duration<Rep, Period> timeout) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        const auto left = [&deadline] {
            const auto r = deadline - std::chrono::steady_clock::now();
            return r > decltype(r)::zero() ? r : decltype(r)::zero();
        };
        bool had_worker = false;
        {
            // Bound the lock too: a concurrent start/stop holding m_lifecycle_mtx would
            // otherwise consume the whole budget before this attempt even starts.
            //
            // Polled try_lock() rather than a timed_mutex: timed_mutex::try_lock_until against
            // steady_clock needs libstdc++'s _M_clocklock, which is not available in every
            // configuration we build (it breaks the TSan build outright). This is a cold path,
            // so polling costs nothing and keeps m_lifecycle_mtx a plain std::mutex — no change
            // to component's layout.
            std::unique_lock<std::mutex> life{m_lifecycle_mtx, std::defer_lock};
            while (!life.try_lock()) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            had_worker = m_thread.has_value();
            if (!had_worker) {
                return true; // already stopped — idempotent, same as stop()
            }
            // Same admission gate as stop(): the bounded drain below is only meaningful if no
            // new writer can slip in behind it and race the teardown.
            const park_coordinator::admission_gate gate{m_park};
            request_worker_exit_locked(); // NOTE: runs the user wake hook; see the caveat above
            if (!wait_until_finished(left())) {
                return false; // worker did not exit in budget; nothing torn down
            }
            // Drain BEFORE the teardown so this wait is inside the budget too. Bailing here is
            // just as safe and retryable as bailing above: the worker has exited but nothing is
            // torn down, so a later try_stop()/stop() joins instantly and finishes the job.
            if (!m_park.drain_in_flight_for(left())) {
                return false;
            }
            finish_stop_locked(); // its own drain is now a no-op
        }
        // Outside m_lifecycle_mtx so a lifecycle-touching on_apply cannot self-deadlock.
        run_reactions_contained();
        return true;
    }

    /// stop() that cannot unwind, for destructors and best-effort teardown. stop() is not
    /// itself noexcept: stop_locked() and the staged-reaction drain can both fail (a worker
    /// that will not park inside the timeout). Letting that escape ~component or
    /// ~auto_stop is std::terminate, and letting it escape a teardown loop abandons every
    /// component after it — so those callers use this and continue.
    auto stop_contained() noexcept -> void {
        try {
            stop();
        } catch (const std::exception& ex) {
            log_contained_failure("stop() failed during teardown", ex.what());
        } catch (...) {
            log_contained_failure("stop() failed during teardown", "unknown exception");
        }
    }

    /**
     * @brief One unit of work. Implemented by every component.
     *
     * **Must return promptly.** stop() joins this worker, and that join cannot be abandoned
     * (the caller may be ~component), so a process() that does not return wedges shutdown --
     * and, through application::stop(), the whole graph's shutdown with it. The framework's
     * own idle path is safe: returning NOOP sleeps on a bounded, stop-token-aware wait.
     *
     * If you must wait on something the framework does not own (a blocking socket, a device,
     * an external queue), then either use a bounded wait and return NOOP between attempts, or
     * override on_park_requested() to interrupt it -- that hook is called on the stop path
     * precisely so a custom wait can be broken out of. A stop that has to report progress is
     * a bug in the component, and stop() will name it in the log.
     *
     * An exception thrown here is caught, logged, recorded in finish_error(), and ends the
     * worker with finish_reason::error; it never escapes the worker thread.
     */
    virtual auto process() -> retval = 0;

    /// Called ONCE on the worker thread when the worker loop terminates on its OWN — i.e. process()
    /// returned FINISH (reason=completed) or threw (reason=error) — just before the worker exits.
    /// NOT called for an external stop(). Override to release RESOURCES or record why the component
    /// finished. A throw here is caught and logged (it cannot prevent the worker exiting). To emit
    /// final buffered DATA at end-of-stream, override on_end_of_stream() instead — it runs earlier
    /// (as an ordinary worker iteration) and can safely send_data().
    ///
    /// Keep this PROMPT. It runs during the completion tail while the park coordinator is still in
    /// its RUNNING state, so a slow on_finished() (or a slow send_eos()) blocks any concurrent
    /// property write's with_worker_parked() for its whole duration — long enough and that write hits
    /// its park timeout and is REJECTED with an API-visible error, even though the component is
    /// finishing normally. If completion needs an unbounded flush (e.g. a network drain), do it before
    /// returning FINISH, not here.
    virtual auto on_finished(finish_reason /*reason*/) -> void {}

    /// Called ONCE on the worker thread when the base detects end-of-stream — process() returned
    /// NOOP while inputs_at_end() is true (every input drained + producer-closed) and finish_at_end
    /// is set — just BEFORE the synthesized FINISH and the send_eos() that closes the outputs.
    /// Override to emit any final held/buffered data via the normal send_data() path (a delay line's
    /// last frame, a framer's partial residue, a partial accumulator). Unlike on_finished(), this
    /// runs as an ordinary worker iteration (the park is in its normal RUNNING state, park_point()
    /// already ran at loop-top), so a BOUNDED flush is safe here. Emission is best-effort on a full
    /// output (drop-on-full); a component that must not drop its tail should pace via can_send().
    /// A throw is caught and logged; the component still finishes. Default: no-op.
    virtual auto on_end_of_stream() -> void {}

    /// Block until the worker has exited (whether it self-finished or was stopped). Returns
    /// immediately if no worker is running. For a component that runs indefinitely this blocks
    /// until it is stopped; for a source/batch component it returns when process() returns FINISH.
    auto wait_until_finished() -> void {
        std::unique_lock lk{m_finished_mtx};
        m_finished_cv.wait(lk, [this] { return m_worker_done; });
    }

    /// Bounded wait_until_finished(); returns true if the worker exited within @p timeout.
    template <typename Rep, typename Period>
    auto wait_until_finished(std::chrono::duration<Rep, Period> timeout) -> bool {
        std::unique_lock lk{m_finished_mtx};
        return m_finished_cv.wait_for(lk, timeout, [this] { return m_worker_done; });
    }

    /// Signal end-of-stream on every output port: no more data will be produced. Called
    /// automatically when the worker completes (finish_reason::completed); a source may also call it
    /// explicitly at EOF before returning FINISH. Downstream consumers reach input at_end() once
    /// drained. Out-of-band (a flag, not a ring packet), so the drop-on-full ring cannot lose it.
    auto send_eos() -> void {
        for (const auto& [name, port] : m_port_set.ports()) {
            if (auto* out = dynamic_cast<output_port_base*>(port)) {
                out->send_eos();
            }
        }
    }

    /// True iff this component has at least one input AND every input is at end-of-stream (its
    /// producer closed and its ring drained). Returns false for a source (no inputs). The
    /// base does the common case FOR you: a process() returning NOOP while inputs_at_end() is true
    /// is auto-promoted to FINISH (see the worker loop / the finish_at_end property), which fires
    /// send_eos() and completes the graph — override on_end_of_stream() to emit held data first.
    /// Call this directly only for custom completion logic beyond that default.
    [[nodiscard]] auto inputs_at_end() const -> bool {
        bool any_input = false;
        for (const auto& [name, port] : m_port_set.ports()) {
            if (const auto* in = dynamic_cast<const input_port_base*>(port)) {
                any_input = true;
                if (!in->at_end()) {
                    return false;
                }
            }
        }
        return any_input;
    }

    auto add_port(port_base& port) -> void {
        m_port_set.add_port(port);

        // Register port metrics with component context
        port.register_port_metrics(m_id);

        // doorbell: an input fed by an upstream producer wakes our worker on the
        // empty->non-empty edge, so it does not wait out its NOOP backoff.
        if (auto* in = dynamic_cast<input_port_base*>(&port)) {
            in->set_doorbell(&m_park);
        }
    }

    auto add_port(port_base* port) -> void {
        if (port == nullptr) {
            return;
        }
        m_port_set.add_port(port);

        // Register port metrics with component context
        port->register_port_metrics(m_id);

        // doorbell (see the reference overload).
        if (auto* in = dynamic_cast<input_port_base*>(port)) {
            in->set_doorbell(&m_park);
        }
    }

    template <typename T>
    auto get_port(std::string_view name) -> T* {
        return m_port_set.get_port<T>(name);
    }

    auto ports() const -> const port_set::port_map_type& { return m_port_set.ports(); }

    /**
     * @brief Connect this component's output port to another's input port
     *
     * @param output_port_name Name of output port on this component
     * @param other Target component
     * @param input_port_name Name of input port on target component
     * @return true if connection successful, false otherwise
     */
    auto connect(std::string_view output_port_name, std::shared_ptr<component> other, std::string_view input_port_name)
        -> bool {
        // Get output port from this component
        auto* out_port = get_port<output_port_base>(output_port_name);
        if (out_port == nullptr) {
            m_logger->error("output port '{}' not found", output_port_name);
            return false;
        }

        // Get input port from target component
        if (other == nullptr) {
            m_logger->error("invalid input component pointer");
            return false;
        }
        auto* in_port = other->get_port<input_port_base>(input_port_name);
        if (in_port == nullptr) {
            m_logger->error("input port '{}' not found", input_port_name);
            return false;
        }

        // Check element type compatibility
        if (out_port->element_type_id() != in_port->element_type_id()) {
            m_logger->error("type mismatch connecting {}:{} ({}) to {}:{} ({})", id(), output_port_name,
                            out_port->element_type().name(), other->id(), input_port_name,
                            in_port->element_type().name());
            return false;
        }

        // Surface the transfer cost at connect time. An immutable output feeding a mutable input
        // is legal, but EVERY frame is deep-copied to give the consumer exclusive writable
        // storage — a silent per-frame cost that otherwise only shows up in a profiler. Warn
        // loudly so the topology author either makes the input immutable (zero-copy share) or
        // knowingly accepts the copy.
        if (!out_port->is_mutable() && in_port->is_mutable()) {
            m_logger->warn("connecting {}:{} (immutable) -> {}:{} (mutable): every frame will be DEEP-COPIED to give "
                           "the consumer writable storage; declare the input as immutable_buffer to share zero-copy, "
                           "or keep it mutable to accept the per-frame copy",
                           id(), output_port_name, other->id(), input_port_name);
        } else {
            m_logger->trace("connecting {}:{} (mutability: {}) -> {}:{} (mutability: {})", id(), output_port_name,
                            out_port->is_mutable() ? "mutable" : "immutable", other->id(), input_port_name,
                            in_port->is_mutable() ? "mutable" : "immutable");
        }

        // Make the connection. Rejects fan-in: an input may have at most one
        // producer (keeps its lock-free SPSC ring sound by construction).
        if (!out_port->connect(in_port)) {
            m_logger->error("cannot connect {}:{} -> {}:{}: input already has a producer (fan-in is unsupported)", id(),
                            output_port_name, other->id(), input_port_name);
            return false;
        }

        // reverse doorbell: wire the input back to THIS (producer) component's worker, so the
        // consumer's pop wakes us on the full->not-full edge when process() returned AWAIT_OUTPUT.
        in_port->set_producer_doorbell(&m_park);

        // Record connection for tracking. Guarded: REST POST /app/connections can
        // run connect() on an httplib pool thread concurrently with GET /app
        // reading connections() on another — an unsynchronized vector push_back vs
        // iteration is UB (reallocation invalidates the reader).
        {
            std::scoped_lock lk{m_connections_mtx};
            m_connections.push_back({.output = std::make_pair(id(), std::string{output_port_name}),
                                     .input = std::make_pair(other->id(), std::string{input_port_name})});
        }
        m_logger->debug("connected {}:{} -> {}:{}", id(), output_port_name, other->id(), input_port_name);

        return true;
    }

    /**
     * @brief Disconnect a specific connection (and update bookkeeping).
     *
     * Parks this component's worker around the port-level disconnect so no
     * send_data() is in flight when the input's producer claim is released —
     * otherwise a sender holding the pre-disconnect fan-out snapshot could write
     * the input's SPSC ring concurrently with a new producer that re-claims it.
     * Also removes the matching record from m_connections.
     * @return true if the connection existed and was removed.
     */
    auto disconnect(std::string_view output_port_name, const std::shared_ptr<component>& other,
                    std::string_view input_port_name) -> bool {
        auto* out_port = get_port<output_port_base>(output_port_name);
        if (out_port == nullptr || other == nullptr) {
            return false;
        }
        auto* in_port = other->get_port<input_port_base>(input_port_name);
        if (in_port == nullptr) {
            return false;
        }

        bool ok = false;
        m_park.with_worker_parked([&] { ok = out_port->disconnect(in_port); });
        if (ok) {
            std::scoped_lock lk{m_connections_mtx};
            std::erase_if(m_connections, [&](const connection& c) {
                return c.output.first == m_id && c.output.second == output_port_name && c.input.first == other->id() &&
                       c.input.second == input_port_name;
            });
        }
        return ok;
    }

    /// Disconnect ALL consumers of an output port (parked; updates bookkeeping).
    /// @return number of connections removed.
    auto disconnect_all(std::string_view output_port_name) -> std::size_t {
        auto* out_port = get_port<output_port_base>(output_port_name);
        if (out_port == nullptr) {
            return 0;
        }
        std::size_t count = 0;
        m_park.with_worker_parked([&] { count = out_port->disconnect(); });
        if (count > 0) {
            std::scoped_lock lk{m_connections_mtx};
            std::erase_if(m_connections, [&](const connection& c) {
                return c.output.first == m_id && c.output.second == output_port_name;
            });
        }
        return count;
    }

    /// Snapshot (by value) of this component's recorded connections — taken under
    /// the lock, so it is safe to call concurrently with connect()/disconnect().
    auto connections() const -> std::vector<connection> {
        std::scoped_lock lk{m_connections_mtx};
        return m_connections;
    }

    // ========================================================================
    // Property Registration API
    // ========================================================================

    /**
     * @brief Register a scalar, optional, or scalar-list property
     * Excludes struct types and struct list types which use separate overloads.
     */
    /// Register a scalar/enum/optional/vector/reflected-struct property bound to
    /// a member. Returns the typed_property for fluent .validate()/.on_change()/.units().
    template <typename T>
    auto add_property(std::string_view name, T& ref,
                      properties::config_type config = properties::config_type::INITIALIZE)
        -> properties::typed_property<T>& {
        return m_prop_set.add(name, ref, config);
    }

    /// Register a keyed collection (std::map<std::string, E>, E reflected via COMPOSITE_STRUCT).
    template <typename E>
    auto add_keyed(std::string_view name, std::map<std::string, E>& ref,
                   properties::config_type config = properties::config_type::INITIALIZE)
        -> properties::keyed_collection<E>& {
        return m_prop_set.add_keyed(name, ref, config);
    }

    /// Register a config<T>: the struct's COMPOSITE_FIELDS become top-level properties
    /// (the wire contract is unchanged), but the whole struct is the validate/commit
    /// unit and reactions arrive via cfg.on_apply(prev, changes<T>). Fields opt into
    /// runtime configurability with the `runtime` attribute; @p config is the baseline.
    template <reflect::reflected T>
    auto add_config(composite::config<T>& cfg, properties::config_type config = properties::config_type::INITIALIZE)
        -> composite::config<T>& {
        return m_prop_set.add_config(cfg, config);
    }

    // ========================================================================
    // Property Access
    // ========================================================================

    /// Typed read of a property's current value. Takes the property read-lock
    /// (excludes concurrent writers/swaps), so it is safe from REST/introspection
    /// threads — previously this read the value with no lock. Reentrant-safe from
    /// inside a property handler: the parked writer short-circuits the lock.
    template <typename T>
    auto get_property(std::string_view name) const -> T {
        // `enabled` is a framework spec/status virtual, not a value property; report the
        // desired state (keeps get_property<bool>("enabled") working for application start
        // and existing call sites).
        if constexpr (std::is_same_v<T, bool>) {
            if (name == "enabled") {
                return is_enabled();
            }
        }
        return m_park.with_reader_lock([&] { return m_prop_set.template get<T>(name); });
    }

    /// No-lock typed read for use INSIDE a with_property_read_lock() block, which
    /// already holds the read lock. Calling the locking get_property() while holding
    /// that lock would recursively acquire the underlying std::shared_mutex — which
    /// is not recursive (undefined behavior). Use this to read several properties
    /// atomically under one with_property_read_lock().
    template <typename T>
    auto get_property_locked(std::string_view name) const -> T {
        if constexpr (std::is_same_v<T, bool>) {
            if (name == "enabled") {
                return is_enabled();
            }
        }
        return m_prop_set.template get<T>(name);
    }

    /// Full property state as JSON. Takes the property read-lock itself, so call it
    /// standalone from REST/introspection threads — NOT inside a
    /// with_property_read_lock() block (that would recursively acquire the shared
    /// lock; use property_set().encode() there instead).
    [[nodiscard]] auto property_state() const -> properties::json {
        auto state = m_park.with_reader_lock([this] { return m_prop_set.encode(); });
        // `enabled` is a spec/status virtual (not in the value set): report the DESIRED
        // state plus the OBSERVED `running` state. Both are computed live, so they can
        // never desync from a stale bound mirror (the old m_enabled_prop bug).
        state["enabled"] = is_enabled();
        state["running"] = is_running();
        // Completion status: whether the worker self-terminated, and why (completed vs error).
        const auto fr = m_finish_reason.load(std::memory_order_acquire);
        state["finished"] = (fr != finish_reason::none);
        if (fr != finish_reason::none) {
            state["finish_reason"] = to_string(fr);
        }
        // Detail for a failure, so a control plane does not have to scrape the log to learn
        // WHY a component stopped. Emitted only when there is one (see finish_error()).
        if (fr == finish_reason::error) {
            if (auto err = finish_error(); !err.empty()) {
                state["finish_error"] = std::move(err);
            }
        }
        return state;
    }

    /**
     * @brief The component's properties as ONE JSON Schema 2020-12 document.
     *
     * Published by `GET /app/components/:id/schema`. Property names are the keys of
     * `properties`; composite-specific metadata (units, configurability, power-of-two)
     * rides as `x-composite-*` vendor extensions, so the document is a conformant schema
     * a generic validator or form generator can consume directly.
     *
     * Also advertises the `enabled` spec/status virtual — it is not a registered value
     * property (set_properties handles it directly), but it IS a legal key, so a UI needs
     * it to render a lifecycle toggle.
     */
    [[nodiscard]] auto property_schema() const -> properties::json {
        auto schema = m_prop_set.schema();
        schema["title"] = m_id;
        schema["properties"]["enabled"] = properties::json{
            {"type", "boolean"},
            {"default", true},
            {"description", "desired lifecycle state — writing it starts/stops the component"},
            {"x-composite-configurability", "runtime"},
        };
        return schema;
    }

    [[nodiscard]] auto property_set() const -> const properties::property_set& { return m_prop_set; }

    /**
     * @brief Does this component accept @p name in a set_properties() batch?
     * @return true for a registered plain property, a registered config<T> FIELD, or the
     *         framework `enabled` virtual.
     *
     * Exists for the application-level ("globals") property block, which is broadcast to
     * every component: a key that applies to some components must be filtered per
     * component rather than forcing the whole batch to tolerate unknown keys. That lets
     * the loader run with allow_unknown=false, so a TYPO in a component's own property
     * block is an error instead of being silently discarded.
     */
    [[nodiscard]] auto has_property(std::string_view name) const -> bool {
        // `enabled` is a spec/status virtual handled directly by set_properties, so it is
        // deliberately absent from the property set — but it IS a legal key.
        return name == "enabled" || m_prop_set.contains(name);
    }

    // ========================================================================
    // Property Setting
    // ========================================================================

    /**
     * @brief Apply a JSON object of property updates as an atomic batch.
     *
     * Parks the worker, validates-all-then-commits-all (a rejection mutates
     * nothing), then runs property_change_handler() — all while the worker is
     * quiesced, so process() reads the new values lock-free and consistently.
     */
    auto set_properties(const properties::json& values,
                        properties::config_type config = properties::config_type::INITIALIZE,
                        bool allow_unknown = false) -> void {
        // `enabled` is a framework spec/status virtual, not a value property: extract it
        // (it would otherwise be rejected as unknown) and apply it as a lifecycle directive
        // AFTER the value batch commits. Only copies `values` when `enabled` is present.
        std::optional<bool> enable_req;
        const properties::json* to_apply = &values;
        properties::json filtered;
        if (values.is_object() && values.contains("enabled")) {
            const auto& e = values.at("enabled");
            if (!e.is_boolean()) {
                logger()->error("{}: 'enabled' must be a boolean", m_id);
                throw properties::validation_error("enabled (must be a boolean)");
            }
            enable_req = e.get<bool>();
            filtered = values;
            filtered.erase("enabled");
            to_apply = &filtered;
        }

        // Apply the value properties (skip the park entirely when only `enabled` was sent).
        if (!(to_apply->is_object() && to_apply->empty())) {
            try {
                m_park.with_worker_parked([&] {
                    // apply() returns the aggregate RFC-7396 diff of what actually
                    // changed; pass it to the reaction hook and skip the hook entirely
                    // on a no-op batch (nothing changed -> nothing to react to). apply()
                    // also STAGES any config<T> on_apply reaction.
                    const properties::json diff = m_prop_set.apply(*to_apply, config, allow_unknown);
                    if (!diff.empty()) {
                        // The batch is already committed and live; a throwing reaction hook
                        // must not turn a successful PATCH into a 400-while-live (symmetric
                        // with the on_change listener success-with-warnings policy). Log it.
                        try {
                            property_change_handler(diff);
                        } catch (const std::exception& ex) {
                            logger()->warn("{}: property_change_handler failed (values already applied): {}", m_id,
                                           ex.what());
                        } catch (...) {
                            logger()->warn("{}: property_change_handler failed (values already applied): "
                                           "unknown exception",
                                           m_id);
                        }
                    }
                });
                // A running worker drains staged config<T> reactions at its loop-top. If
                // there is NO live worker (INITIALIZE-time load, a stopped component, or a
                // source component that has no process() loop), run them inline now — there
                // is no process() to race, and the reaction must still take effect. The
                // atomic mailbox flag makes this safe against a concurrently-starting worker.
                if (!m_park.has_worker()) {
                    run_reactions_parked();
                }
            } catch (const properties::unknown_property& err) {
                logger()->error("{}: unknown property '{}'", m_id, err.name);
                throw;
            } catch (const properties::config_violation& err) {
                logger()->error("{}: property '{}' is not runtime configurable", m_id, err.name);
                throw;
            } catch (const properties::validation_error& err) {
                logger()->error("{}: property change rejected: '{}'", m_id, err.name);
                throw;
            } catch (const std::exception& ex) {
                logger()->error("{}: property error: {}", m_id, ex.what());
                throw;
            } catch (...) {
                // A non-standard exception out of a validator or a prepare/commit step would
                // otherwise escape set_properties() entirely — past the REST layer's
                // std::exception handlers and out of the worker on a self-write. Normalize it
                // so the failure still surfaces as an error rather than a terminate.
                logger()->error("{}: property error: unknown exception", m_id);
                throw std::runtime_error(m_id + ": property error: unknown exception");
            }
        }

        // Lifecycle directive: only reached if the value batch above committed (a failed
        // apply rethrows). The write IS the action — a RUNTIME write reconciles the worker
        // immediately (start/stop); an INITIALIZE write just records the desired state (the
        // app start sequence / apply_lifecycle_changes reconciles it). Idempotent, and
        // immune to the old no-op re-enable trap (acts on the actual running state).
        if (enable_req.has_value()) {
            m_desired_enabled.store(*enable_req, std::memory_order_release);
            if (config == properties::config_type::RUNTIME) {
                {
                    std::scoped_lock life{m_lifecycle_mtx};
                    reconcile_enabled_locked();
                }
                // If the reconcile STOPPED the worker, a reaction staged by the value batch
                // above is now undrained — drain it here (the loop-top drain is gone with the
                // worker). Lock released, so a lifecycle-touching on_apply is safe. If the
                // reconcile STARTED a worker, it drains at its own loop-top. CONTAINED: the
                // value batch is already committed and the lifecycle change already took
                // effect, so a drain failure must not retroactively fail the write.
                if (!m_park.has_worker()) {
                    run_reactions_contained();
                }
            }
        }
    }

    /**
     * @brief Hook invoked once (with the worker parked) after a successful
     * set_properties() batch that actually changed something, receiving the
     * aggregate RFC-7396 @p diff (`{property: sub-diff}`) of what changed.
     * Override this to react to the committed state — the diff lets a single-field
     * write react narrowly instead of rebuilding everything. Per-property reactions
     * can also be attached via typed_property::on_change().
     */
    virtual auto property_change_handler(const properties::json& diff) -> void { (void)diff; }

    /**
     * @brief Run @p fn under a shared property read-lock
     *
     * Excludes concurrent property writers/swaps (which run under the park
     * handshake's unique side), so REST/introspection read paths see a
     * consistent property_set. The worker hot path does NOT take this lock —
     * its consistency comes from the park, not the read-lock.
     */
    template <typename Fn>
    auto with_property_read_lock(Fn&& fn) const -> decltype(auto) {
        return m_park.with_reader_lock(std::forward<Fn>(fn));
    }

    auto log_level(composite::log_level level) const -> void { m_logger->set_level(level); }

    /**
     * @brief Set the CPU affinity for this component's thread
     * @param cpuset CPU set to apply when thread starts
     *
     * This must be called before start(). The affinity will be applied
     * when the component thread is created.
     */
    auto set_cpu_affinity(const cpu_set_t& cpuset) -> void { m_cpu_affinity = cpuset; }

    /**
     * @brief Reconcile the worker to the desired `enabled` state.
     *
     * `enabled` is a framework spec/status virtual, not a value property: the desired
     * state starts/stops the worker. A RUNTIME `enabled` write reconciles immediately
     * (the write IS start/stop), so this is only needed after an INITIALIZE-context
     * `enabled` write (config load) or as an explicit reconcile.
     *
     * NOT a compatibility shim, despite sharing a name with the pre-0.5 two-step API:
     * this IS the INITIALIZE-context reconcile path, and `application::start()` calls it
     * per component to bring a config-loaded graph up. Unlike the old version it has no
     * change-pending flag, so it cannot miss a no-op re-enable after a direct stop:
     * it reconciles against the worker's ACTUAL state, not against a "something
     * changed" hint that a direct stop() would have left stale.
     */
    auto apply_lifecycle_changes() -> void {
        {
            std::scoped_lock life{m_lifecycle_mtx};
            reconcile_enabled_locked();
        }
        // If the reconcile STOPPED the worker, drain any staged reaction (no worker to do
        // it; lock released so a lifecycle-touching on_apply is safe). If it started one,
        // the new worker drains it at loop-top. CONTAINED: application::start() calls this
        // per component and reports which ones failed to START — a post-reconcile drain
        // failure is not a start failure and must not be reported as one.
        if (!m_park.has_worker()) {
            run_reactions_contained();
        }
    }

    /// Drain staged config<T> reactions with on_apply serialized under the park's data
    /// write-lock, instead of lock-free. Callers use this only when there is no live worker
    /// to drain at its loop-top (a stopped/source/INITIALIZE-time component); with_worker_parked
    /// then takes its inline-writer-gated path (park.hpp run_inline_gated), which holds m_data_mtx
    /// and registers in m_inline_writers — so two concurrent inline REST writers no longer both
    /// run on_apply outside every lock (on_apply reads m_cfg->m_value, incl. std::string
    /// members, so the unserialized version was a UAF, not just a torn scalar). Park-owner reentrancy
    /// keeps it safe if this thread already owns the park (a self-writing on_apply).
    auto run_reactions_parked() -> void {
        m_park.with_worker_parked([this] { m_prop_set.run_pending_reactions(); });
    }

    /// Record (or clear, with an empty string) the detail behind a finish_reason::error.
    /// noexcept: called from the worker's own exception handlers and from the start-up
    /// failure path, where a throw would defeat the point.
    auto set_finish_error(std::string_view what) noexcept -> void {
        try {
            const std::scoped_lock lk{m_finish_error_mtx};
            m_finish_error.assign(what);
        } catch (...) { // NOLINT(bugprone-empty-catch) — a failed assign must not mask the real error
        }
    }

    /// run_reactions_parked() that cannot unwind. The reactions themselves are already
    /// contained (property_set::run_pending_reactions), but the PARK can still throw —
    /// with_worker_parked() raises "worker failed to park within timeout" on a wedged
    /// worker, which is not user code and can therefore surface on a stop or teardown path
    /// where unwinding is fatal. Every caller that drains from stop(), a destructor, or a
    /// lifecycle reconcile uses this; set_properties() keeps the throwing form so a genuine
    /// park failure still reaches the REST caller.
    auto run_reactions_contained() noexcept -> void {
        try {
            run_reactions_parked();
        } catch (const std::exception& ex) {
            log_contained_failure("staged reaction drain failed", ex.what());
        } catch (...) {
            log_contained_failure("staged reaction drain failed", "unknown exception");
        }
    }

    /// Log a contained failure without ever throwing. The containment wrappers are noexcept
    /// and run from destructors, so a throwing logger (allocation, formatting) would defeat
    /// the very guarantee they exist to provide.
    auto log_contained_failure(const char* what, const char* detail) noexcept -> void {
        try {
            logger()->error("{}: {}: {}", m_id, what, detail);
        } catch (...) { // NOLINT(bugprone-empty-catch) — logging must not defeat containment
        }
    }

    /// Desired (spec) enabled state — what the operator/config asked for.
    [[nodiscard]] auto is_enabled() const -> bool { return m_desired_enabled.load(std::memory_order_acquire); }
    /// Observed (status) running state — whether a worker is currently live.
    [[nodiscard]] auto is_running() const -> bool { return m_park.has_worker(); }

    /// Whether the worker self-terminated (process() returned FINISH or threw). Distinct from an
    /// external stop(): a stopped component that never self-finished reports finish_reason::none.
    [[nodiscard]] auto is_finished() const -> bool {
        return m_finish_reason.load(std::memory_order_acquire) != finish_reason::none;
    }
    /// Why the worker self-terminated (none if it did not, or was stopped externally).
    [[nodiscard]] auto finished_reason() const -> finish_reason {
        return m_finish_reason.load(std::memory_order_acquire);
    }

    /**
     * @brief Detail for a finish_reason::error — the what() of the exception that ended
     *        the worker, or "unknown exception" for a non-std throw.
     * @return The message, or an empty string when the component did not fail.
     *
     * finished_reason() says only THAT the component failed; a control plane or an
     * embedding caller generally needs to know WHY without scraping the log. Set once as
     * the worker exits (or when start-up itself fails) and cleared on the next start.
     * Cold path: a mutex, not an atomic — this is read by REST introspection, never on
     * the data path.
     */
    [[nodiscard]] auto finish_error() const -> std::string {
        const std::scoped_lock lk{m_finish_error_mtx};
        return m_finish_error;
    }

protected:
    explicit component(std::string_view id)
        : m_id(id), m_logger(std::make_shared<composite::logger>(std::string{id})), m_park(std::string{id}) {
        if (m_id.empty()) {
            throw std::invalid_argument("component id cannot be empty");
        }

        // Register component lifecycle metrics
        register_lifecycle_metrics();

        // When a property write needs to park the worker, wake any input port the
        // worker is blocked on so it reaches a park point promptly.
        m_park.set_poke([this] { on_park_requested(); });

        // A post-commit on_change listener that throws is non-fatal: the value is
        // already committed and live, so log it as a warning rather than turning a
        // successful PATCH into a 400 (success-with-warnings).
        m_prop_set.set_listener_error_handler([this](const std::string& name, const char* what) {
            logger()->warn("{}: property '{}' change listener failed (value already applied): {}", m_id, name, what);
        });

        // Same policy for a staged config<T> on_apply reaction, which additionally runs at
        // the worker loop-top, in ~component, and during graph teardown — boundaries an
        // exception must never cross. Contained in property_set::run_pending_reactions();
        // this is where the failure becomes visible.
        m_prop_set.set_reaction_error_handler([this](const std::string& name, const char* what) {
            logger()->error("{}: config '{}' on_apply reaction failed (values already applied): {}", m_id, name, what);
        });

        add_property("noop_thread_delay", m_delay).units("ns");
        add_property("yield_interval", m_yield_interval, properties::config_type::RUNTIME)
            .validate([](const uint32_t& v) { return v >= 1; }, "yield_interval must be >= 1")
            .units("1");
        // `enabled` is NOT registered as a value property — it is a framework spec/status
        // virtual handled directly by set_properties (the write IS start/stop) and reported
        // by property_state() as desired + observed. See is_enabled()/reconcile_enabled_locked().
        // Opt-in per-iteration process() timing. Off by default so the hot path
        // reads no clock (inversion principle 4); flip at runtime via REST/config
        // to populate the composite.component.process_time histogram. The write
        // parks the worker, so the worker's lock-free read is never torn.
        add_property("measure_process_time", m_measure_process_time, properties::config_type::RUNTIME);

        // EOS-by-default: when process() returns NOOP with every input drained + producer-closed
        // (inputs_at_end()), the base synthesizes a clean FINISH so the component self-completes and
        // EOS propagates downstream. On by default. Set false for the rare component that must keep
        // running past its inputs (e.g. one that also produces on a timer). Worker reads it lock-free;
        // swapped only while parked.
        add_property("finish_at_end", m_finish_at_end, properties::config_type::RUNTIME);

        // Resilience (error_policy): if process() throws, restart-with-backoff instead of finishing.
        // error_restart_max = 0 (default) keeps the strict behaviour (a throw -> FINISH/error). A
        // value N > 0 means the worker retries process() after an exponential backoff, up to N
        // consecutive failures, before giving up (FINISH/error). A successful iteration resets the
        // counter. The worker reads these on the (cold) error path only; swapped under park.
        add_property("error_restart_max", m_error_restart_max, properties::config_type::RUNTIME).units("1");
        add_property("error_restart_backoff_ms", m_error_restart_backoff_ms, properties::config_type::RUNTIME)
            .validate([](const uint32_t& v) { return v >= 1; }, "error_restart_backoff_ms must be >= 1")
            .units("ms");
    }

    auto logger() const -> std::shared_ptr<composite::logger> { return m_logger; }

    /// Worker lifecycle extension points, invoked by start_locked()/stop_locked() so that EVERY
    /// start/stop path runs them — the direct virtual start()/stop() AND the enabled-reconcile path
    /// (application::start() / a RUNTIME `enabled` write, which call the private *_locked helpers,
    /// not the virtual start()/stop()). A subclass that owns auxiliary worker resources (e.g.
    /// pipeline_component's thread pool) MUST hook here rather than override start()/stop(), or those
    /// resources never spin up when the component is started via the application. on_worker_start()
    /// runs BEFORE the main worker thread is spawned; on_worker_stop() runs AFTER it is joined.
    virtual auto on_worker_start() -> void {}
    virtual auto on_worker_stop() -> void {}

    /**
     * @brief Hook invoked when a property write needs the worker to park
     *
     * Default wakes every input port so a worker blocked in get_data() returns
     * to a park point promptly (instead of stalling until the receive timeout).
     * Components that run their own threads or long inner loops should override
     * to also nudge those to a park point.
     */
    virtual auto on_park_requested() -> void {
        // With non-blocking SPSC input ports the worker is never blocked in
        // get_data(): it reaches a park point at the top of its loop, and its
        // NOOP idle_wait is interrupted by the park coordinator's CV. So no
        // per-port wake is needed. Override to nudge a custom blocking wait.
    }

    // ========================================================================
    // Metrics Convenience Methods
    // ========================================================================

    /**
     * @brief Create a counter metric for this component
     *
     * The metric name is used verbatim (it is NOT prefixed with the component ID).
     * The component's identity is carried in an auto-added "component_id" label, so
     * the same metric name (e.g. "packets_sent") can be shared across components and
     * disambiguated by label. Metrics are removed on destruction via the label.
     *
     * @param name Metric name (used verbatim — component identity is carried in the
     *             auto-added "component_id" label, NOT a name prefix)
     * @param description Human-readable description
     * @param unit Unit of measurement (default "1")
     * @param labels Additional labels (component_id is auto-added)
     * @return Reference to the created counter
     */
    auto create_counter(std::string_view name, std::string_view description = "", std::string_view unit = "1",
                        metrics::labels_t labels = {}) -> metrics::counter<uint64_t>& {
        labels.emplace_back("component_id", m_id);
        return metrics::registry::instance().create_counter(std::string{name}, std::string{description},
                                                            std::string{unit}, std::move(labels));
    }

    /**
     * @brief Create an up/down counter metric for this component
     *
     * @param name Metric name (used verbatim — component identity is carried in the
     *             auto-added "component_id" label, NOT a name prefix)
     * @param description Human-readable description
     * @param unit Unit of measurement (default "1")
     * @param labels Additional labels (component_id is auto-added)
     * @return Reference to the created updown_counter
     */
    auto create_updown_counter(std::string_view name, std::string_view description = "", std::string_view unit = "1",
                               metrics::labels_t labels = {}) -> metrics::updown_counter<int64_t>& {
        labels.emplace_back("component_id", m_id);
        return metrics::registry::instance().create_updown_counter(std::string{name}, std::string{description},
                                                                   std::string{unit}, std::move(labels));
    }

    /**
     * @brief Create a gauge metric for this component
     *
     * @param name Metric name (used verbatim — component identity is carried in the
     *             auto-added "component_id" label, NOT a name prefix)
     * @param description Human-readable description
     * @param unit Unit of measurement (default "1")
     * @param labels Additional labels (component_id is auto-added)
     * @return Reference to the created gauge
     */
    auto create_gauge(std::string_view name, std::string_view description = "", std::string_view unit = "1",
                      metrics::labels_t labels = {}) -> metrics::gauge<double>& {
        labels.emplace_back("component_id", m_id);
        return metrics::registry::instance().create_gauge(std::string{name}, std::string{description},
                                                          std::string{unit}, std::move(labels));
    }

    /**
     * @brief Create a histogram metric for this component
     *
     * @param name Metric name (used verbatim — component identity is carried in the
     *             auto-added "component_id" label, NOT a name prefix)
     * @param description Human-readable description
     * @param unit Unit of measurement
     * @param boundaries Bucket boundaries
     * @param labels Additional labels (component_id is auto-added)
     * @return Reference to the created histogram
     */
    auto create_histogram(std::string_view name, std::string_view description, std::string_view unit,
                          std::vector<double> boundaries, metrics::labels_t labels = {}) -> metrics::histogram& {
        labels.emplace_back("component_id", m_id);
        return metrics::registry::instance().create_histogram(
            std::string{name}, std::string{description}, std::string{unit}, std::move(boundaries), std::move(labels));
    }

    /**
     * @brief Create a histogram with power-of-2 boundaries for this component
     *
     * Convenience for latency-style distributions: the boundaries are 1, 2, 4, 8, …
     * (i.e. 2^0 … 2^(num_buckets-2)). Bucket lookup is O(log n) binary search like any
     * histogram — the boundaries are simply pre-computed powers of two.
     *
     * @param name Metric name (used verbatim — component identity is carried in the
     *             auto-added "component_id" label, NOT a name prefix)
     * @param description Human-readable description
     * @param unit Unit of measurement
     * @param num_buckets Number of buckets, range 2..64 (default 20: boundaries 1..2^18 = 262144)
     * @param labels Additional labels (component_id is auto-added)
     * @return Reference to the created histogram
     */
    auto create_histogram_pow2(std::string_view name, std::string_view description = "", std::string_view unit = "1",
                               std::size_t num_buckets = 20, metrics::labels_t labels = {}) -> metrics::histogram& {
        labels.emplace_back("component_id", m_id);
        return metrics::registry::instance().create_histogram_pow2(std::string{name}, std::string{description},
                                                                   std::string{unit}, num_buckets, std::move(labels));
    }

private:
    std::string m_id;
    std::shared_ptr<composite::logger> m_logger;
    park_coordinator m_park; ///< park handshake: lock-free process(), parked property writes
    std::optional<std::jthread> m_thread;
    uint32_t m_delay{DEFAULT_DELAY};
    uint32_t m_yield_interval{DEFAULT_YIELD_INTERVAL}; ///< sched_yield once per N consecutive NORMALs (worker reads
                                                       ///< lock-free; swapped under park)
    // `enabled` is NOT a value property: it is the framework-owned DESIRED lifecycle
    // state (the "spec"). The observed state is computed from the worker (m_park /
    // m_thread), so there is no bound mirror to desync. A RUNTIME write reconciles
    // immediately (the write IS start/stop); property_state reports desired + observed.
    std::atomic_bool m_desired_enabled{true};
    // Completion status: set by the worker when it self-terminates (FINISH / exception), cleared on
    // (re)start. m_worker_done + its CV let wait_until_finished() block until the worker exits (for
    // ANY reason). m_worker_done starts true (no worker running yet).
    std::atomic<finish_reason> m_finish_reason{finish_reason::none};
    mutable std::mutex m_finish_error_mtx; ///< guards m_finish_error (cold: worker exit vs REST read)
    std::string m_finish_error;            ///< what() behind a finish_reason::error; see finish_error()
    std::mutex m_finished_mtx;
    std::condition_variable m_finished_cv;
    bool m_worker_done{true};
    // Pairs on_worker_start() with on_worker_stop() so the latter runs EXACTLY ONCE per successful
    // start, whether the reap is reached by stop_locked() or by a self-finishing worker's completion
    // tail in thread_func() — never both, and never zero. Exchange-guarded; see
    // worker_resources_down().
    std::atomic<bool> m_worker_resources_up{false};
    bool m_measure_process_time{
        false}; ///< "measure_process_time" property: gates the hot-path clock reads (default off → zero clocks;
                ///< principle 4). Worker reads lock-free; swapped only while parked.
    bool m_finish_at_end{
        true}; ///< "finish_at_end" property: NOOP + inputs_at_end() → synthesized FINISH so the component
               ///< self-completes on upstream EOS. Worker reads lock-free; swapped only while parked.
    // Resilience: error_policy = restart-with-backoff (read on the cold error path only).
    uint32_t m_error_restart_max{
        0}; ///< "error_restart_max": max consecutive process() throws to retry (0 = stop on first error, the default)
    uint32_t m_error_restart_backoff_ms{
        100};                          ///< "error_restart_backoff_ms": initial backoff; doubles each retry, capped
    std::uint32_t m_error_restarts{0}; ///< worker-local consecutive-error counter (reset on a successful iteration)
    std::optional<cpu_set_t> m_cpu_affinity;
    port_set m_port_set;
    properties::property_set m_prop_set;
    std::mutex m_lifecycle_mtx; ///< serializes start/stop/enabled reconcile
    std::vector<connection> m_connections;
    mutable std::mutex m_connections_mtx; ///< guards m_connections (connect/disconnect vs REST readers)
    std::map<std::string, std::size_t> m_saved_input_depths;

    // Lifecycle metrics (registered in constructor)
    metrics::counter<uint64_t>* m_process_calls{nullptr};
    metrics::counter<uint64_t>* m_noop_count{nullptr};
    metrics::histogram* m_process_time{nullptr};
    metrics::gauge<double>* m_state{nullptr};

    /**
     * @brief Register component lifecycle metrics
     *
     * Creates the following metrics in the global registry:
     * - composite.component.process_calls: Number of times process() was called
     * - composite.component.noop_count: Number of times process() returned NOOP
     * - composite.component.process_time: Histogram of process() execution time in microseconds
     * - composite.component.state: Current state (0=stopped, 1=running)
     */
    auto register_lifecycle_metrics() -> void {
        auto& registry = metrics::registry::instance();

        metrics::labels_t labels = {{"component_id", m_id}};

        m_process_calls = &registry.get_or_create_counter("composite.component.process_calls",
                                                          "Number of times process() was called", "1", labels);

        m_noop_count = &registry.get_or_create_counter("composite.component.noop_count",
                                                       "Number of times process() returned NOOP", "1", labels);

        // Power-of-2 histogram for process time in microseconds
        // 20 buckets covers 1µs to ~1s which handles most signal processing scenarios
        m_process_time = &registry.get_or_create_histogram_pow2("composite.component.process_time",
                                                                "Time spent in process() call", "us", 20, labels);

        m_state = &registry.get_or_create_gauge("composite.component.state", "Component state (0=stopped, 1=running)",
                                                "1", labels);

        // Initialize state to stopped
        m_state->set(0.0);
    }

    /**
     * @brief Pause all input ports by setting their queue depth to 0
     *
     * Saves current depth values and sets all input port depths to 0,
     * preventing queue growth and memory bloat when component is stopped.
     * Depths can be restored via resume_input_ports().
     */
    /// Backoff delay for the Nth consecutive error restart: initial << (N-1), capped at 30s (also
    /// guards against the shift overflowing). Read on the cold error path only.
    [[nodiscard]] auto error_backoff_delay() const -> std::chrono::milliseconds {
        const std::uint32_t shift = std::min<std::uint32_t>(m_error_restarts > 0 ? m_error_restarts - 1 : 0, 20);
        std::uint64_t ms = static_cast<std::uint64_t>(m_error_restart_backoff_ms) << shift;
        ms = std::min<std::uint64_t>(ms, 30'000);
        return std::chrono::milliseconds{ms};
    }

    auto pause_input_ports() -> void {
        m_saved_input_depths.clear();
        for (const auto& [name, port] : m_port_set.ports()) {
            if (auto* input_port = dynamic_cast<input_port_base*>(port)) {
                // Save current depth
                m_saved_input_depths[name] = input_port->depth();
                // Set to 0 to drop all incoming data
                input_port->depth(0);
                logger()->debug("Paused input port '{}' (saved depth: {})", name, m_saved_input_depths[name]);
            }
        }
    }

    /**
     * @brief Resume all input ports by restoring their saved queue depths
     *
     * Restores depths that were saved by pause_input_ports(). If no saved
     * depth exists for a port, it remains at its current depth.
     */
    auto resume_input_ports() -> void {
        for (const auto& [name, saved_depth] : m_saved_input_depths) {
            if (auto* input_port = dynamic_cast<input_port_base*>(m_port_set.get_port<input_port_base>(name))) {
                input_port->depth(saved_depth);
                logger()->debug("Resumed input port '{}' (restored depth: {})", name, saved_depth);
            }
        }
        m_saved_input_depths.clear();
    }

    // ------------------------------------------------------------------------
    // Lifecycle mechanics (assume m_lifecycle_mtx is held by the public wrappers)
    // ------------------------------------------------------------------------

    // Drive the worker to match the desired `enabled` spec. Caller holds m_lifecycle_mtx.
    // Two distinct signals, because a worker that self-stopped (process() returned FINISH
    // or threw) has EXITED the park (state EXITING) but its jthread handle is NOT reset
    // until a join (the only m_thread.reset() is in stop_locked):
    //  - START decision keys off LIVENESS (a live or starting worker exists). A FINISH-
    //    exited worker is NOT live, so a re-enable correctly RESTARTS it (without this it
    //    reintroduces the re-enable no-op trap). start_locked() joins any stale handle first.
    //  - STOP decision keys off the HANDLE, so a disable also cleans up a finished worker.
    auto reconcile_enabled_locked() -> void {
        const bool want = m_desired_enabled.load(std::memory_order_acquire);
        const bool has_handle = m_thread.has_value();
        // "live" = a handle that has not exited the park (running or still starting up).
        const bool live = has_handle && m_park.current_state() != park_coordinator::state::EXITING;
        if (want && !live) {
            logger()->debug("Enabling component '{}'", m_id);
            resume_input_ports();
            start_locked();
        } else if (!want && has_handle) {
            logger()->debug("Disabling component '{}'", m_id);
            pause_input_ports();
            stop_locked();
        }
    }

    /// Tear down subclass worker resources (on_worker_stop) EXACTLY ONCE per successful
    /// on_worker_start(). Called from BOTH stop_locked() (external stop / disable) and a
    /// self-finishing worker's completion tail; the atomic exchange guarantees only the first
    /// caller invokes on_worker_stop(), so the two never double-reap (and the pool is never
    /// left un-joined). The winning call is always ordered w.r.t. the worker via a join, so
    /// on_worker_stop() never runs concurrently with the pool it tears down.
    auto worker_resources_down() -> void {
        if (m_worker_resources_up.exchange(false, std::memory_order_acq_rel)) {
            // USER hook, reached on every teardown path (stop, disable-reconcile, worker
            // self-termination, destructor). A throw must not abort the remainder of the
            // stop sequence, and the exchange above has already marked the resources down,
            // so a rethrow could not be retried anyway.
            try {
                on_worker_stop();
            } catch (const std::exception& ex) {
                log_contained_failure("on_worker_stop() failed", ex.what());
            } catch (...) {
                log_contained_failure("on_worker_stop() failed", "unknown exception");
            }
        }
    }

    auto start_locked() -> void {
        if (m_thread.has_value()) {
            stop_locked(); // clean stop before a restart
        }
        // Clear completion status for the new run: not finished, error counter reset (else a
        // restarted component gives up early on the STALE consecutive-error count from the prior run).
        m_finish_reason.store(finish_reason::none, std::memory_order_release);
        set_finish_error({}); // a new run has no failure yet; drop the previous run's detail
        m_error_restarts = 0;
        // Re-open our outputs' downstream inputs: a (re)starting producer will send data again, so a
        // stale end-of-stream latch from a prior completed run must not leave downstream reporting
        // at_end() forever (nor mis-fire a premature EOS on the next completion).
        for (const auto& [name, port] : m_port_set.ports()) {
            if (auto* out = dynamic_cast<output_port_base*>(port)) {
                out->reopen();
            }
        }
        // m_worker_done=false BEFORE we spawn, so the worker's exit (which sets it true) is always
        // ordered after this reset even if the worker exits immediately.
        {
            std::scoped_lock lk{m_finished_mtx};
            m_worker_done = false;
        }
        // Bring up subclass worker resources (e.g. pipeline_component's pool), THEN spawn the main
        // worker — both under ONE try. A throw from on_worker_start() is realistic, not exotic:
        // start_pool() spawns N std::threads in a loop and a mid-loop std::thread ctor can throw
        // std::system_error under resource exhaustion (num_workers is runtime-settable up to 1024).
        // If that escaped we would (a) leak the threads it already spawned, (b) leave m_thread unset
        // so stop() becomes a permanent no-op with no way to reap them, and (c) ABORT the process on
        // the operator's retry — start_pool()'s first act is m_pool.clear(), destroying a vector of
        // still-joinable threads (std::terminate). So on ANY failure here we reap whatever came up
        // (on_worker_stop) and record the run as errored.
        try {
            on_worker_start();
            // Pair-flag set BEFORE the worker can run, so a fast self-finishing worker's reap
            // (worker_resources_down) always observes it set — see worker_resources_down().
            m_worker_resources_up.store(true, std::memory_order_release);
            // Lambda (rather than &component::thread_func member-pointer) so std::jthread
            // reliably injects the stop_token across libstdc++ versions.
            m_thread.emplace([this](std::stop_token token) { thread_func(token); });
        } catch (...) {
            // No live worker exists (on_worker_start threw, or emplace threw before spawning): undo
            // the done-flag so wait_until_finished() doesn't block, report the failed run as errored
            // (distinct from "never started"), then tear down any partial worker resources HERE —
            // safe because nothing is running to reap them, so this cannot race a worker's own reap.
            {
                std::scoped_lock lk{m_finished_mtx};
                m_worker_done = true;
            }
            m_finish_reason.store(finish_reason::error, std::memory_order_release);
            try {
                throw; // re-inspect the in-flight exception to record its detail, then rethrow below
            } catch (const std::exception& e) {
                set_finish_error(e.what());
            } catch (...) {
                set_finish_error("unknown exception");
            }
            m_worker_resources_up.store(false, std::memory_order_release);
            // Contained: this cleanup hook runs while the START failure is in flight. Letting
            // it throw here would REPLACE the original exception with a teardown artifact,
            // losing the reason the start actually failed (already recorded above).
            try {
                on_worker_stop();
            } catch (const std::exception& ex) {
                log_contained_failure("on_worker_stop() failed while unwinding a failed start", ex.what());
            } catch (...) {
                log_contained_failure("on_worker_stop() failed while unwinding a failed start", "unknown exception");
            }
            throw;
        }
        pthread_setname_np(m_thread->native_handle(), m_id.c_str());

        // Apply CPU affinity if configured
        if (m_cpu_affinity.has_value()) {
            if (pthread_setaffinity_np(m_thread->native_handle(), sizeof(*m_cpu_affinity), &(*m_cpu_affinity)) != 0) {
                m_logger->warn("Failed to set thread CPU affinity: {}", strerror(errno));
            }
        }

        if (m_state) {
            m_state->set(1.0);
        }
    }

    /// How long either shutdown wait stays quiet before it starts reporting.
    static constexpr auto k_stop_report_interval = std::chrono::seconds(5);

    /// Wait for the worker to leave thread_func, REPORTING progress instead of waiting mutely.
    ///
    /// The join cannot be abandoned: after a timeout the worker would still be running
    /// against an object stop() is about to tear down (and ~component is one of stop()'s
    /// callers), so giving up would trade a diagnosable hang for a use-after-free. The
    /// framework's own idle wait is bounded AND stop-token-aware, so a worker only fails to
    /// exit when USER code does not return -- process(), or a hook it calls. What this can
    /// fix is the silence: name the component, say what to look at, and keep saying it.
    auto await_worker_exit_reporting() -> void {
        for (int elapsed = 0;; elapsed += static_cast<int>(k_stop_report_interval.count())) {
            if (wait_until_finished(k_stop_report_interval)) {
                break; // worker has left thread_func; the join below returns immediately
            }
            logger()->warn("{}: worker still running {}s into stop() — process() (or something it "
                           "calls) is not returning. It must observe the stop token and use bounded "
                           "waits; override on_park_requested() to interrupt a custom wait.",
                           m_id, elapsed + static_cast<int>(k_stop_report_interval.count()));
        }
    }

    /// Wait out any external property write still inside with_worker_parked(), reporting
    /// rather than spinning mutely. Same reasoning as join_worker_reporting(): the only way
    /// this does not finish is a user on_apply / property_change_handler that never returns,
    /// and we cannot proceed to tear down while one is still touching our data.
    auto drain_park_reporting() -> void {
        for (int elapsed = 0; !m_park.drain_in_flight_for(k_stop_report_interval);
             elapsed += static_cast<int>(k_stop_report_interval.count())) {
            logger()->warn("{}: a property write has been in flight {}s into stop() — a config "
                           "on_apply or property_change_handler is not returning.",
                           m_id, elapsed + static_cast<int>(k_stop_report_interval.count()));
        }
    }

    auto stop_locked() -> void {
        if (!m_thread.has_value()) {
            return;
        } // idempotent

        // Close the door on new external property writes for the whole teardown: draining
        // in-flight writers only means something if no further writer can arrive behind the
        // drain. RAII so a throw cannot leave the door shut (which would hang every later write).
        const park_coordinator::admission_gate gate{m_park};
        request_worker_exit_locked();
        await_worker_exit_reporting();
        finish_stop_locked();
    }

    /// Phase 1 of a stop: latch the exit request and nudge the worker. NON-BLOCKING.
    /// Idempotent — the stop request is a latch, so re-requesting is a no-op.
    auto request_worker_exit_locked() -> void {
        auto source = m_thread->get_stop_source();
        if (source.stop_possible()) {
            if (source.stop_requested()) {
                // Already latched AND nudged by an earlier call (e.g. request_stop() followed by
                // try_stop()). The nudge only has to land once — the request is a latch, and the
                // worker's idle wait is stop-token-aware — while on_park_requested() is USER code
                // that may be slow. Paying for it twice would charge a signal-then-collect
                // shutdown double the hook cost.
                return;
            }
            source.request_stop();
        }
        m_park.cancel_waiters(); // release a writer blocked waiting to park
        // Best-effort wake for a worker blocked in a custom wait. This is a USER hook sitting
        // one line before the SINGLE join site, so its failure must never skip the join:
        // stop() is reached from ~component via stop_contained(), and an unjoined worker
        // would then keep running against a destroyed derived object. Contain it here —
        // containing it at the stop() boundary alone would trade a loud terminate for a
        // silent use-after-free.
        try {
            on_park_requested(); // wake a worker blocked in get_data()
        } catch (const std::exception& ex) {
            log_contained_failure("on_park_requested() failed during stop", ex.what());
        } catch (...) {
            log_contained_failure("on_park_requested() failed during stop", "unknown exception");
        }
    }

    /// Phase 2 of a stop: the worker HAS exited — finish the teardown. Never call this while
    /// the worker may still be running: settle_stopped() would publish NO_WORKER against a
    /// live worker (letting inline writers race it), and worker_resources_down() would tear
    /// down resources it is still using.
    auto finish_stop_locked() -> void {
        m_thread.reset();        // the single join site; the worker has exited, so this returns at once
        m_park.settle_stopped(); // park state -> NO_WORKER
        // Ensure no EXTERNAL park call is still touching us before teardown. Skip the
        // wait when this stop() is REENTRANT from inside our own with_worker_parked (m_park_owner ==
        // this thread — e.g. a lifecycle-touching config on_apply calling stop() during the inline
        // reaction drain): we already hold m_data_mtx as the park owner, so external park calls are
        // excluded (blocked before touching our data), and waiting here would spin forever on our
        // OWN in-flight guard. Non-reentrant stops drain normally.
        if (!m_park.owned_by_current_thread()) {
            drain_park_reporting();
        }

        // Tear down subclass worker resources (e.g. pipeline_component's pool) AFTER the main worker
        // is joined — runs on every stop path (direct stop() AND app/reconcile disable). Exchange-
        // guarded so a worker that already self-finished (and reaped its own resources in
        // thread_func's completion tail) is not torn down a second time here.
        worker_resources_down();

        // NOTE: a reaction staged just before this stop is NOT drained here — that would
        // run on_apply while holding m_lifecycle_mtx, and an on_apply that touches the
        // lifecycle (a reentrant enabled/start/stop) would self-deadlock on the non-
        // recursive mutex. The public callers drain AFTER releasing the lock (see stop(),
        // set_properties, apply_lifecycle_changes), so the staged reaction still runs on
        // the stopping thread but lock-free.

        if (m_state) {
            m_state->set(0.0);
        }
    }

    /// doorbell re-check: does any input port currently hold data? Called by the worker
    /// after arming the doorbell (under a seq_cst fence) to catch a packet that arrived
    /// between its last NOOP and the arm — so it never sleeps with data already queued.
    /// Cold path (per idle wake, not per packet); the dynamic_casts are amortized there.
    [[nodiscard]] auto any_input_has_data() const -> bool {
        for (const auto& [name, port] : m_port_set.ports()) {
            if (const auto* in = dynamic_cast<const input_port_base*>(port)) {
                if (in->pending() != 0) {
                    return true;
                }
            }
        }
        return false;
    }

    /// reverse-doorbell re-check: can any output port accept data (a connected consumer has
    /// a free slot)? The AWAIT_OUTPUT idle path arms then re-reads this (absolute, not a delta —
    /// safe here precisely because process() DECLARED it is output-blocked, so re-checking and
    /// re-processing when a slot is free makes progress and never busy-spins). Cold path (per
    /// idle wake). NOTE: "any" output — a component with MULTIPLE outputs that is blocked on one
    /// specific full output while another is sendable can wake-spin; such a component should
    /// manage its own pacing rather than rely on AWAIT_OUTPUT (documented limitation).
    [[nodiscard]] auto any_output_can_send() const -> bool {
        for (const auto& [name, port] : m_port_set.ports()) {
            if (const auto* out = dynamic_cast<const output_port_base*>(port)) {
                if (out->can_send()) {
                    return true;
                }
            }
        }
        return false;
    }

    auto thread_func(std::stop_token token) -> void {
        using enum retval;
        m_park.worker_started();
        park_coordinator::exit_guard park_exit{m_park};
        std::uint32_t normal_streak = 0; // batched yield: consecutive NORMALs since the last sched_yield
        finish_reason exit_reason = finish_reason::none; // set iff the loop self-terminates (FINISH/throw)

        while (!token.stop_requested()) {
            m_park.park_point(); // quiesce here while a property write swaps; lock-free otherwise
            if (token.stop_requested()) {
                break;
            }

            // Run any config<T> on_apply reaction that a property write staged while
            // we were parked — on THIS (worker) thread, at loop-top, BEFORE process(). So
            // process() never observes a new config value alongside stale derived state
            // (the fft/psd use-after-free class). No-op when no reaction is staged.
            //
            // Reaction exceptions are contained inside run_pending_reactions() (per binding,
            // routed to the reaction error sink): this is a bare worker-thread frame, so an
            // escape here is std::terminate, exactly as it would be from process() — which
            // is why process() is wrapped below. The belt-and-braces guard keeps that true
            // even if the property layer ever grows a throwing path of its own.
            try {
                m_prop_set.run_pending_reactions();
            } catch (const std::exception& e) {
                logger()->error("component '{}' staged reaction drain threw: {}", m_id, e.what());
            } catch (...) {
                logger()->error("component '{}' staged reaction drain threw an unknown exception", m_id);
            }

            retval res{};
            bool errored = false; // set if process() threw this iteration (FINISH-with-reason=error)

            // Per-iteration timing is opt-out to zero: only read the clock when the
            // "measure_process_time" property is set (lock-free read; swapped only
            // while parked). Off by default → the hot path reads no clock at all.
            const bool measure = m_measure_process_time;
            std::chrono::steady_clock::time_point start;
            if (measure) {
                start = std::chrono::steady_clock::now();
            }
            try {
                res = process();
            } catch (const std::exception& e) {
                logger()->error("component '{}' process() threw an exception: {}", m_id, e.what());
                set_finish_error(e.what()); // retained for finish_error() / property_state()
                res = FINISH;
                errored = true;
            } catch (...) {
                logger()->error("component '{}' process() threw an unknown exception", m_id);
                set_finish_error("unknown exception");
                res = FINISH;
                errored = true;
            }

            // Record metrics (process_calls is a clock-free counter, always on)
            if (m_process_calls) {
                m_process_calls->inc();
            }
            if (measure && m_process_time != nullptr) {
                const auto elapsed_us =
                    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start)
                        .count();
                m_process_time->record(static_cast<double>(elapsed_us));
            }

            if (errored) {
                // error_policy = restart-with-backoff: retry process() after an exponential backoff
                // instead of finishing, up to error_restart_max CONSECUTIVE failures. A successful
                // iteration (below) resets the counter, so only a sustained fault gives up.
                if (m_error_restart_max > 0 && m_error_restarts < m_error_restart_max) {
                    ++m_error_restarts;
                    const auto backoff = error_backoff_delay();
                    logger()->warn("component '{}' retrying after error (attempt {}/{}), backoff {} ms", m_id,
                                   m_error_restarts, m_error_restart_max, static_cast<long long>(backoff.count()));
                    // Wait out the backoff on the park CV so a stop OR a property-write park request
                    // wakes us promptly — a raw sleep here would leave a concurrent set_properties
                    // (with_worker_parked) waiting for us to reach a park point until it times out.
                    m_park.wait_for_data(std::chrono::duration_cast<std::chrono::nanoseconds>(backoff), token);
                    if (token.stop_requested()) {
                        break;
                    }
                    continue; // retry (loop-top park_point applies any pending property write first)
                }
                // Give up: pause inputs, then fall through to the FINISH dispatch (exit_reason=error).
                pause_input_ports();
            } else {
                m_error_restarts = 0; // a non-throwing iteration clears the consecutive-error count
            }

            // EOS-by-default: a NOOP with every input drained + producer-closed means the
            // stream is over — synthesize a clean FINISH so the component self-completes and EOS
            // propagates (send_eos() fires below on finish_reason::completed). Sources (no inputs)
            // never trip this (inputs_at_end() == false). Give on_end_of_stream() one chance to emit
            // held/buffered data first; then fall through to the FINISH dispatch (errored == false
            // here, so exit_reason = completed). Opt out with finish_at_end = false.
            if (res == NOOP && m_finish_at_end && inputs_at_end()) {
                try {
                    on_end_of_stream();
                } catch (const std::exception& e) {
                    logger()->error("component '{}' on_end_of_stream() threw: {}", m_id, e.what());
                } catch (...) {
                    logger()->error("component '{}' on_end_of_stream() threw an unknown exception", m_id);
                }
                res = FINISH;
            }

            if (res == NOOP) {
                if (m_noop_count) {
                    m_noop_count->inc();
                }
                // doorbell: instead of always sleeping out m_delay, arm the doorbell so an
                // upstream producer's add_data wakes us the instant data arrives. The Dekker
                // re-check (arm -> seq_cst fence -> re-scan inputs) closes the window where a
                // packet lands between this NOOP and the arm: if any input now has data we skip
                // the sleep entirely; otherwise we wait until a signal / park / stop / the
                // m_delay fallback (the fallback keeps source components — which no producer
                // rings — polling at the NOOP cadence; the token makes a plain stop() wake us
                // immediately rather than wait out m_delay). disarm unconditionally so a later
                // signal_data cannot find us armed while we are off processing.
                m_park.arm_doorbell();
                std::atomic_thread_fence(std::memory_order_seq_cst);
                if (!any_input_has_data()) {
                    m_park.wait_for_data(std::chrono::nanoseconds{m_delay}, token);
                }
                m_park.disarm_doorbell();
                normal_streak = 0; // the idle wait is itself a (much larger) yield
            } else if (res == AWAIT_OUTPUT) {
                if (m_noop_count) {
                    m_noop_count->inc();
                }
                // reverse doorbell: process() declared it is blocked on a full output. Arm,
                // then re-check can_send() under a seq_cst fence (closes the window where a
                // consumer drained between this return and the arm — its pop's fence pairs with
                // ours); if still full, sleep until the consumer's pop signals our park (the
                // full->not-full edge), a park/stop, or the m_delay fallback. Absolute can_send
                // (not a delta) is correct because process() will return NORMAL the instant a
                // slot frees, so re-processing cannot busy-spin.
                m_park.arm_doorbell();
                std::atomic_thread_fence(std::memory_order_seq_cst);
                if (!any_output_can_send()) {
                    m_park.wait_for_data(std::chrono::nanoseconds{m_delay}, token);
                }
                m_park.disarm_doorbell();
                normal_streak = 0;
            } else if (res == FINISH) {
                exit_reason = errored ? finish_reason::error : finish_reason::completed;
                break;
            } else if (res == NORMAL) {
                // batched yield: sched_yield only once per m_yield_interval consecutive
                // NORMALs (yield is a syscall; this cuts it to ~1/N on a saturated stream).
                // park_point() at loop-top — not this yield — keeps property writes responsive.
                if (++normal_streak >= m_yield_interval) {
                    normal_streak = 0;
                    std::this_thread::yield();
                }
            }
            // NO_YIELD: loop immediately without yielding (component opted out; streak untouched)
        }

        // Completion: if the loop self-terminated (FINISH / exception, not an external stop), record
        // the reason and fire on_finished() ON THIS worker thread before it exits. Then, regardless
        // of how the loop ended, signal wait_until_finished() waiters that the worker is done.
        if (exit_reason != finish_reason::none) {
            m_finish_reason.store(exit_reason, std::memory_order_release);
            try {
                on_finished(exit_reason);
            } catch (const std::exception& e) {
                logger()->error("component '{}' on_finished() threw: {}", m_id, e.what());
            } catch (...) {
                logger()->error("component '{}' on_finished() threw an unknown exception", m_id);
            }
        }
        // EOS propagation: orderly completion closes our outputs (AFTER on_finished, so a component
        // that flushes final data there gets it out before the close), so downstream consumers reach
        // at_end() and can finish in turn — the chain completes end to end. An ERROR exit does NOT
        // send EOS (it was not orderly; downstream should not treat it as a clean end-of-stream).
        if (exit_reason == finish_reason::completed) {
            try {
                send_eos();
            } catch (const std::exception& e) {
                logger()->error("component '{}' send_eos() at completion threw: {}", m_id, e.what());
            } catch (...) {
            }
        }
        // Reap subclass worker resources on SELF-termination (FINISH / error). stop_locked() is NOT
        // in the call path of a self-finish, so without this a pipeline_component's pool would linger
        // (idle, but alive, holding N threads) until a later stop() / re-enable / destruction — a
        // real window now that pipeline_component self-completes on inputs_at_end(). Exchange-guarded
        // (see worker_resources_down) so a concurrent or subsequent stop_locked() cannot double-reap.
        // Ordered BEFORE the finished signal so a wait_until_finished() waiter observes a fully
        // quiesced component (pool joined). Safe from this (the main worker) thread: on_worker_stop()
        // joins the POOL threads, never this one.
        if (exit_reason != finish_reason::none) {
            try {
                worker_resources_down();
            } catch (const std::exception& e) {
                logger()->error("component '{}' on_worker_stop() at completion threw: {}", m_id, e.what());
            } catch (...) {
            }
            // Flip the exported state gauge to "stopped" — a self-finished worker is no longer running,
            // and stop_locked() (the only OTHER writer) is NOT in this call path, so without this the
            // composite.component.state metric would report 1.0/"running" forever after a batch/source
            // graph completes, contradicting is_running(). Ordered after the reap so the gauge flips to
            // 0 only once the component is fully quiesced, mirroring stop_locked().
            if (m_state) {
                m_state->set(0.0);
            }
        }
        // ALWAYS signal completion last (even if on_finished/send_eos/reap above threw) so a
        // wait_until_finished() waiter can never be stranded.
        {
            std::scoped_lock lk{m_finished_mtx};
            m_worker_done = true;
        }
        m_finished_cv.notify_all();
    }

}; // class component

/**
 * @brief Create a component whose destruction stops it FIRST — while the leaf type is intact.
 *
 * The returned shared_ptr's deleter calls stop() before destroying the object, so the worker is
 * joined (and subclass resources are reaped via the on_worker_stop() hook) while virtual dispatch
 * still reaches the leaf class. This closes the destruction-order use-after-free where the base
 * ~component's own stop() runs only after the derived members — and the leaf vtable — are already
 * gone, with the worker possibly still in process() touching them.
 *
 * Prefer this over make_shared/new for every heap-allocated component (the
 * COMPOSITE_REGISTER_SIMPLE macro builds with it; custom factory lambdas should too). The deleter
 * survives upcasts: a shared_ptr<component> copied/moved from this still stops through it. A
 * stack-allocated component still needs component::auto_stop as its last member (or an explicit
 * stop() early in its leaf destructor).
 */
template <typename T, typename... Args>
    requires std::derived_from<T, component>
auto make_component(Args&&... args) -> std::shared_ptr<T> {
    return std::shared_ptr<T>(new T(std::forward<Args>(args)...), [](T* ptr) {
        try {
            ptr->stop(); // leaf vtable intact: the worker and its hooks tear down fully derived
        } catch (...) {
            // The deleter runs inside ~shared_ptr (a noexcept context): a throw here would
            // std::terminate. stop() logs its own failures; destruction proceeds regardless.
        }
        delete ptr;
    });
}

} // namespace composite
