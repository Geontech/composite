/*
 * Copyright (C) 2024-2026 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "component.hpp"
#include "lifecycle.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace composite {

/**
 * @brief Manages a collection of components and their lifecycle.
 *
 * The `application` owns the component registry and drives initialize/start/stop
 * across it. The registry is guarded by a `std::shared_mutex`: readers (the REST
 * control plane's GET routes, lifecycle iteration) take a shared lock; structural
 * mutation (add/clear) takes the unique lock. `add_component` rejects a duplicate
 * id atomically under that lock, so the REST "does it already exist?" check and
 * the insert cannot race (no check-then-add TOCTOU).
 *
 * Lifecycle calls (initialize/start/stop) snapshot the registry under the shared
 * lock and then operate on the snapshot with NO registry lock held — component
 * start()/stop() take the component's own lifecycle lock and may spawn/join the
 * worker thread, so holding the registry lock across them would needlessly
 * serialize the whole control plane (and risk lock-order inversions). The
 * snapshot's shared_ptrs keep every component alive for the duration even if a
 * concurrent clear() removes them from the registry.
 */
class application : public lifecycle {
public:
    /// Alias for a shared pointer to a component
    using component_ptr = std::shared_ptr<component>;

    /**
     * @brief Constructs an application with a given name.
     * @param name The name of the application.
     */
    explicit application(std::string_view name) : m_name(name) {}

    /**
     * @brief Gets the name of the application.
     * @return A constant reference to the application's name.
     */
    auto name() const noexcept -> const std::string& { return m_name; }

    /**
     * @brief Initializes all components managed by the application.
     * @see lifecycle::initialize()
     */
    auto initialize() -> void override {
        for (auto& component : snapshot()) {
            component->initialize();
        }
    }

    /**
     * @brief Starts all enabled components managed by the application.
     *
     * Each component is RECONCILED toward its desired `enabled` spec
     * (apply_lifecycle_changes), rather than a check-then-start. Reconcile reads the
     * desired flag and the worker state together under the component's lifecycle lock,
     * so a concurrent RUNTIME enabled=false write cannot leave a component running
     * against an explicit disable (the check-then-act TOCTOU it would otherwise have).
     * @see lifecycle::start()
     */
    auto start() -> void override {
        // Per-component isolation: one component failing to come up (e.g. a worker pool hitting
        // thread exhaustion) must NOT abort the reconciliation loop and silently leave every
        // later-iterated component un-started. Start them all, collect failures, and surface the
        // aggregate afterward so the REST control plane reports WHICH components failed rather than a
        // single opaque 500 that also masks the ones that did start.
        const auto components = snapshot();
        // PASS 1 — establish the DISABLED input gates before any producer starts. Reconciling in
        // one snapshot-order pass let an enabled producer that precedes a disabled consumer start
        // sending before the consumer's inputs were paused: the consumer's ring retained the
        // EARLIEST packets (drop-on-full keeps the oldest), and a later enable consumed that
        // stale backlog first. Gating disabled components up front closes the ordering window
        // regardless of declaration order. Failures here are deliberately not collected: pass 2
        // re-reconciles every component and reports through the existing containment, so a
        // pass-1 failure is retried once and reported once. (A concurrent RUNTIME `enabled`
        // write is itself an immediate lifecycle action — startup does not serialize operator
        // writes into a graph-wide transaction.)
        for (auto& component : components) {
            if (!component->is_enabled()) {
                try {
                    component->apply_lifecycle_changes(); // pauses inputs; no worker to start
                } catch (...) { // NOLINT(bugprone-empty-catch) — pass 2 retries and reports
                }
            }
        }
        // PASS 2 — full reconcile in snapshot order (idempotent: pass-1-gated components no-op).
        std::vector<std::string> failures;
        for (auto& component : components) {
            try {
                component->apply_lifecycle_changes(); // starts iff desired-enabled, atomically
            } catch (const std::exception& e) {
                failures.emplace_back(component->id() + ": " + e.what());
            } catch (...) {
                failures.emplace_back(component->id() + ": unknown exception");
            }
        }
        if (!failures.empty()) {
            std::string msg = "component(s) failed to start:";
            for (const auto& f : failures) {
                msg += " [" + f + "]";
            }
            throw std::runtime_error(msg);
        }
    }

