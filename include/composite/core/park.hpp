/*
 * Copyright (C) 2024-2026 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>

namespace composite {

/**
 * @brief Park-handshake coordinator: lock-free worker reads, parked writes.
 *
 * Lets an external (e.g. REST/config) writer atomically mutate a worker's data
 * members while the worker thread is quiesced ("parked"), so the worker's hot
 * path reads those members with NO lock. Correctness rests on a happens-before
 * edge that is *visible to ThreadSanitizer*: every park transition is performed
 * under @c m_run_mtx and signalled on @c m_run_cv, so the mutex/CV synchronizes-with
 * chain (which TSan models natively) orders the writer's mutation before the
 * worker's subsequent reads. A second, redundant atomic release/acquire chain on
 * @c m_park is also present.
 *
 * Two orthogonal exclusions:
 *  - worker vs writer:  the park (worker is PARKED while @c fn runs)
 *  - REST reader vs writer:  @c m_data_mtx (readers shared, writer unique)
 * The worker takes neither lock on its hot path.
 *
 * State machine (single owner; every edge a strict CAS-from-expected):
 *   NO_WORKER --worker_started--> RUNNING
 *   RUNNING   --writer--> PARK_REQUESTED --worker ack--> PARKED
 *   PARKED    --writer resume--> RESUMING --worker consume--> RUNNING
 *   any       --worker exit--> EXITING (terminal until next worker_started)
 *
 * The state machine above is the whole contract: every rule the rest of this class
 * enforces (bounded park timeout, RAII resume, inline-writer gating, in-flight drain)
 * exists to keep a writer and a worker from observing different sides of a swap.
 */
class park_coordinator {
public:
    enum class state : std::uint8_t { NO_WORKER, RUNNING, PARK_REQUESTED, PARKED, RESUMING, EXITING };

    explicit park_coordinator(std::string name = {}) : m_name(std::move(name)) {}

    park_coordinator(const park_coordinator&) = delete;
    park_coordinator& operator=(const park_coordinator&) = delete;

    /// Optional hook invoked (outside @c m_run_mtx) when a park is requested,
    /// so a blocked/looping worker can be poked to reach a park point promptly
    /// (e.g. wake() all input ports). (Invoked while a teardown guard is held.)
    auto set_poke(std::function<void()> poke) -> void { m_poke = std::move(poke); }

    /// Bounded wait before a park request (or a closed admission gate) gives up and throws.
    /// Never spins forever.
    auto set_timeout(std::chrono::milliseconds t) -> void { m_timeout = t; }

    // ------------------------------------------------------------------ worker

    /// Call once at worker-thread entry. Waits for any in-flight inline write to
    /// finish (so the worker cannot start reading data mid-write), then publishes
    /// RUNNING. The wait + transition happen under m_run_mtx, which is the same
    /// lock the inline path takes to (de)register, so start vs inline-write is
    /// strictly serialized and the happens-before to the worker's first read holds.
    auto worker_started() -> void {
        std::unique_lock lk{m_run_mtx};
        m_run_cv.wait(lk, [this] { return m_inline_writers.load(std::memory_order_acquire) == 0; });
        m_worker_id.store(std::this_thread::get_id(), std::memory_order_release);
        m_park.store(state::RUNNING, std::memory_order_release);
        m_run_cv.notify_all();
    }

    /// RAII: counts this caller into the PRE-CLOSE COHORT — the callers that were already inside
    /// with_worker_parked() when permanent closure happened, which are exactly the ones a
    /// destructor must wait for.
    ///
    /// A cohort counter, NOT entry/exit totals. Totals were wrong, and subtly so: exits are
    /// FUNGIBLE, so a caller arriving after the cut could supply the exit that satisfied it.
    /// Concretely — cut at entered=100/exited=99 with writer A still running; writer B arrives
    /// afterwards, is rejected, and pushes exited to 100; the destructor concludes "drained" and
    /// frees the coordinator underneath A. Sequential consistency does not help, because the
    /// defect is identity, not ordering: a total cannot express "these particular callers
    /// finished". So each caller now removes ITSELF from the set being waited on.
    struct entry_guard {
        park_coordinator& c;
        bool counted{false};

