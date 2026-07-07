/*
 * Copyright (C) 2024-2026 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "composite/core/component.hpp"
#include "composite/core/metadata.hpp"
#include "composite/ports/input_port.hpp"
#include "composite/ports/output_port.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>

namespace composite {

/**
 * @brief One-in/one-out component base with an internal worker pool and a
 *        **slot ring** that re-serialises out-of-order completion back to
 *        submission order — the distillation of the ingest-thread + task_queue +
 *        ordered-future-drain pattern that fft/psd hand-roll today (ASSESSMENT §4.D).
 *
 * The derived class implements three hooks instead of process():
 *   - prepare(md)                  : main thread, ARRIVAL order (per metadata CHANGE, see below)
 *   - work(in, ts, md) -> out      : pool worker, CONCURRENT across packets
 *   - finalize(out, ts, md) -> bool: main thread, SUBMISSION order (false = drop)
 * plus optional on_workers_resized(n). Order-sensitive stages (prepare/finalize)
 * run single-threaded on the component's worker; the parallel stage (work) runs on
 * the pool. Output is emitted strictly in submission order regardless of which pool
 * worker finishes first.
 *
 * **Concurrency:** the component's single worker thread is the "main" thread — it
 * ingests (get_data, in order), submits to a free ring slot, and retires DONE
 * slots in order (finalize + send_data). N pool threads claim READY slots, run
 * work() concurrently, and publish DONE. A bounded ring gives explicit
 * backpressure (full ⇒ the main thread stops ingesting until a slot frees). The
 * main thread waits event-driven on a CV (no busy-yield) and is interrupted by the
 * park coordinator (on_park_requested) so property writes / stop quiesce promptly.
 *
 * num_workers is a RUNTIME property: a change is applied by the main worker at
 * loop-top via a drain-and-rebuild (all in-flight slots retire, then the pool is
 * stopped and restarted at the new size, and on_workers_resized() fires) — no
 * stop/start of the component is required. A work() exception is captured per slot
 * and logged on the main thread (that packet is dropped); the pipeline keeps running.
 *
 * **Destruction order (IMPORTANT):** the pool is torn down by on_worker_stop() via the base
 * start_locked()/stop_locked() path, so a normal stop()/disable stops it correctly. BUT `~component()`
 * cannot: by the time the base destructor runs, the derived vtable is already unwound, so its stop()
 * resolves on_worker_stop() to the base no-op — the pool would outlive the object. A concrete leaf
 * therefore MUST stop the worker while it is still fully alive: put `component::auto_stop` as its LAST
 * data member, OR call stop() early in its own destructor (fft does the latter because it must stop
 * before fftw_cleanup_threads()). This mirrors plain component::auto_stop, and matters even more here.
 *
 * **Park scope (IMPORTANT):** the park handshake (with_worker_parked, and hence config<T>'s loop-top
 * on_apply staging) quiesces ONLY the main worker — the POOL threads keep running work() through a
 * park. So park-protected/config<T> members are NOT safe to read from work(). A pool-based component
 * must publish config to its work() via an atomic snapshot (e.g. std::atomic<std::shared_ptr<const
 * cfg>>), as fft/psd do; do NOT assume "quiesced by park" extends to the pool.
 *
 * **Output backpressure:** retire (finalize+send) paces LOSSLESSLY against a full downstream — the
 * head DONE slot is held and the worker returns AWAIT_OUTPUT rather than dropping it, so no data is
 * lost and self-completion (inputs_at_end -> FINISH) only fires after the output has flushed. The
 * trade-off: if a connected consumer never drains (it self-finished, or stopped while still
 * connected), retire blocks until it drains or the app is stopped — use wait_until_finished(timeout)/
 * drain_stop() for graphs where a downstream may stop early. (A DISABLED consumer is exempt: its port
 * is paused to depth 0, which discards on send rather than blocking.)
 *
 * @tparam InBuf  input buffer type (e.g. immutable_buffer<cf32>)
 * @tparam OutBuf output buffer type (e.g. mutable_buffer<cf32>)
 */
template <typename InBuf, typename OutBuf>
class pipeline_component : public component {
public:
    using in_t = InBuf;
    using out_t = OutBuf;

    explicit pipeline_component(std::string_view id,
                                std::string in_name = "in",
                                std::string out_name = "out",
                                int default_workers = 2)
        : component(id), m_in(in_name), m_out(out_name), m_num_workers(default_workers) {
        add_port(m_in);
        add_port(m_out);
        add_property("num_workers", m_num_workers, properties::config_type::RUNTIME)
            .validate([](const int& n) { return n >= 1 && n <= 1024; })
            .on_change([this](const properties::json&) {
                // Flag a pool rebuild; the main worker performs it at a safe point
                // (between iterations, after draining in-flight work) — see do_resize.
                m_resize_pending.store(true, std::memory_order_release);
            });
    }