    /**
     * @brief Stops all components managed by the application.
     * @see lifecycle::stop()
     */
    auto stop() -> void override {
        for (auto& component : snapshot()) {
            // Best-effort: one component's stop() throwing (e.g. a staged config<T> on_apply that runs
            // during its reaction-drain) must not abort the shutdown of the rest. The component logs
            // its own error; here we continue.
            try {
                component->stop();
            } catch (...) { // NOLINT(bugprone-empty-catch) — shutdown is best-effort per component
            }
        }
    }

    /**
     * @brief Bounded stop(), for callers that must not hang — the REST control plane above all.
     * @param timeout Overall budget, shared across every component (not per component).
     * @return The ids of components that did NOT stop within the budget. Empty means everything
     *         stopped. "Did not stop" is deliberately broader than "still running": a component
     *         also lands here if its lifecycle lock was unavailable, or if its worker exited but
     *         a property write was still in flight. All three carry the same obligation — it was
     *         not torn down, so do not destroy it.
     *
     * stop() waits as long as it takes, which is correct for shutdown but wrong for a request
     * handler: one component whose process() never returns makes `POST /app/stop` hang
     * forever, taking the control plane down with it regardless of whose bug it is.
     *
     * Signals every component FIRST, then collects against one shared deadline, so a wedged
     * component drains concurrently with the rest instead of serialising a timeout per
     * component (which on a large graph is mostly idle waiting). The deadline is taken before
     * signalling, so the signalling pass is inside the budget rather than on top of it.
     *
     * Carries the same caveat as component::try_stop(): the budget cannot bound the synchronous
     * user hooks it must invoke (on_park_requested / on_worker_stop). A component that blocks
     * in one of those can overrun the budget, and will be named in the log when it does.
     *
     * **Nothing is destroyed.** Components that did not stop are left un-torn-down and still
     * registered — the registry keeps them alive, which is exactly what a false try_stop()
     * requires. Report them, and retry or investigate; do NOT clear() an application with a
     * wedged component, as that drops the last reference and the destructor's join is
     * unbounded by necessity.
     */
    template <typename Rep, typename Period>
    [[nodiscard]] auto try_stop(std::chrono::duration<Rep, Period> timeout) -> std::vector<std::string> {
        // The deadline starts NOW — before signalling. Pass 1 calls each component's
        // on_park_requested() wake hook, which is user code; starting the clock after that loop
        // would put an unbounded phase outside the caller's budget.
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        auto components = snapshot();
        // Pass 1: latch the exit request everywhere, without waiting. BOUNDED by the same
        // deadline — request_stop() would take each component's lifecycle lock untimed, so one
        // component already being torn down (an unbounded join, holding that lock) would hang
        // this pass and the budget below would never be consulted at all.
        for (auto& component : components) {
            try {
                component->try_request_stop(deadline);
            } catch (...) { // NOLINT(bugprone-empty-catch) — collected as "did not stop" in pass 2
            }
        }
        std::vector<std::string> not_stopped;
        for (auto& component : components) {
            const auto remaining = deadline - std::chrono::steady_clock::now();
            const auto budget = remaining > decltype(remaining)::zero() ? remaining : decltype(remaining)::zero();
            bool stopped = false;
            try {
                stopped = component->try_stop(budget);
            } catch (...) {
                stopped = false; // a throwing stop path is reported, not propagated
            }
            if (!stopped) {
                not_stopped.push_back(component->id());
            }
        }
        return not_stopped;
    }

    /**
     * @brief Block until every component's worker has exited (self-finished or stopped).
     *
     * Waits on each managed component via component::wait_until_finished(). For a graph whose
     * sources self-terminate (e.g. a file source at EOF) and whose downstream components finish in
     * turn, this returns once the whole application is done — the natural join point for a batch or
     * file-processing run. A component that runs indefinitely blocks this until it is stopped.
     */
    auto wait_until_finished() -> void {
        for (auto& component : snapshot()) {
            component->wait_until_finished();
        }
    }