        explicit entry_guard(park_coordinator& coord) : c(coord) {
            // Already closed: this caller is a post-close arrival. It will be rejected at the gate,
            // and it is deliberately NOT part of the cohort — waiting for arrivals that keep
            // coming is what livelocks a destructor on a busy control plane.
            if (c.m_closing.load(std::memory_order_seq_cst)) {
                return;
            }
            c.m_pre_close_inside.fetch_add(1, std::memory_order_seq_cst);
            // Re-check: closure may have happened between the two. This is a Dekker pairing and
            // needs seq_cst on BOTH sides — the caller increments then reads the flag, the closer
            // stores the flag then reads the count. In the single total order S at least one of
            // them observes the other, so no caller can slip through uncounted while the closer
            // simultaneously concludes the cohort is empty.
            if (c.m_closing.load(std::memory_order_seq_cst)) {
                c.m_pre_close_inside.fetch_sub(1, std::memory_order_release);
                return; // withdraw: also a post-close arrival
            }
            counted = true;
        }
        entry_guard(const entry_guard&) = delete;
        auto operator=(const entry_guard&) -> entry_guard& = delete;
        entry_guard(entry_guard&&) = delete;
        auto operator=(entry_guard&&) -> entry_guard& = delete;
        ~entry_guard() {
            if (counted) {
                // RELEASE, so a destructor that observes the cohort empty also observes everything
                // every member of it did.
                c.m_pre_close_inside.fetch_sub(1, std::memory_order_release);
            }
        }
    };

    /// RAII guard a worker constructs at thread entry; publishes EXITING on every
    /// exit path so a waiting writer is released instead of hanging.
    struct exit_guard {
        park_coordinator& c;
        explicit exit_guard(park_coordinator& c) : c(c) {}
        exit_guard(const exit_guard&) = delete;
        exit_guard& operator=(const exit_guard&) = delete;
        ~exit_guard() {
            std::scoped_lock lk{c.m_run_mtx};
            c.m_park.store(state::EXITING, std::memory_order_release);
            // Forget which thread was the worker. The OS reuses thread ids, so a stale value
            // would let an unrelated thread be mistaken for the worker later and take a bypass
            // it has no right to (notably the admission gate's).
            c.m_worker_id.store(std::thread::id{}, std::memory_order_release);
            c.m_run_cv.notify_all();
        }
    };

    /// Cheap per-iteration probe: is a park pending? (one acquire load)
    [[nodiscard]] auto park_requested() const -> bool {
        return m_park.load(std::memory_order_acquire) == state::PARK_REQUESTED;
    }

    /// Call at the top of each worker loop iteration. If a park is pending, ack
    /// (PARK_REQUESTED->PARKED, release), wait for resume, consume
    /// (RESUMING->RUNNING, acquire — the publication edge), then return.
    auto park_point() -> void {
        if (m_park.load(std::memory_order_acquire) != state::PARK_REQUESTED) {
            return; // fast path: nothing pending
        }
        std::unique_lock lk{m_run_mtx};
        auto expected = state::PARK_REQUESTED;
        if (!m_park.compare_exchange_strong(expected, state::PARKED, std::memory_order_release,
                                            std::memory_order_acquire)) {
            return; // raced to EXITING (or already serviced) — nothing to do
        }
        m_run_cv.notify_all(); // tell the writer we have parked
        m_run_cv.wait(lk, [this] {
            auto s = m_park.load(std::memory_order_acquire);
            return s == state::RESUMING || s == state::EXITING;
        });
        auto resuming = state::RESUMING;
        // Consume RESUMING->RUNNING (acquire): synchronizes-with the writer's
        // release of the mutation. If EXITING, the CAS fails and we fall through.
        m_park.compare_exchange_strong(resuming, state::RUNNING, std::memory_order_acquire, std::memory_order_relaxed);
        m_run_cv.notify_all(); // let a second waiting writer observe RUNNING
    }

    // ------------------------------------------------------- doorbell wake
    //
    // Wake an idle worker the instant data arrives, instead of waiting out its NOOP
    // backoff. The consumer worker, on a NOOP, arms the doorbell and re-checks its inputs
    // (a Dekker handshake driven from component::thread_func), then sleeps in
    // wait_for_data(). A producer, on the empty->non-empty edge of an input ring, calls
    // signal_data(): it reuses this same m_run_mtx/m_run_cv (no second futex), and a
    // seq_cst fence pairs with the worker's arm-store + fence so the common
    // single-add-vs-concurrent-arm case is never missed.
    //
    // The doorbell is a LATENCY optimization, not the liveness guarantee: wait_for_data()
    // always carries the m_delay timeout (the worst-case wake latency and the true
    // backstop). A rare multi-packet burst that a lagging consumer later reads as empty via
    // a coherence-stale ring index can miss the fast wake and fall back to that timeout —
    // the same bound the pre-doorbell backoff always had, never a hang. Callers should keep
    // m_delay modest and treat it as the wake-latency ceiling.

    /// Consumer: announce intent to sleep on data. seq_cst so it pairs with the
    /// producer's fenced armed-load (and with the consumer's own re-check fence).
    auto arm_doorbell() -> void { m_doorbell_armed.store(true, std::memory_order_seq_cst); }