    // Manage the worker pool via the base's on_worker_start()/on_worker_stop() hooks (NOT by
    // overriding start()/stop()). The hooks run on EVERY start/stop path — the direct start()/stop()
    // AND the enabled-reconcile path that application::start() / a RUNTIME `enabled` write take (they
    // call the private start_locked()/stop_locked(), which the virtual start()/stop() would bypass).
    // Overriding start()/stop() instead would leave the pool un-started when launched via the app.
    auto on_worker_start() -> void override {
        start_pool();  // workers ready before the main (ingest/retire) worker begins
    }
    auto on_worker_stop() -> void override {
        stop_pool();   // main worker already joined by the base; now stop + join the pool
    }

protected:
    /// ARRIVAL order, main thread. Inspect/annotate the metadata that will travel with the
    /// packets. Metadata is shared across packets, so this runs only when it actually needs
    /// rebuilding: when the incoming packet carries a DIFFERENT metadata instance than the
    /// previous one, or after invalidate_prepared_metadata(). Packets in between share the
    /// prepared result — do per-CHANGE stamping here (config params, formats), never
    /// per-packet counting. Default: no-op.
    virtual auto prepare(composite::metadata& /*md*/) -> void {}

    /// The parallel stage: runs on a pool worker, concurrently across packets.
    /// Must be thread-safe w.r.t. other workers (use worker_index() for per-worker
    /// state). Returns the output buffer for this input.
    virtual auto work(in_t in, timestamp ts, const composite::metadata& md) -> out_t = 0;

    /// SUBMISSION order, main thread. Decide keep/drop for this packet (false = not sent
    /// downstream). The metadata is read-only here — it is shared across packets; annotate
    /// in prepare(). Default: keep + send.
    virtual auto finalize(out_t& /*out*/, timestamp /*ts*/, const composite::metadata& /*md*/) -> bool { return true; }

    /// Tell the pipeline that prepare() would now stamp different values (e.g. a property
    /// changed the config it reads): the cached prepared metadata is rebuilt for the next
    /// ingested packet even if the incoming metadata instance is unchanged. Callable from
    /// any thread (property on_change hooks included).
    auto invalidate_prepared_metadata() -> void {
        m_prepare_dirty.store(true, std::memory_order_release);
    }

    /// Called once on the main thread after the pool is (re)sized, to (re)build
    /// per-worker state. Default: no-op.
    virtual auto on_workers_resized(int /*n*/) -> void {}

    /// Index of the calling pool worker (0..n-1). Only valid inside work().
    [[nodiscard]] auto worker_index() const -> int { return t_worker_index; }

    auto in_port() -> input_port<in_t>& { return m_in; }
    auto out_port() -> output_port<out_t>& { return m_out; }