    /**
     * @brief Bounded wait_until_finished(): returns true iff ALL components exited within @p timeout
     *        (measured against a single overall deadline), false if the deadline elapsed first.
     */
    template <typename Rep, typename Period>
    auto wait_until_finished(std::chrono::duration<Rep, Period> timeout) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (auto& component : snapshot()) {
            const auto remaining = deadline - std::chrono::steady_clock::now();
            const auto wait = remaining > decltype(remaining)::zero() ? remaining : decltype(remaining)::zero();
            if (!component->wait_until_finished(wait)) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief Graceful drain-shutdown: stop the sources, let end-of-stream propagate, then hard-stop.
     *
     * Stops every SOURCE component (one with no incoming connection in the recorded topology) and
     * signals end-of-stream on its outputs, so no new data enters the graph and EOS-aware downstream
     * components reach input at_end() and FINISH in turn (component completion auto-propagates EOS, so
     * the whole chain drains in dependency order). Waits up to @p timeout for the graph to self-finish,
     * then stops anything still running. Best-effort: a component that ignores EOS is simply
     * hard-stopped at the end (its in-flight data dropped) — so this degrades gracefully to stop().
     */
    template <typename Rep, typename Period>
    auto drain_stop(std::chrono::duration<Rep, Period> timeout) -> void {
        std::size_t sources = 0;
        {
            // Hold the topology lock ONLY for the classify-then-stop-sources-then-EOS phase: a
            // concurrent connect/disconnect/remove during it would race the source classification (a
            // freshly-wired consumer falsely EOS-latched, or a true source missed and hard-stopped
            // without EOS). Once every source is stopped and EOS is latched we RELEASE it — no
            // source can produce new data, so a later connect cannot reintroduce the misclassification,
            // and holding the lock across the (possibly full-timeout) wait below would needlessly stall
            // the REST control plane's mutation surface. Lock order is topology-lock then the registry
            // m_mtx (via snapshot()); never inverted.
            auto topo = topology_lock();
            auto comps = snapshot();
            // A source has no incoming edge — no component's recorded connections target it.
            auto has_incoming = [&](const std::string& id) {
                for (const auto& c : comps) {
                    for (const auto& conn : c->connections()) {
                        if (conn.input.first == id) {
                            return true;
                        }
                    }
                }
                return false;
            };
            // Halt production at the sources FIRST (no more new data), then signal EOS downstream.
            for (const auto& c : comps) {
                if (!has_incoming(c->id())) {
                    ++sources;
                    // CONTAINED, like every other per-component loop in this class. stop() is not
                    // noexcept (park timeout, staged-reaction failure), and a throw here would
                    // abandon EOS for every remaining source and skip the wait/stop tail below —
                    // from an API documented as best-effort and degrading gracefully.
                    c->stop_contained();
                    c->send_eos();
                }
            }
        }
        if (sources == 0) {
            // No source (e.g. a fully cyclic graph): no EOS to inject and nothing will self-finish,
            // so a drain wait would just block the whole timeout. Fall straight through to stop().
            stop();
            return;
        }
        // Let the EOS-aware rest drain + self-finish (topology lock released), then hard-stop any
        // straggler. The trailing stop() is idempotent, so a connect that raced in during the wait is
        // still stopped here.
        wait_until_finished(timeout);
        stop();
    }

    /**
     * @brief Adds a component to the application, rejecting a duplicate id.
     * @param comp A shared pointer to the component to be added.
     * @return true if added; false if a component with the same id already exists
     *         (or @p comp is null). The check and insert are atomic under the
     *         registry's unique lock.
     */
    auto add_component(component_ptr comp) -> bool {
        if (comp == nullptr) {
            return false;
        }
        std::unique_lock lk{m_mtx};
        for (const auto& existing : m_components) {
            if (existing->id() == comp->id()) {
                return false;
            }
        }
        m_components.emplace_back(std::move(comp));
        return true;
    }

    /**
     * @brief Retrieves a component by its ID.
     * @param id The unique identifier of the component to retrieve.
     * @return A shared pointer to the component if found, otherwise a nullptr.
     */
    auto get_component(std::string_view id) const -> component_ptr {
        std::shared_lock lk{m_mtx};
        for (const auto& component : m_components) {
            if (component->id() == id) {
                return component;
            }
        }
        return {nullptr};
    }

    /**
     * @brief Returns a snapshot copy of the managed components.
     *
     * A copy (not a reference) so callers iterate a stable list without holding
     * the registry lock and without racing a concurrent add/clear.
     */
    auto components() const -> std::vector<component_ptr> { return snapshot(); }

    /**
     * @brief Acquire the topology lock that serializes edge mutation.
     *
     * connect / disconnect / remove_component must be mutually exclusive: otherwise a
     * concurrent connect could re-claim (or freshly create) an edge into a component that
     * remove_component is tearing down, and the producer's still-running worker would then
     * send into the freed input ring (use-after-free). The REST connection handlers hold
     * this lock while they resolve the endpoints AND perform the connect/disconnect, and
     * remove_component holds it across the whole teardown — so once a component is erased
     * from the registry, any concurrent connect re-resolves it (get_component) under the
     * lock, finds it gone, and is rejected before it can touch the dying component.
     *
     * Lock order is ALWAYS topology-lock then the registry m_mtx (remove_component, and the
     * handlers via get_component), so the two never invert.
     */
    [[nodiscard]] auto topology_lock() const -> std::unique_lock<std::mutex> {
        return std::unique_lock<std::mutex>{m_topology_mtx};
    }

    /**
     * @brief Stop and remove a single component, quiescing its connections first.
     *
     * The component is removed from the registry under the unique lock (so concurrent
     * lookups immediately miss it), then torn down SAFELY off-lock: it is stopped, every
     * peer producer feeding one of its inputs is disconnected (each under that producer's
     * worker park, so no send is in flight when the input's producer-claim is released —
     * the managed-disconnect path that parks a producer before releasing its claim), and
     * its own outputs are disconnected from
     * downstream consumers. The returned shared_ptr is the last owner; when the caller
     * drops it, `~component` runs (idempotent stop) and the deleter-owned dlopen handle
     * unmaps the library. This makes a live DELETE safe against the connection
     * layer rather than relying on "destroy a running peer mid-send".
     *
     * @param id The id of the component to remove.
     * @return The removed component (sole remaining owner), or nullptr if not found.
     * @throws std::runtime_error if an edge could not be quiesced (a connected peer's worker
     *         did not park within the timeout). The component is stopped but NOT destroyed:
     *         it is re-registered (or retained internally if its id was re-taken meanwhile),
     *         because returning it for destruction with a live edge into its rings would be a
     *         use-after-free on the peer's next send. Same posture as try_stop(): not torn
     *         down means do not destroy; retry once the peer quiesces.
     */
    auto remove_component(std::string_view id) -> component_ptr {
        // Serialize against connect/disconnect: held across the whole teardown so no edge
        // can be created into the target between its erase and its destruction. (topology
        // lock before the registry m_mtx — the invariant lock order.)
        auto topo = topology_lock();
        component_ptr target;
        std::vector<component_ptr> others;
        {
            std::unique_lock lk{m_mtx};
            for (auto it = m_components.begin(); it != m_components.end(); ++it) {
                if ((*it)->id() == id) {
                    target = *it;
                    m_components.erase(it);
                    break;
                }
            }
            if (target == nullptr) {
                return {nullptr};
            }
            others = m_components; // remaining peers (potential producers into / consumers of target)
        }
        // Stop the target so its worker is not sending/receiving during teardown.
        // CONTAINED: a failed stop (wedged worker / staged reaction) must NOT skip the
        // disconnect loops below. The caller drops the last reference on return, so an
        // early exit here would destroy the target while peer producers still hold edges
        // into its input rings — a use-after-free on the very next send. Disconnecting is
        // what makes the removal safe, so it happens unconditionally; a target that would
        // not stop is logged by stop_contained() and still fully unwired.
        target->stop_contained();
        // Disconnect every peer-producer edge feeding the target's inputs. disconnect()
        // parks the producer's worker, so the producer is not mid-send when the target's
        // input releases its producer-claim.
        // CONTAINED per edge: with_worker_parked() throws on a park timeout or a closed
        // admission gate, and an escape here unwinds past the return — destroying the target
        // (the registry entry is already erased) while the remaining peers still hold edges
        // into its rings. One unresponsive producer must not abandon the other edges; whether
        // EVERY edge came off decides below if the removal may complete at all.
        bool unwired = true;
        for (auto& producer : others) {
            for (const auto& c : producer->connections()) {
                if (c.input.first == target->id()) {
                    try {
                        producer->disconnect(c.output.second, target, c.input.second);
                    } catch (const std::exception& ex) {
                        unwired = false;
                        producer->logger()->error("remove_component('{}'): disconnecting producer '{}' failed: {}",
                                                  target->id(), producer->id(), ex.what());
                    } catch (...) {
                        unwired = false;
                        producer->logger()->error("remove_component('{}'): disconnecting producer '{}' failed",
                                                  target->id(), producer->id());
                    }
                }
            }
        }
        // Disconnect the target's own outputs from downstream consumers (trivial when the target
        // stopped; the same park timeout applies if it did not). Redundant calls per fan-out
        // output port are no-ops.
        for (const auto& c : target->connections()) {
            try {
                target->disconnect_all(c.output.second);
            } catch (const std::exception& ex) {
                unwired = false;
                target->logger()->error("remove_component('{}'): disconnecting output '{}' failed: {}", target->id(),
                                        c.output.second, ex.what());
            } catch (...) {
                unwired = false;
                target->logger()->error("remove_component('{}'): disconnecting output '{}' failed", target->id(),
                                        c.output.second);
            }
        }
        if (!unwired) {
            // The removal cannot complete: an edge is still live, so handing the last reference
            // to the caller destroys a component a running peer still points into. Put the
            // target back (the topology lock has kept new edges out the whole time; only a
            // concurrent add_component() could have re-taken the id, in which case the target
            // is retained internally so it simply cannot be destroyed). Stopped-but-registered
            // is the same posture a false try_stop() leaves: not torn down, do not destroy.
            {
                std::unique_lock lk{m_mtx};
                const bool id_free = std::none_of(m_components.begin(), m_components.end(),
                                                  [&](const component_ptr& c) { return c->id() == target->id(); });
                if (id_free) {
                    m_components.push_back(target);
                } else {
                    m_unremovable.push_back(target);
                }
            }
            throw std::runtime_error("cannot remove component '" + target->id() +
                                     "': a connected edge could not be quiesced (peer worker did not park); the "
                                     "component was stopped but was NOT destroyed — retry once the peer is stopped");
        }
        return target;
    }

    /**
     * @brief Stop all components and remove them from the application.
     *
     * The registry is emptied under the unique lock first (so concurrent lookups
     * immediately see an empty application), then each component is stopped via
     * the retained snapshot outside the lock. Safe to call multiple times.
     */
    auto clear() -> void {
        std::vector<component_ptr> removed;
        {
            std::unique_lock lk{m_mtx};
            removed.swap(m_components);
        }
        // Best-effort, same policy as stop(): one component failing to stop must not
        // abandon the cleanup of every component after it in the vector.
        for (auto& component : removed) {
            component->stop_contained();
        }
    }

private:
    /// Copy the registry under the shared lock.
    auto snapshot() const -> std::vector<component_ptr> {
        std::shared_lock lk{m_mtx};
        return m_components;
    }

    std::string m_name;                ///< The name of the application.
    mutable std::shared_mutex m_mtx;   ///< Guards m_components (readers shared, mutators unique).
    mutable std::mutex m_topology_mtx; ///< Serializes edge mutation (connect/disconnect/remove); see topology_lock().
    std::vector<component_ptr> m_components; ///< Components managed by this application.
    /// Components whose removal failed to unwire while a concurrent add re-took their id: kept
    /// alive (never destroyed while a peer holds an edge into them), invisible to lookups.
    std::vector<component_ptr> m_unremovable;

}; // class application

} // namespace composite