    /// Consumer: cancel the announcement (it woke, or found data in the re-check).
    auto disarm_doorbell() -> void { m_doorbell_armed.store(false, std::memory_order_relaxed); }

    /// Producer: called on an input's empty->non-empty edge. The seq_cst fence orders
    /// the producer's prior ring tail-store before this armed-load, so it pairs with the
    /// consumer's arm-store + fence: either the producer observes the arm (and notifies)
    /// or the consumer observes the packet in its re-check (and skips the sleep). Bails
    /// lock-free when no consumer is armed (the common case).
    ///
    /// Cost note: the caller gates this on a fresh empty->non-empty edge, so on a
    /// SATURATED stream (ring stays non-empty) this is never reached. But on a
    /// consumer-keeps-up stream where the ring oscillates 0<->1, the edge — and hence
    /// this seq_cst fence — is hit per packet; that regime is latency- not
    /// throughput-bound (the consumer idles between packets), so the fence is affordable
    /// exactly where it is paid. notify_all (not notify_one): writers can also wait on
    /// m_run_cv, so notify_one could hand the wake to a writer and leave the worker
    /// asleep until the timeout — notify_all guarantees the worker re-checks.
    auto signal_data() -> void {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (!m_doorbell_armed.load(std::memory_order_seq_cst)) {
            return;
        }
        {
            std::scoped_lock lk{m_run_mtx};
            m_data_pending = true;
        }
        m_run_cv.notify_all();
    }

    /// Consumer: sleep until a producer signals data, a park is requested, the worker's
    /// stop is requested, or the timeout @p d fires. The timeout is the fallback that
    /// keeps source components (no producer rings the doorbell) polling at the NOOP
    /// cadence. @p token makes the wait stop-aware so a plain stop()/teardown wakes the
    /// worker immediately instead of waiting out @p d (cancel_waiters() notifies
    /// m_run_cv under m_run_mtx on stop, so the re-checked predicate sees the request —
    /// no lost wakeup). Clears the data-pending flag on wake.
    auto wait_for_data(std::chrono::nanoseconds d, std::stop_token token) -> void {
        std::unique_lock lk{m_run_mtx};
        m_run_cv.wait_for(lk, d, [this, &token] {
            return m_data_pending || token.stop_requested() ||
                   m_park.load(std::memory_order_acquire) == state::PARK_REQUESTED;
        });
        m_data_pending = false;
    }

    /// Consumer: sleep out a backoff of @p d, waking early ONLY for a park request or stop —
    /// never for data. The error-restart backoff must not be shortened by a data signal: the
    /// doorbell is not armed on the backoff path, but a signal can land in the window between
    /// a producer's armed-load and the worker's unconditional disarm, and that stale
    /// m_data_pending would satisfy wait_for_data() immediately — truncating the backoff the
    /// caller is deliberately serving. The stale flag is consumed here (under the lock) so it
    /// also cannot leak a spurious wake into the next wait_for_data().
    auto wait_backoff(std::chrono::nanoseconds d, std::stop_token token) -> void {
        std::unique_lock lk{m_run_mtx};
        m_data_pending = false; // the data this signalled was already attempted (and threw)
        m_run_cv.wait_for(lk, d, [this, &token] {
            return token.stop_requested() || m_park.load(std::memory_order_acquire) == state::PARK_REQUESTED;
        });
    }

    // ------------------------------------------------------------------ writer