    // ---- the ingest / retire loop (runs on the component's single worker) ----
    auto process() -> retval final {
        using enum retval;
        { std::scoped_lock lk{m_mtx}; m_park_pending = false; }

        // 0) Apply a pending num_workers change (drains in-flight, rebuilds the pool).
        if (m_resize_pending.exchange(false, std::memory_order_acq_rel)) { do_resize(); }

        // 1) Retire DONE slots in submission order (finalize + send, main thread). Pace against a full
        //    downstream: if a finalized packet cannot be sent right now, STOP retiring and AWAIT_OUTPUT
        //    rather than dropping it (mirrors source_component §2.4). Without this a full downstream
        //    silently drops-on-full and §2.1's FINISH then reports finish_reason::completed having lost
        //    data — silent truncation on the exact batch/file use case §2.1 exists to enable.
        auto rr = retire_ready();
        if (rr.blocked_on_output) { return AWAIT_OUTPUT; }
        bool progress = rr.retired_any;

        // 2) Ingest one packet into a free slot (arrival order), if room + data. try_get() (not
        //    get_data()) so a genuine zero-length packet is submitted through work()/finalize() rather
        //    than mistaken for an empty ring and silently dropped — §2.1's FINISH would otherwise make
        //    that loss terminal (a lost EOF/flush marker reported as a clean completion).
        bool have_free{};
        { std::scoped_lock lk{m_mtx}; have_free = (m_submit - m_retire) < m_cap; }
        if (have_free) {
            if (auto pkt = m_in.try_get()) {
                auto& [in, ts, md] = *pkt;
                slot& s = m_ring[m_submit & m_mask];  // FREE; owned by main until READY published
                s.in = std::move(in);
                s.ts = ts;
                s.md = prepared_metadata(md);  // shared across packets; prepare() runs per CHANGE
                s.err = nullptr;
                s.state.store(slot::READY, std::memory_order_release);
                { std::scoped_lock lk{m_mtx}; ++m_submit; }
                m_work_cv.notify_one();
                progress = true;
            }
        }

        if (progress) { return NORMAL; }

        // 3) Nothing to do right now. If work is in flight, wait event-driven for
        //    the head slot to complete (interrupted by a park request / stop).
        std::unique_lock lk{m_mtx};
        if (m_submit > m_retire) {
            m_done_cv.wait_for(lk, std::chrono::milliseconds(20), [this] {
                return m_ring[m_retire & m_mask].state.load(std::memory_order_acquire) == slot::DONE
                       || m_park_pending || m_pool_stop.load(std::memory_order_acquire);
            });
            return NORMAL;  // re-enter to retire whatever completed
        }
        lk.unlock();
        // Truly idle: no input available AND nothing in flight (m_submit == m_retire, so no packet is
        // dropped). If every input is at end-of-stream (upstream sent EOS and we have ingested and
        // retired everything), the pipeline is complete: FINISH so the base records
        // finish_reason::completed and auto-sends EOS on m_out — a downstream stage then reaches
        // at_end() and completes in turn. Without this a pipeline_component (fft, psd — the framework's
        // flagship adopters) could NEVER self-terminate, so a batch/file graph built on it would hang
        // application::wait_until_finished() and burn the full drain_stop() timeout even after the
        // source finished.
        if (inputs_at_end()) { return FINISH; }
        return NOOP;  // idle but the stream is still open — wait for more input
    }

    /// Wake the main worker (and pool) so a property write / stop quiesces promptly
    /// even while the main worker is waiting on the done-CV.
    auto on_park_requested() -> void override {
        { std::scoped_lock lk{m_mtx}; m_park_pending = true; }
        m_done_cv.notify_all();
        m_work_cv.notify_all();
    }

private:
    struct slot {
        enum state_t : int { FREE, READY, BUSY, DONE };
        std::atomic<int> state{FREE};
        in_t in{};
        out_t out{};
        timestamp ts{};
        composite::metadata_ptr md{};  ///< shared prepared metadata; always non-null once submitted
        std::exception_ptr err{};
        // Retire-time latches (main-thread only): finalize() runs exactly once per slot even if the
        // send is deferred across an AWAIT_OUTPUT round-trip; `keep` records its decision so a DROPPED
        // packet retires immediately without waiting for output room it never needs. [fix round 4]
        bool finalized{false};
        bool keep{false};
    };

    static auto round_up_pow2(std::size_t n) -> std::size_t {
        std::size_t p = 1;
        while (p < n) { p <<= 1; }
        return p;
    }

    /// ARRIVAL order, main thread. Return the shared metadata to travel with this packet,
    /// re-running prepare() only when the incoming instance differs from the previous
    /// packet's (pointer identity — producers latch their instance) or after
    /// invalidate_prepared_metadata(). The steady state is a refcount bump: no metadata
    /// copy, no prepare() call, no allocation.
    auto prepared_metadata(const composite::metadata_ptr& in_md) -> composite::metadata_ptr {
        // exchange (not load) so an invalidate() that lands DURING prepare() re-marks dirty
        // and the next packet rebuilds again — never a lost invalidation.
        const bool dirty = m_prepare_dirty.exchange(false, std::memory_order_acq_rel);
        if (!dirty && m_prepared != nullptr && in_md == m_last_in_md) {
            return m_prepared;
        }
        auto working = in_md ? *in_md : composite::metadata{};
        try {
            prepare(working);
        } catch (...) {
            m_prepare_dirty.store(true, std::memory_order_release);  // retry on the next packet
            throw;
        }
        m_last_in_md = in_md;
        m_prepared = composite::make_metadata(std::move(working));
        return m_prepared;
    }

    struct retire_result {
        bool retired_any{false};     ///< at least one slot was finalized+sent (or an errored slot logged)
        bool blocked_on_output{false};///< head slot is DONE but the output is full — caller should AWAIT_OUTPUT
    };

