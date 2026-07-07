/*
 * Copyright (C) 2025-2026 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "component.hpp"
#include "composite/ports/output_port.hpp"

#include <optional>
#include <string_view>
#include <utility>

namespace composite {

/**
 * @brief First-class base for a SOURCE — a component that produces data with no data input.
 *
 * A source has no upstream to wake it, so the plain component's get_data()→NOOP idiom doesn't fit;
 * sources previously hand-rolled a "produce, else NOOP" process() loop and had to remember to signal
 * end-of-stream themselves. `source_component` packages that pattern: derive it, implement produce(),
 * and get the loop, the doorbell-friendly idle backoff, and — the payoff — automatic EOS at the end
 * of the stream, so the whole downstream graph completes cleanly (see component completion + EOS).
 *
 * The derived class implements ONE hook:
 *   produce() -> produce_result
 * returning:
 *   - `emit(buffer, ts[, md])` — send this packet on the output port (worker returns NORMAL);
 *   - `idle()`                 — nothing to produce right now; back off on the doorbell (NOOP);
 *   - `done()`                 — end of stream: the framework sends EOS on the output and FINISHes
 *                                (finish_reason::completed), which propagates at_end() downstream.
 *
 * produce() must return promptly (like process()): a source that waits on an external event (a
 * socket, a file, a timer) should poll with a short timeout and return idle() when it has nothing,
 * NOT block — so the worker stays responsive to property writes and stop().
 *
 * Backpressure is handled FOR you (single consumer): when the downstream ring is full the base HOLDS
 * the produced packet and returns AWAIT_OUTPUT (reverse doorbell), emitting it once a slot frees —
 * so production paces to consumption instead of dropping, and you do NOT need to check can_send()
 * yourself. done() and idle() are NEVER gated by backpressure, so a source at EOF always reaches
 * end-of-stream even when the consumer is full or paused (EOS is out-of-band). NOTE: this no-drop
 * guarantee is for a SINGLE consumer. On a FAN-OUT output, can_send() is true if ANY consumer has
 * room, so a slower branch whose ring fills is still dropped (drop-on-full) while a faster branch
 * keeps up; a fan-out source that must not drop the slow branch has to pace itself (inspect
 * per-consumer capacity in produce()). Also: a source HOLDING undelivered data whose consumer never
 * drains (it self-finished, or stopped while still connected) blocks (AWAIT_OUTPUT) until the consumer
 * drains or the app is stopped — the lossless-backpressure trade-off — so use
 * wait_until_finished(timeout)/drain_stop() for graphs where a consumer may stop early. (A DISABLED
 * consumer is exempt: its port is paused to depth 0, which DISCARDS on send rather than blocking.)
 *
 * **Destruction (IMPORTANT):** a concrete source MUST stop its worker while the object is still fully
 * alive — put `component::auto_stop` as its LAST data member, OR call stop() early in its own
 * destructor. This base canNOT do it for you: produce() is pure here, so the instant a leaf's
 * destructor has run, the object's vtable degrades to source_component and a worker still in
 * process() would invoke the now-pure produce() ("pure virtual method called" -> abort). A base-class
 * MEMBER auto_stop runs too late — by the time it executes, the leaf vtable is already gone — so the
 * stop MUST live in the leaf. This is the same rule pipeline_component documents for its pool.
 *
 * @tparam OutBuf the output buffer type (immutable_buffer<T> or mutable_buffer<T>).
 */
template <typename OutBuf>
class source_component : public component {
public:
    enum class produce_status { data, idle, done };

    /// What produce() returns: a status plus (for `data`) the packet to emit.
    struct produce_result {
        produce_status status{produce_status::idle};
        OutBuf buffer{};
        timestamp ts{};
        composite::metadata_ptr md{};

        /// Emit a packet on the output port. Latch the metadata_ptr in the source and pass
        /// the same instance every packet (rebuild via composite::make_metadata on change).
        static auto emit(OutBuf b, timestamp t, composite::metadata_ptr m = nullptr)
            -> produce_result {
            return produce_result{produce_status::data, std::move(b), t, std::move(m)};
        }
        /// Nothing to produce right now — back off (NOOP).
        static auto idle() -> produce_result { return produce_result{produce_status::idle, {}, {}, {}}; }
        /// End of stream — send EOS on the output and finish (completed).
        static auto done() -> produce_result { return produce_result{produce_status::done, {}, {}, {}}; }
    };

    explicit source_component(std::string_view id, std::string_view out_name = "out")
        : component(id), m_out(out_name) {
        add_port(&m_out);
    }

protected:
    /// Produce the next output. Called repeatedly on the worker thread; must return promptly.
    virtual auto produce() -> produce_result = 0;

    /// The output port, for derived classes that need it (e.g. to check can_send()).
    [[nodiscard]] auto output() -> output_port<OutBuf>& { return m_out; }

    /// Drop any packet held from a PRIOR run so a (re)start begins cleanly. Without this, a source
    /// stopped while output-blocked (m_pending set) would, on restart, replay that stale packet
    /// (old buffer/ts/md) ahead of freshly-produced data. Runs on every start path (see component's
    /// start_locked -> on_worker_start). A derived source that also overrides on_worker_start() MUST
    /// call this base.
    auto on_worker_start() -> void override { m_pending.reset(); }

private:
    // The produce loop. `final` — a source overrides produce(), not process().
    auto process() -> retval final {
        // If we produced a packet last iteration but the output ring was full, retry sending it FIRST.
        // We HELD it (never dropped) and pulled nothing new from the source until it lands.
        if (m_pending.has_value()) {
            if (m_out.producer_is_connected() && !m_out.producer_can_send()) {
                return retval::AWAIT_OUTPUT;  // still full — wait on the reverse doorbell
            }
            m_out.send_data(std::move(m_pending->buffer), m_pending->ts, std::move(m_pending->md));
            m_pending.reset();
            return retval::NORMAL;
        }
        auto r = produce();
        switch (r.status) {
            case produce_status::data:
                // Pace against downstream backpressure (reverse doorbell): if the output ring is
                // full, HOLD this packet and AWAIT_OUTPUT instead of dropping it — and do NOT pull the
                // next item until this one lands. Backpressure gates only DATA; done()/idle below stay
                // reachable, so a source at EOF still signals end-of-stream even when the downstream is
                // full or paused. Gated on producer_is_connected() so an unconnected source (can_send
                // is false purely for lack of a consumer, not backpressure) still runs. The producer_*
                // variants are the lock-free single-producer path (this worker is the sole producer on
                // m_out), avoiding the per-iteration connection-snapshot tax.
                //
                if (m_out.producer_is_connected() && !m_out.producer_can_send()) {
                    m_pending = std::move(r);
                    return retval::AWAIT_OUTPUT;
                }
                m_out.send_data(std::move(r.buffer), r.ts, std::move(r.md));
                return retval::NORMAL;
            case produce_status::done:
                // FINISH -> finish_reason::completed -> the base auto-sends EOS on m_out, so the
                // downstream graph reaches at_end() and completes in turn. Reachable regardless of
                // backpressure — EOS is out-of-band, never gated by can_send().
                return retval::FINISH;
            case produce_status::idle:
                break;
        }
        return retval::NOOP;
    }

    output_port<OutBuf> m_out;
    std::optional<produce_result> m_pending;  // produced packet awaiting a free downstream slot (worker-thread only)
    // NOTE: no auto_stop here on purpose. A base-class member auto_stop cannot make destruction safe
    // (produce() is pure here; see the class doc's "Destruction" note) — the worker must be stopped by
    // the LEAF, while the leaf vtable is still intact.
};

} // namespace composite