    /**
     * @brief Run @c fn with the worker quiesced and the data write-lock held.
     *
     * @c fn may mutate the worker's data members in place (or swap them). On
     * return the mutation is published to the worker via the resume edge.
     * Throws std::runtime_error if the worker fails to park within the timeout (the request is
     * rolled back first, so the worker keeps running), or if the component is being torn down and
     * admission does not reopen within the timeout. Both are BOUNDED by set_timeout().
     */
    template <typename Fn>
    auto with_worker_parked(Fn&& fn) -> void {
        const auto self = std::this_thread::get_id();
        // ENTRY COUNT, taken before ANY other member of this coordinator is touched.
        //
        // m_park_calls_in_flight only starts counting once a writer has been ADMITTED, so a writer
        // that has entered this function but is still between the checks below and the gate — on
        // its way to the mutex, blocked on it, or asleep on the CV — is invisible to it. A
        // destructor draining only that count sees zero, returns, and frees the mutex the writer
        // is about to lock.
        //
        // Counting from the very top closes that: every caller already INSIDE this function is
        // accounted for, whatever it happens to be blocked on. (A call that starts after
        // destruction has begun is not, and cannot be — that is the caller's lifetime problem,
        // solved by holding the component alive, not by anything this class can do.)
        //
        // Drained ONLY by close_admission_permanently_for(). An ordinary stop must never wait on
        // this: it holds the gate shut while draining, and a caller blocked on that gate can only
        // leave once the gate reopens, so a stop that waited here would deadlock itself.
        const entry_guard entered{*this};
        // ADMISSION GATE. Draining in-flight writers is not enough on its own: the count
        // reaching zero says nothing about the NEXT writer, which can enter immediately after
        // and race a teardown that has already decided it is alone (worker_resources_down()
        // runs the user's on_worker_stop() while such a writer holds the data lock). A stopping
        // thread closes admission FIRST, then drains, so "drained" actually means "and no more
        // are coming".
        //
        // The admission check and the in-flight REGISTRATION must be one atomic step. Testing
        // the flag and then incrementing leaves a window in which close_admission() + a drain
        // can both complete between the two — the writer would be uncounted, unblocked, and
        // running straight into the teardown. Doing both under m_admit_mtx (which
        // close_admission() also takes) leaves only two possibilities for every external writer:
        // it is counted BEFORE the close, so the drain waits for it; or it arrives after and
        // parks on the CV until teardown re-opens the door.
        //
        // Reentrant (park-owner) and worker-originated calls bypass the gate deliberately: they
        // are already inside the machinery being torn down, so making them queue behind it would
        // deadlock the very thread that has to make progress.
        // Three bypasses, each for a thread that would otherwise deadlock behind a gate it is
        // itself inside of:
        //  - the park owner (a reentrant write from within a parked section);
        //  - the LIVE worker (its own self-write). Pair the id with the park state: m_worker_id
        //    is cleared on exit, but the state check means a recycled thread id can never be
        //    mistaken for the worker even for an instant;
        //  - the thread that CLOSED the gate. stop() closes admission and then calls user hooks
        //    (on_park_requested, on_worker_stop); if one of those writes a property, the stopping
        //    thread would otherwise wait forever for a gate only it can reopen.
        const auto park_state = m_park.load(std::memory_order_acquire);
        const bool is_live_worker = m_worker_id.load(std::memory_order_acquire) == self &&
                                    park_state != state::NO_WORKER && park_state != state::EXITING;
        const bool external = m_park_owner.load(std::memory_order_acquire) != self && !is_live_worker &&
                              m_admission_owner.load(std::memory_order_acquire) != self;
        if (external) {
            std::unique_lock admit_lk{m_admit_mtx};
            // COUNTED WHILE WAITING, separately from m_park_calls_in_flight. A writer parked on
            // this CV has not incremented the in-flight count yet, so a teardown draining only
            // that count sees zero and proceeds — and the waiter then wakes up inside an
            // already-destroyed mutex and condition variable. That waiter is covered by the entry
            // ticket taken at the top of this function, which is why there is no separate
            // CV-waiter count: a writer asleep here has entered and not yet exited, so a teardown
            // draining tickets is already waiting for it.
            //
            // Deliberately NOT folded into m_park_calls_in_flight: an ordinary stop drains that
            // count while HOLDING the gate closed, and a waiter blocked on the gate can only
            // leave once the gate reopens. Counting it there would deadlock every stop.
            //
            // BOUNDED, like the park attempt it sits in front of. The gate is held across
            // component::stop_locked(), whose worker join is deliberately unbounded, so an
            // untimed wait here turns every property write into an unbounded one: a component
            // whose process() has wedged would pin an httplib worker per PATCH until the whole
            // control plane — including the bounded POST /app/stop that exists to survive
            // exactly this — is dead. A caller would rather be told than hang.
            const bool admitted = m_admit_cv.wait_for(admit_lk, m_timeout, [this] {
                return !m_admission_closed.load(std::memory_order_acquire) || m_admission_permanent;
            });
            const bool destroying = m_admission_permanent;
            if (!admitted || destroying) {
                // admit_lk unlocks as this unwinds, and ~entry_guard then stamps the exit ticket
                // and notifies — so a teardown cannot return, and cannot free this mutex, until
                // after that unlock has happened.
                throw std::runtime_error(destroying ? "property write rejected: component is being destroyed"
                                                    : "property write rejected: component teardown in progress");
            }
            m_park_calls_in_flight.fetch_add(1, std::memory_order_acq_rel); // counted under the gate
        } else {
            m_park_calls_in_flight.fetch_add(1, std::memory_order_acq_rel);
        }
        const in_flight_guard flight{*this, in_flight_guard::adopt};

        // Reentrancy: already the park owner on this thread — the worker is
        // parked and this thread already holds m_data_mtx. Just run fn.
        if (m_park_owner.load(std::memory_order_acquire) == self) {
            fn();
            return;
        }

        // Worker-originated write (process() or an on_change reaction setting its
        // own property): the worker cannot park itself — requesting a park and then
        // waiting for itself to reach a park point would deadlock until the timeout.
        // The worker is the *sole* reader of its data members, so it need not park;
        // but it MUST take the data write lock to exclude REST readers, and publish
        // itself as owner so a nested write re-enters the fast path above. Only when
        // a worker is actually live (not NO_WORKER/EXITING — guards a recycled
        // thread::id during teardown, which falls through to the inline path).
        if (self == m_worker_id.load(std::memory_order_acquire)) {
            const auto s = m_park.load(std::memory_order_acquire);
            if (s != state::NO_WORKER && s != state::EXITING) {
                std::unique_lock data_lk{m_data_mtx};
                m_park_owner.store(self, std::memory_order_release);
                owner_clear clear{*this};
                fn();
                return;
            }
        }

        std::unique_lock run_lk{m_run_mtx};

        // Drive RUNNING -> PARK_REQUESTED, waiting out any in-flight cycle.
        for (;;) {
            auto s = m_park.load(std::memory_order_acquire);
            if (s == state::NO_WORKER || s == state::EXITING) {
                // No worker will reach a park point: either none is running, or it
                // has exited and run its exit_guard (so it no longer reads). Run
                // inline, gated by m_inline_writers so a (re)starting worker cannot
                // begin reading mid-write.
                return run_inline_gated(self, run_lk, std::forward<Fn>(fn));
            }
            if (s == state::RUNNING) {
                auto expected = state::RUNNING;
                if (m_park.compare_exchange_strong(expected, state::PARK_REQUESTED, std::memory_order_release,
                                                   std::memory_order_acquire)) {
                    break;
                }
                continue; // lost the CAS race; re-evaluate
            }
            // PARK_REQUESTED / PARKED / RESUMING from another writer's cycle: wait
            // until it settles back to RUNNING (or the worker exits).
            m_run_cv.wait(run_lk, [this] {
                auto t = m_park.load(std::memory_order_acquire);
                return t == state::RUNNING || t == state::NO_WORKER || t == state::EXITING;
            });
        }

        m_run_cv.notify_all();
        run_lk.unlock();
        if (m_poke) {
            m_poke();
        } // poke a blocked/looping worker [outside m_run_mtx]
        run_lk.lock();

        // Wait for the worker to ack the park (PARKED) or to have exited (EXITING/
        // NO_WORKER, published by its exit_guard — the release signal during stop()).
        const bool acked = m_run_cv.wait_for(run_lk, m_timeout, [this] {
            auto s = m_park.load(std::memory_order_acquire);
            return s == state::PARKED || s == state::EXITING || s == state::NO_WORKER;
        });
        const auto observed = m_park.load(std::memory_order_acquire);
        if (!acked) {
            // Bounded timeout: roll the request back so the worker keeps running.
            auto req = state::PARK_REQUESTED;
            m_park.compare_exchange_strong(req, state::RUNNING, std::memory_order_release, std::memory_order_relaxed);
            m_run_cv.notify_all();
            run_lk.unlock();
            throw std::runtime_error{m_name + ": worker failed to park within timeout"};
        }

        if (observed != state::PARKED) {
            // Worker exited before acking (stop() in progress); it no longer reads.
            // Run inline (gated) — there is no parked worker to publish a resume to.
            return run_inline_gated(self, run_lk, std::forward<Fn>(fn));
        }
        run_lk.unlock();

        // fn runs under the data write-lock; resume via RAII so an exception in
        // fn still resumes the worker.
        std::unique_lock data_lk{m_data_mtx};
        m_park_owner.store(self, std::memory_order_release);
        resume_guard resume{*this, observed};
        fn();
    }