    /// Retire currently-DONE slots in submission order (finalize + send). Stops early — WITHOUT
    /// dropping — if the head slot would be sent but the output ring is full, reporting
    /// blocked_on_output so the caller parks on the reverse doorbell instead of overrunning it.
    auto retire_ready() -> retire_result {
        bool any = false;
        for (;;) {
            std::size_t r{};
            {
                std::scoped_lock lk{m_mtx};
                if (m_retire >= m_submit) { break; }
                r = m_retire;
            }
            slot& s = m_ring[r & m_mask];
            if (s.state.load(std::memory_order_acquire) != slot::DONE) { break; }  // head not finished
            if (s.err) {
                try { std::rethrow_exception(s.err); }
                catch (const std::exception& e) { logger()->error("pipeline work() threw: {} (packet dropped)", e.what()); }
                catch (...) { logger()->error("pipeline work() threw unknown exception (packet dropped)"); }
                s.err = nullptr;
            } else {
                // Decide keep/drop FIRST (exactly once — latched), THEN pace only a KEEP packet
                // against a full downstream. A DROP packet (finalize returned false) sends nothing, so
                // it needs no output room and must NOT be blocked on it — otherwise a dropped head with
                // a full/wedged downstream would stall the whole pipeline forever (never reaching
                // FINISH). Backpressure on a KEEP: leave the head DONE (don't advance m_retire) and
                // AWAIT_OUTPUT; the reverse doorbell re-wakes on the full->not-full edge. The latch
                // means finalize() is not re-run across that round-trip. Single-producer on m_out:
                // nothing else fills the downstream ring between this check and the send. [fix round 4]
                if (!s.finalized) {
                    // A throwing finalize() is treated like a throwing work(): log + drop this packet.
                    // Crucially the latch is set REGARDLESS of a throw, so the packet is never
                    // re-finalized — otherwise, under error_restart_max>0, thread_func would catch the
                    // throw, restart, and re-enter retire_ready() on this same un-retired head, running
                    // finalize() (and its pre-throw side effects) again per retry. [fix round 5]
                    try {
                        s.keep = finalize(s.out, s.ts, *s.md);
                    } catch (const std::exception& e) {
                        logger()->error("pipeline finalize() threw: {} (packet dropped)", e.what());
                        s.keep = false;
                    } catch (...) {
                        logger()->error("pipeline finalize() threw unknown exception (packet dropped)");
                        s.keep = false;
                    }
                    s.finalized = true;
                }
                if (s.keep) {
                    if (m_out.producer_is_connected() && !m_out.producer_can_send()) {
                        return {any, /*blocked_on_output=*/true};
                    }
                    m_out.send_data(std::move(s.out), s.ts, s.md);  // refcount bump; slot keeps its ref until reset below
                }
            }
            s.in = in_t{};
            s.out = out_t{};
            s.md = nullptr;
            s.finalized = false;
            s.keep = false;
            s.state.store(slot::FREE, std::memory_order_release);
            { std::scoped_lock lk{m_mtx}; ++m_retire; }
            any = true;
        }
        return {any, false};
    }

    /// Apply a num_workers change on the main worker: drain all in-flight slots
    /// (so nothing is dropped), then tear down and rebuild the pool at the new
    /// size. Runs between process() iterations, never mid-packet.
    auto do_resize() -> void {
        const int n = m_num_workers < 1 ? 1 : m_num_workers;
        if (static_cast<int>(m_pool.size()) == n) { return; }  // no actual change

        // Drain: retire everything in flight without ingesting more.
        for (;;) {
            if (retire_ready().blocked_on_output) {
                // Downstream is full — draining would drop. Defer the resize (re-arm) and hand back to
                // the main loop, which returns AWAIT_OUTPUT; the reverse doorbell re-runs do_resize once
                // the output frees. Avoids a busy-spin here (the head-DONE CV predicate is already
                // satisfied, so wait_for would not block) and never drops. [fix round 3]
                m_resize_pending.store(true, std::memory_order_release);
                return;
            }
            std::unique_lock lk{m_mtx};
            if (m_submit == m_retire) { break; }  // ring fully drained
            m_done_cv.wait_for(lk, std::chrono::milliseconds(20), [this] {
                return m_ring[m_retire & m_mask].state.load(std::memory_order_acquire) == slot::DONE
                       || m_park_pending || m_pool_stop.load(std::memory_order_acquire);
            });
            if (m_pool_stop.load(std::memory_order_acquire)) { return; }  // stopping — abandon
            if (m_park_pending) {  // another park (e.g. a different write) — retry the resize later
                m_resize_pending.store(true, std::memory_order_release);
                return;
            }
        }
        stop_pool();
        start_pool();  // rebuilds the ring + spawns the new worker count + on_workers_resized(n)
        logger()->debug("pipeline '{}' resized to {} workers", id(), m_pool.size());
    }