    /// REST reader side: shared lock, excludes writers/swaps. Worker never calls this.
    template <typename Fn>
    auto with_reader_lock(Fn&& fn) const -> decltype(auto) {
        // If this thread is the parked writer (a property_change_handler /
        // on_change listener reading its own properties while set_properties holds
        // the park), we ALREADY hold m_data_mtx exclusively. Re-acquiring it as a
        // shared lock would self-deadlock — std::shared_mutex is not recursive — so
        // run fn directly; the data is quiescent under our exclusive hold.
        if (m_park_owner.load(std::memory_order_acquire) == std::this_thread::get_id()) {
            return fn();
        }
        std::shared_lock lk{m_data_mtx};
        return fn();
    }

    // --------------------------------------------------------------- lifecycle
    // These are invoked by component under m_lifecycle_mtx.

    /// Nudge any writer blocked in with_worker_parked to re-check state (the
    /// worker's exit_guard publishing EXITING is the actual release signal; this
    /// is a belt-and-suspenders wakeup for stop()).
    auto cancel_waiters() -> void {
        std::scoped_lock lk{m_run_mtx};
        m_run_cv.notify_all();
    }

    /// Settle to NO_WORKER after the worker has been joined.
    auto settle_stopped() -> void {
        std::scoped_lock lk{m_run_mtx};
        m_park.store(state::NO_WORKER, std::memory_order_release);
        m_data_pending = false; // clear a stale data-signal so a restarted worker does not spin one NOOP
        m_run_cv.notify_all();
    }

    /// In-flight with_worker_parked calls; stop() drains this to 0 before tearing
    /// down resources the poke() hook touches.
    [[nodiscard]] auto in_flight() const -> int { return m_park_calls_in_flight.load(std::memory_order_acquire); }

    /// Block until no with_worker_parked call is in flight. stop() calls this after
    /// the worker is joined and settled, so a subsequent teardown (e.g. port
    /// destruction) cannot race an external park call still inside its poke()/data
    /// lock. Once the worker is gone (state NO_WORKER), in-flight calls take the
    /// quick inline path and drain promptly.
    auto drain_in_flight() -> void {
        while (!drain_in_flight_for(std::chrono::hours(24))) {
        }
    }

    /// Close the door on NEW external property writes, so a subsequent drain means "no writer is
    /// inside, and none can arrive". Reentrant and worker-originated calls still pass (see
    /// with_worker_parked). Idempotent. ALWAYS pair with open_admission() via RAII — leaving it
    /// closed would hang every later property write on this component.
    auto close_admission() -> void {
        const auto self = std::this_thread::get_id();
        std::scoped_lock lk{m_admit_mtx};
        // Depth counts NESTED closes by the SAME thread. Both call sites hold the component's
        // lifecycle mutex, so two threads can never interleave closes here — which is what makes
        // a single owner slot sufficient. If that ever changes, the owner must become a set: with
        // one slot, an inner close by a different thread would overwrite the outer closer's
        // identity and strip it of the bypass it needs to finish its own teardown.
        assert(m_admission_depth == 0 || m_admission_owner.load(std::memory_order_acquire) == self);
        ++m_admission_depth;
        m_admission_owner.store(self, std::memory_order_release);
        m_admission_closed.store(true, std::memory_order_release);
    }

    /// Close admission for good. For DESTRUCTION only; IDEMPOTENT, so it is safe to call before
    /// each attempt of a reporting drain loop.
    ///
    /// The RAII gate re-opens at the end of stop(), which is correct for a stop the component
    /// survives but not for the last one: a writer that evaluated its "external" test before the
    /// gate closed can wake up after stop() has returned, walk through the re-opened gate, and run
    /// against members that ~component is already destroying. Nothing re-opens after this.
    ///
    /// Deliberately does NOT drain — the caller must, and must do so WITHOUT a deadline. See
    /// component::~component().
    /// IDEMPOTENT, so it is safe to call once per iteration of a reporting loop.
    ///
    /// @return true once NO caller is left inside with_worker_parked(); false if @p timeout
    ///         elapsed first (the closure itself has still taken effect).
    [[nodiscard]] auto close_admission_permanently_for(std::chrono::nanoseconds timeout) -> bool {
        std::unique_lock lk{m_admit_mtx};
        if (!m_admission_permanent) { // idempotent: never double-count the depth
            m_admission_permanent = true;
            // SEQ_CST, pairing with the entry guard's re-check. Published BEFORE the cohort wait
            // below, so a caller that is not yet counted must observe it and withdraw.
            m_closing.store(true, std::memory_order_seq_cst);
            ++m_admission_depth;
            m_admission_owner.store(std::this_thread::get_id(), std::memory_order_release);
            m_admission_closed.store(true, std::memory_order_release);
        }
        // Wake every queued writer NOW rather than leaving it to time out. Their predicate is
        // satisfied by m_admission_permanent, so each returns immediately and throws instead of
        // sitting on the CV for the rest of the park timeout.
        m_admit_cv.notify_all();

        // Wait for the PRE-CLOSE COHORT to empty. m_closing was published above, so from here on
        // every arriving caller withdraws itself from this counter — which is what makes the wait
        // converge even while a busy control plane keeps issuing property writes. Callers that
        // arrived BEFORE the close stay counted until they actually return, so this cannot be
        // satisfied by somebody else's exit.
        //
        // A call that STARTS after destruction has begun is still the caller's lifetime problem
        // (hold the component alive); nothing inside an object being destroyed can fix that.
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        // SEQ_CST, not acquire. This load is one half of the Dekker pairing with the entry guard,
        // and a Dekker argument only holds if BOTH sides participate in the single total order S.
        // With an acquire load this execution stayed legal on a weak memory model: the writer sees
        // m_closing false, does its seq_cst increment, the closer does its seq_cst store to
        // m_closing, the closer's acquire load still reads the stale zero and returns, and the
        // writer's second m_closing load — ordered before the store — also reads false. Both sides
        // conclude the other is absent, and the coordinator is freed under a live writer.
        // x86 would almost never show it; this is what the arm64 job exists for.
        while (m_pre_close_inside.load(std::memory_order_seq_cst) != 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            // min(1ms, remaining): a generic duration API should not overshoot a budget smaller
            // than its own poll interval. Destruction uses multi-second intervals, so this only
            // matters to a caller passing a sub-millisecond timeout — but that caller is entitled
            // to have it honoured.
            const auto remaining = deadline - std::chrono::steady_clock::now();
            const auto slice = remaining < std::chrono::nanoseconds{std::chrono::milliseconds{1}}
                                   ? remaining
                                   : std::chrono::nanoseconds{std::chrono::milliseconds{1}};
            m_admit_cv.wait_for(lk, slice);
        }
        return true;
    }