    auto start_pool() -> void {
        const int n = m_num_workers < 1 ? 1 : m_num_workers;
        m_cap = round_up_pow2(static_cast<std::size_t>(n) * 2);  // depth = 2x workers (>= 1 slot/worker + headroom)
        m_mask = m_cap - 1;
        m_ring = std::make_unique<slot[]>(m_cap);
        m_submit = m_claim = m_retire = 0;
        m_pool_stop.store(false, std::memory_order_release);
        m_pool.clear();
        m_pool.reserve(static_cast<std::size_t>(n));
        for (int w = 0; w < n; ++w) {
            try {
                m_pool.emplace_back([this, w] { pool_worker(w); });
            } catch (const std::system_error& e) {
                // Thread-resource exhaustion partway through the spawn loop (EAGAIN). DEGRADE to the
                // workers we did get rather than failing the whole (re)size: a transient limit on a
                // RUNTIME num_workers bump — or at start — must not permanently kill the component nor
                // (in the resize path, which has no try/catch of its own) escape as an errored
                // iteration. Only a total failure to spawn even ONE worker is fatal, and is propagated
                // (start_locked()'s catch reaps it; a resize cannot proceed). The ring is already sized
                // for the request (larger than the achieved pool — harmless), worker indices stay in
                // range, and on_workers_resized() below is told the ACTUAL count.
                if (m_pool.empty()) { throw; }
                logger()->warn("pipeline '{}': spawned only {}/{} workers ({}); running degraded",
                               id(), m_pool.size(), n, e.what());
                break;
            }
        }
        // Build per-worker state for the ACHIEVED worker count (workers are still idle here —
        // m_submit==0, so none has entered work() yet, so this races nothing).
        on_workers_resized(static_cast<int>(m_pool.size()));
    }

    auto stop_pool() -> void {
        m_pool_stop.store(true, std::memory_order_release);
        m_work_cv.notify_all();
        for (auto& t : m_pool) { if (t.joinable()) { t.join(); } }
        m_pool.clear();
    }

    auto pool_worker(int widx) -> void {
        t_worker_index = widx;
        for (;;) {
            std::size_t my{};
            {
                std::unique_lock lk{m_mtx};
                m_work_cv.wait(lk, [this] {
                    return m_claim < m_submit || m_pool_stop.load(std::memory_order_acquire);
                });
                if (m_pool_stop.load(std::memory_order_acquire)) { return; }
                my = m_claim++;  // claim this READY slot (only this worker gets it)
            }
            slot& s = m_ring[my & m_mask];
            // s.state == READY here; run the parallel stage outside the lock. The slot holds
            // its own metadata reference, so a concurrent rebuild of the prepared metadata on
            // the main thread never invalidates *s.md.
            try {
                s.out = work(std::move(s.in), s.ts, *s.md);
            } catch (...) {
                s.err = std::current_exception();
            }
            s.state.store(slot::DONE, std::memory_order_release);
            m_done_cv.notify_one();  // wake the main (retire) thread
        }
    }

    input_port<in_t> m_in;
    output_port<out_t> m_out;
    int m_num_workers;

    std::unique_ptr<slot[]> m_ring;
    std::size_t m_cap{0};
    std::size_t m_mask{0};

    std::mutex m_mtx;                  ///< guards the submit/claim/retire counters + park_pending
    std::condition_variable m_work_cv; ///< pool workers wait for READY slots
    std::condition_variable m_done_cv; ///< main thread waits for the head slot to be DONE
    std::size_t m_submit{0};           ///< packets submitted (main writes)
    std::size_t m_claim{0};            ///< packets claimed by workers (workers write)
    std::size_t m_retire{0};           ///< packets retired (main writes)
    bool m_park_pending{false};        ///< set by on_park_requested to break the done-wait
    std::atomic_bool m_resize_pending{false};  ///< set by num_workers on_change; applied by the main worker
    std::atomic_bool m_pool_stop{false};

    // Prepared-metadata cache (main-thread ingest state; the dirty flag alone is cross-thread).
    composite::metadata_ptr m_last_in_md{};   ///< incoming instance the cache was built from
    composite::metadata_ptr m_prepared{};     ///< prepare()'s result, shared by packets until it changes
    std::atomic_bool m_prepare_dirty{true};   ///< set by invalidate_prepared_metadata()
    std::vector<std::thread> m_pool;

    static inline thread_local int t_worker_index{-1};

}; // class pipeline_component

} // namespace composite