    /// Re-open admission and release anyone queued behind it.
    auto open_admission() -> void {
        {
            std::scoped_lock lk{m_admit_mtx};
            if (m_admission_depth > 0 && --m_admission_depth > 0) {
                return; // an outer close is still in effect
            }
            m_admission_owner.store(std::thread::id{}, std::memory_order_release);
            m_admission_closed.store(false, std::memory_order_release);
        }
        m_admit_cv.notify_all();
    }

    /// RAII closer: guarantees admission re-opens even if teardown throws.
    class admission_gate {
    public:
        explicit admission_gate(park_coordinator& park) : m_park(&park) { m_park->close_admission(); }
        admission_gate(const admission_gate&) = delete;
        auto operator=(const admission_gate&) -> admission_gate& = delete;
        admission_gate(admission_gate&&) = delete;
        auto operator=(admission_gate&&) -> admission_gate& = delete;
        ~admission_gate() { m_park->open_admission(); }

    private:
        park_coordinator* m_park;
    };

    /// Bounded drain_in_flight(): waits up to @p timeout for the in-flight count to reach 0.
    /// @return true if it drained, false if @p timeout elapsed first.
    ///
    /// An in-flight call is only long-lived if the USER code inside it (a config on_apply
    /// reaction, a property_change_handler) is slow or wedged, so an unbounded wait here can
    /// in principle never finish. Callers bound it and REPORT rather than waiting mutely --
    /// see component::stop_locked().
    ///
    /// Backs off to a sleep after a short spin: the common case drains within a few yields,
    /// but a wedged shutdown would otherwise peg a core for as long as the process lives.
    [[nodiscard]] auto drain_in_flight_for(std::chrono::nanoseconds timeout) -> bool {
        constexpr int k_spins_before_sleep = 64;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        int spins = 0;
        while (m_park_calls_in_flight.load(std::memory_order_acquire) != 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            if (++spins < k_spins_before_sleep) {
                std::this_thread::yield();
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        return true;
    }

    [[nodiscard]] auto current_state() const -> state { return m_park.load(std::memory_order_acquire); }

    /// True if a worker thread exists that will reach a loop point (i.e. NOT
    /// NO_WORKER / EXITING). Used to decide whether a deferred config reaction will
    /// be drained by the worker at loop-top, or must be run inline by the writer.
    /// True when called ON the component's live worker thread. Same test the writer path uses to
    /// classify a self-write: the id alone is not enough, because a recycled thread id could match
    /// a worker that has already exited.
    [[nodiscard]] auto on_worker_thread() const -> bool {
        const auto st = m_park.load(std::memory_order_acquire);
        return m_worker_id.load(std::memory_order_acquire) == std::this_thread::get_id() && st != state::NO_WORKER &&
               st != state::EXITING;
    }

    [[nodiscard]] auto has_worker() const -> bool {
        const auto s = m_park.load(std::memory_order_acquire);
        return s != state::NO_WORKER && s != state::EXITING;
    }

    /// True if the CURRENT thread is the park owner — i.e. it is inside its own with_worker_parked()
    /// and holds m_data_mtx. Lets a reentrant stop() (a lifecycle-touching config on_apply run during
    /// the inline reaction drain) skip drain_in_flight(): waiting there would spin forever on the
    /// caller's OWN in-flight guard, and external park calls are already excluded because this thread
    /// holds the data lock.
    [[nodiscard]] auto owned_by_current_thread() const -> bool {
        return m_park_owner.load(std::memory_order_acquire) == std::this_thread::get_id();
    }

private:
    // Register as an inline writer under the held m_run_mtx (so worker_started
    // cannot race ahead), release the lock, then run fn inline.
    template <typename Fn>
    auto run_inline_gated(std::thread::id self, std::unique_lock<std::mutex>& run_lk, Fn&& fn) -> void {
        m_inline_writers.fetch_add(1, std::memory_order_acq_rel);
        run_lk.unlock();
        run_inline(self, std::forward<Fn>(fn)); // deregisters via RAII
    }

    // Caller has incremented m_inline_writers (under m_run_mtx). This runs fn under
    // the data write-lock and deregisters via RAII — so the count is decremented
    // even if fn throws, and the data lock is released before the decrement.
    template <typename Fn>
    auto run_inline(std::thread::id self, Fn&& fn) -> void {
        inline_dereg dereg{*this};
        std::unique_lock data_lk{m_data_mtx};
        m_park_owner.store(self, std::memory_order_release);
        owner_clear clear{*this};
        fn();
    }

    struct inline_dereg {
        park_coordinator& c;
        ~inline_dereg() {
            std::scoped_lock lk{c.m_run_mtx};
            c.m_inline_writers.fetch_sub(1, std::memory_order_acq_rel);
            c.m_run_cv.notify_all(); // let a waiting worker_started() proceed
        }
    };

    std::atomic<bool> m_admission_closed{false};      ///< teardown in progress: hold new external writers
    std::atomic<std::thread::id> m_admission_owner{}; ///< thread that closed it (bypasses its own gate)
    int m_admission_depth{0};                         ///< nesting count; guarded by m_admit_mtx
    std::mutex m_admit_mtx;                           ///< guards the admission flag + its CV
    std::condition_variable m_admit_cv;               ///< released by open_admission()
    bool m_admission_permanent{false};                ///< set by close_admission_permanently()
    /// Permanent closure is in effect. Read at the top of with_worker_parked() so an arriving
    /// caller can exclude itself from the cohort below.
    std::atomic<bool> m_closing{false};
    /// Callers inside with_worker_parked() that entered BEFORE permanent closure. Counted from the
    /// very top of the function, so it also covers a caller that has not reached the gate mutex
    /// yet — invisible to both the in-flight count and any CV-waiter count.
    std::atomic<int> m_pre_close_inside{0};

    struct in_flight_guard {
        /// Tag: the caller has ALREADY incremented the in-flight count (it had to, to make the
        /// admission check and the registration atomic — see with_worker_parked). The guard then
        /// owns only the matching decrement.
        struct adopt_t {};
        static constexpr adopt_t adopt{};

        park_coordinator& c;
        explicit in_flight_guard(park_coordinator& c) : c(c) {
            c.m_park_calls_in_flight.fetch_add(1, std::memory_order_acq_rel);
        }
        in_flight_guard(park_coordinator& c, adopt_t) : c(c) {}
        in_flight_guard(const in_flight_guard&) = delete;
        in_flight_guard& operator=(const in_flight_guard&) = delete;
        ~in_flight_guard() { c.m_park_calls_in_flight.fetch_sub(1, std::memory_order_acq_rel); }
    };

    struct owner_clear {
        park_coordinator& c;
        ~owner_clear() { c.m_park_owner.store(std::thread::id{}, std::memory_order_release); }
    };

    struct resume_guard {
        park_coordinator& c;
        state observed;
        ~resume_guard() {
            {
                std::scoped_lock lk{c.m_run_mtx};
                if (observed == state::PARKED) {
                    auto parked = state::PARKED;
                    // Publish the mutation: PARKED->RESUMING (release). Only if we
                    // actually parked the worker (else it's gone — don't orphan a token).
                    c.m_park.compare_exchange_strong(parked, state::RESUMING, std::memory_order_release,
                                                     std::memory_order_relaxed);
                    c.m_run_cv.notify_all();
                }
            }
            c.m_park_owner.store(std::thread::id{}, std::memory_order_release);
        }
    };

    std::string m_name;
    std::atomic<state> m_park{state::NO_WORKER};
    std::mutex m_run_mtx; ///< guards every park transition + CV
    std::condition_variable m_run_cv;
    std::atomic<bool> m_doorbell_armed{false}; ///< consumer is sleeping & wants a data-arrival wake
    bool m_data_pending{false};                ///< producer signalled data (guarded by m_run_mtx)
    mutable std::shared_mutex m_data_mtx;      ///< REST reader/writer exclusion
    std::atomic<std::thread::id> m_worker_id{};
    std::atomic<std::thread::id> m_park_owner{}; ///< reentrancy key
    std::atomic<int> m_park_calls_in_flight{0};  ///< park calls in flight; see drain_in_flight()
    std::atomic<int> m_inline_writers{0};        ///< gates worker_started vs inline writes
    std::chrono::milliseconds m_timeout{std::chrono::seconds{5}};
    std::function<void()> m_poke;
};

} // namespace composite
