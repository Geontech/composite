/*
 * Copyright (C) 2024-2026 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "composite/buffers/buffer.hpp"
#include "composite/core/metadata.hpp"
#include "composite/core/park.hpp" // doorbell: signal the consumer worker on the empty->non-empty edge
#include "composite/core/timestamp.hpp"
#include "port_base.hpp"

#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <vector>

namespace composite {

/// Trait: is @c Buf a mutable_buffer<T>? Reports a port's mutability without two
/// separate input_port specializations.
template <typename>
struct is_mutable_buffer : std::false_type {};
template <typename T>
struct is_mutable_buffer<mutable_buffer<T>> : std::true_type {};

namespace detail {
/// Round a ring size up to a power of two, SATURATING at the largest representable one.
/// The previous loop (`std::size_t p = 1; while (p < n) p <<= 1;`) shifted p to 0 once n
/// exceeded 2^63 and then spun forever — reachable from the public depth() setter, so one
/// out-of-range value hung the process. Saturating lets the subsequent allocation fail
/// loudly (std::length_error / std::bad_alloc) instead, which the caller can report.
inline auto round_up_pow2(std::size_t n) -> std::size_t {
    constexpr std::size_t k_max_pow2 = std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1);
    if (n <= 1) {
        return 1;
    }
    return n > k_max_pow2 ? k_max_pow2 : std::bit_ceil(n);
}
} // namespace detail

/**
 * @brief Input port: a bounded **lock-free single-producer/single-consumer ring**
 *        of (buffer, timestamp, metadata) packets.
 * @tparam Buf Buffer type — either @c immutable_buffer<T> or @c mutable_buffer<T>.
 *
 * **Concurrency model (the precondition the lock-free ring rests on):**
 *  - **One consumer:** only the owning component's single worker thread calls
 *    get_data(). (REST introspection reads stats, never the ring.)
 *  - **One producer:** an input may be fed by at most one output port. This is
 *    enforced at connect time (output_port_base::connect claims the input;
 *    fan-in is rejected), so the single-producer invariant holds by construction.
 *
 * The ring uses two monotonically increasing indices: the producer owns @c m_tail,
 * the consumer owns @c m_head, on separate cache lines. Publication is a
 * release/acquire pair on each index — see add_data()/pop() for the ordering
 * argument. Capacity is a power of two fixed at construction (grown only while
 * empty, at setup); @c depth() is the runtime soft limit (0 ⇒ drop all, i.e.
 * paused). When the ring is full the packet is dropped at the producer and the
 * overflow callback fires — real bounded backpressure.
 */
template <typename Buf>
class input_port : public input_port_base {
public:
    using buffer_type = Buf;
    using value_type = typename Buf::value_type;
    // Metadata rides as a shared_ptr<const metadata> (composite::metadata_ptr): packets between
    // metadata changes share ONE instance, so enqueue/dequeue is a refcount bump, never a map copy.
    using queue_type = std::tuple<buffer_type, timestamp, composite::metadata_ptr>;

    /// @param name Port name. @param depth Initial queue depth (also sizes the ring).
    explicit input_port(std::string_view name, std::size_t depth = 1024) : input_port_base(name) {
        this->depth(depth); // sizes the ring (empty) + sets the soft limit
    }

    ~input_port() override = default;

    auto element_type() const -> std::type_index override { return std::type_index(typeid(value_type)); }
    auto element_type_id() const -> std::size_t override { return typeid(value_type).hash_code(); }
    auto is_mutable() const -> bool override { return is_mutable_buffer<Buf>::value; }

    /// Set the queue depth (soft limit). Grows the PHYSICAL ring to hold @p value only
    /// while this input is UNCLAIMED and empty (setup-time: before connect(), or after an
    /// explicit disconnect); a change on a connected input (e.g. pause via depth(0)) only
    /// moves the soft limit and never reallocates a live ring.
    ///
    /// The claim — not an empty observation — is what makes the replacement safe. A
    /// connected producer can sit between its capacity check and its slot write with the
    /// ring momentarily empty, so `head == tail` does NOT exclude a producer; replacing
    /// m_ring under it frees the storage it is about to write. The check and the
    /// replacement are performed under m_resize_mtx, which claim_producer() also takes, so
    /// a concurrent connect() cannot slip in between them; a release can only make the
    /// check conservative (declining to grow), never unsafe. The additional empty check
    /// keeps a grow from silently discarding packets still queued on a disconnected input.
    ///
    /// NOTE: this makes physical growth a SETUP-time operation by construction. It is not a
    /// live-resize primitive: growing the ring of an input whose producer was disconnected
    /// by a RAW port-level disconnect (rather than the component-level managed disconnect,
    /// which parks the producer's worker first) is still the caller's responsibility to
    /// sequence. v0.5 does not support live physical resizing at all.
    auto depth(std::size_t value) -> void override {
        const std::size_t want = detail::round_up_pow2(value == 0 ? 1 : value);
        {
            const auto lock = std::scoped_lock{m_resize_mtx};
            const bool unclaimed = !m_has_producer.load(std::memory_order_acquire);
            const bool empty = m_head.load(std::memory_order_acquire) == m_tail.load(std::memory_order_acquire);
            if (want > m_ring.size() && unclaimed && empty) {
                m_ring = std::vector<queue_type>(want); // default-construct slots (queue_type is move-only)
                m_mask = want - 1;
                m_head.store(0, std::memory_order_relaxed);
                m_tail.store(0, std::memory_order_relaxed);
            }
        }
        input_port_base::depth(value); // soft limit + capacity gauge
    }
    using input_port_base::depth; // keep the const getter overload visible

    /// Try-pop that distinguishes an EMPTY ring (returns std::nullopt) from a REAL packet (returns
    /// the packet, even one carrying a zero-length buffer) — resolving the size-0 ambiguity that
    /// get_data() cannot. This is the CANONICAL read: `if (auto pkt = in.try_get()) { ... }`, else
    /// return NOOP to idle on the doorbell (the base auto-FINISHes at end-of-stream). Single-consumer:
    /// pending()>0 guarantees pop() yields the slot.
    [[nodiscard]] auto try_get() -> std::optional<queue_type> {
        if (pending() == 0) {
            return std::nullopt;
        }
        return pop();
    }

    /// Non-blocking get: try-pop one packet, returning a default-constructed (size-0) buffer when the
    /// ring is empty (test `buffer.empty()`). Prefer try_get() — it distinguishes an empty ring from a
    /// genuine zero-length packet, which this overload cannot.
    auto get_data() -> queue_type { return pop(); }

    /// Drain up to @p out.size() packets in one shot, publishing a single head
    /// advance for the whole batch (amortizes the release fence + cache-line bounce
    /// across the batch). @return number of packets moved into @p out. Consumer-side.
    auto get_batch(std::span<queue_type> out) -> std::size_t {
        const auto head = m_head.load(std::memory_order_relaxed); // consumer-owned
        const auto tail = m_tail.load(std::memory_order_acquire); // observe producer
        const std::size_t avail = static_cast<std::size_t>(tail - head);
        const std::size_t k = avail < out.size() ? avail : out.size();
        // reverse doorbell: was the ring full before this drain? (see pop() — consumer owns
        // head; the batch frees k slots.) If so, the batch crosses the full->not-full edge.
        auto* producer_doorbell = m_producer_doorbell.load(std::memory_order_acquire);
        bool was_full = false;
        if (producer_doorbell != nullptr && k != 0) {
            const auto cap = depth() < m_ring.size() ? depth() : m_ring.size(); // match add_data's clamp
            was_full = avail >= cap;
        }
        std::size_t bytes = 0;
        for (std::size_t i = 0; i < k; ++i) {
            out[i] = std::move(m_ring[(head + i) & m_mask]);
            bytes += std::get<0>(out[i]).size() * sizeof(value_type);
        }
        if (k != 0) {
            m_head.store(head + k, std::memory_order_release); // one publish for the batch
            m_stats.record_transfer(bytes, k); // k packets, not 1 — keeps packets_transferred / drop_rate accurate
            update_queue_depth_metric(static_cast<std::size_t>(tail - (head + k)));
            if (was_full) {
                producer_doorbell->signal_data(); // wake an AWAIT_OUTPUT producer (see pop())
            }
        }
        return k;
    }

    auto size() const -> std::size_t {
        const auto t = m_tail.load(std::memory_order_acquire);
        const auto h = m_head.load(std::memory_order_acquire);
        return t - h;
    }
    /// @copydoc input_port_base::pending
    [[nodiscard]] auto pending() const -> std::size_t override { return size(); }
    auto clear() -> void {
        // Consumer-side drain. Safe only on the consumer thread (or while quiesced).
        while (m_head.load(std::memory_order_relaxed) != m_tail.load(std::memory_order_acquire)) {
            (void)pop();
        }
    }
    auto is_full() const -> bool override {
        const auto cap = clamped_capacity();
        return size() >= cap;
    }
    auto available_capacity() const -> std::size_t override {
        const auto cap = clamped_capacity();
        const auto s = size();
        return s >= cap ? 0 : cap - s;
    }

private:
    /// depth() clamped to the PHYSICAL ring size — add_data()'s clamp, but taken under
    /// m_resize_mtx.
    ///
    /// These two are the INTROSPECTION path (can_send() reaches them from REST threads), not the
    /// producer path. The producer may read m_ring/m_mask unlocked because a claimed input is
    /// never resized — the claim and the resize share m_resize_mtx — but an arbitrary reader has
    /// no such exclusion, and depth(value) reassigns the vector wholesale. Reading .size() while
    /// that happens is a torn read of a pointer/size pair, which is a data race outright and a
    /// bad clamp in practice. Both callers are cold, so the lock costs nothing.
    auto clamped_capacity() const -> std::size_t {
        const auto lock = std::scoped_lock{m_resize_mtx};
        const auto physical = m_ring.size();
        const auto soft = depth();
        return soft < physical ? soft : physical;
    }

public:
private:
    template <typename>
    friend class output_port;

    /// Producer side (single thread). Enqueue or drop-if-full.
    /// @return true if the packet was admitted; false if it was dropped (full, or paused).
    auto add_data(buffer_type data, timestamp ts, composite::metadata_ptr md = nullptr) -> bool {
        const auto tail = m_tail.load(std::memory_order_relaxed); // only the producer writes tail
        const auto head = m_head.load(std::memory_order_acquire); // observe consumer progress
        // Clamp the effective limit to the PHYSICAL ring capacity. depth() is a
        // soft limit that can be raised at runtime above the ring size (the ring
        // only grows while empty); without this clamp, a raised depth lets the
        // producer lap and overwrite unread slots — silent data loss, and a
        // read/write race with the consumer's pop().
        const auto cap = depth() < m_ring.size() ? depth() : m_ring.size();
        if (tail - head >= cap) { // full (or paused: depth()==0)
            record_drop();
            return false;
        }
        m_ring[tail & m_mask] = queue_type{std::move(data), ts, std::move(md)};
        // Release: the slot write above happens-before a consumer that acquire-loads
        // this new tail value, so the consumer never reads a torn/stale slot.
        m_tail.store(tail + 1, std::memory_order_release);
        // NOTE: the queue-depth gauge IS still written here (producer side). Moving it fully
        // off the data path needs a pull/observable gauge (compute size() from head/tail at
        // scrape time) so the live-enqueue-depth metric — a deliberate, tested feature — is
        // preserved without the per-packet producer+consumer cache-line ping-pong. That is a
        // metrics-subsystem addition, deferred; the per-packet clock and the atomic<shared_ptr>
        // connection-snapshot spinlock (the two larger steady-state taxes) are removed.
        update_queue_depth_metric(tail + 1 - head);
        // doorbell: FAST-PATH wake of an idle consumer on the empty->non-empty edge.
        // The edge is decided from a FRESH head re-loaded AFTER the publish: the entry
        // `head` (above) is stale — the consumer can drain to empty between that load and
        // here, so testing `tail == head` (entry head) misses real edges. fresh_head == tail
        // (the pre-increment index = the slot we just wrote) means the consumer has consumed
        // everything before it: the ring was empty and our packet is the only one — exactly
        // when an arming/sleeping consumer must be woken (and when armed, its head is frozen
        // here, so this test catches it). On a saturated stream head lags far behind, so this
        // is false and signal_data() (with its seq_cst fence) is never reached — the fence
        // lands only in the near-empty / consumer-keeps-up regime, not the throughput path.
        //
        // This is a best-effort LATENCY optimization, NOT the liveness guarantee: the
        // worker's wait_for_data() always carries the m_delay timeout, which is the true
        // backstop (and the worst-case wake latency). The fence-Dekker makes the common
        // single-add-vs-concurrent-arm case reliably fast; a rarer multi-packet burst that a
        // lagging consumer later observes as empty via a coherence-stale m_tail can miss the
        // fast wake and fall back to the m_delay timeout — the SAME bound the pre-doorbell
        // NOOP backoff always had, never a hang or data loss. So keep m_delay modest; treat
        // it as the wake-latency ceiling and the doorbell as the typical-case accelerator.
        if (m_doorbell != nullptr && m_head.load(std::memory_order_acquire) == tail) {
            m_doorbell->signal_data();
        }
        return true;
    }

    /// Producer side (single thread). Enqueue up to in.size() packets with a single
    /// tail publish; the rest (if the ring fills) are dropped + counted. Symmetric
    /// to get_batch — amortizes the release fence + cache-line bounce over the batch.
    /// @return number of packets accepted.
    /// @param accepted_bytes If non-null, receives the byte count of the ACCEPTED prefix
    ///        only — summed in the move loop below, so a producer can record an accurate
    ///        transfer stat on a partial admission (the packets are moved-from on return).
    auto add_batch(std::span<queue_type> in, std::size_t* accepted_bytes = nullptr) -> std::size_t {
        const auto tail = m_tail.load(std::memory_order_relaxed);
        const auto head = m_head.load(std::memory_order_acquire);
        const auto cur = tail - head;
        const auto cap = depth() < m_ring.size() ? depth() : m_ring.size(); // clamp to ring capacity
        const std::size_t room = cap > cur ? static_cast<std::size_t>(cap - cur) : 0;
        const std::size_t k = room < in.size() ? room : in.size();
        std::size_t bytes = 0;
        for (std::size_t i = 0; i < k; ++i) {
            bytes += std::get<0>(in[i]).size() * sizeof(value_type); // before the move
            m_ring[(tail + i) & m_mask] = std::move(in[i]);
        }
        if (accepted_bytes != nullptr) {
            *accepted_bytes = bytes;
        }
        if (k != 0) {
            m_tail.store(tail + k, std::memory_order_release); // one publish for the batch
            update_queue_depth_metric(tail + k - head);
            // doorbell: empty->non-empty edge via a FRESH head re-load (see add_data for
            // why the entry head is stale). fresh_head == tail (the pre-batch index) means
            // nothing before our batch is unconsumed — the ring was empty — so wake an idle
            // consumer; otherwise the consumer is busy/behind and cannot be armed.
            if (m_doorbell != nullptr && m_head.load(std::memory_order_acquire) == tail) {
                m_doorbell->signal_data();
            }
        }
        record_drop(in.size() - k); // aggregate the rejected suffix
        return k;
    }

    /// Producer-side direct batch for an output whose buffer type exactly
    /// matches this input. Moves accepted buffers straight into ring slots — no
    /// temporary packet vector and no second move pass. Rejected suffix buffers
    /// are consumed too, matching send_batch's contract.
    /// @param accepted_bytes If non-null, receives the byte count of the ACCEPTED prefix
    ///        only — summed in the move loop below, so the producer can record an accurate
    ///        transfer stat on a partial admission without a second pass over the span
    ///        (the buffers are moved-from once this returns and can no longer be sized).
    auto add_batch(std::span<buffer_type> in, timestamp ts, composite::metadata_ptr md = nullptr,
                   std::size_t* accepted_bytes = nullptr) -> std::size_t {
        const auto tail = m_tail.load(std::memory_order_relaxed);
        const auto head = m_head.load(std::memory_order_acquire);
        const auto cur = tail - head;
        const auto cap = depth() < m_ring.size() ? depth() : m_ring.size();
        const std::size_t room = cap > cur ? static_cast<std::size_t>(cap - cur) : 0;
        const std::size_t k = room < in.size() ? room : in.size();
        std::size_t bytes = 0;
        for (std::size_t i = 0; i < k; ++i) {
            bytes += in[i].size() * sizeof(value_type); // before the move: in[i] is emptied below
            auto packet_md = (i + 1 == k) ? std::move(md) : md;
            m_ring[(tail + i) & m_mask] = queue_type{std::move(in[i]), ts, std::move(packet_md)};
        }
        if (accepted_bytes != nullptr) {
            *accepted_bytes = bytes;
        }
        // A send consumes the complete span even when bounded admission accepts
        // only a prefix. Release rejected buffers without constructing packets.
        for (std::size_t i = k; i < in.size(); ++i) {
            in[i] = buffer_type{};
        }
        if (k != 0) {
            m_tail.store(tail + k, std::memory_order_release);
            update_queue_depth_metric(tail + k - head);
            if (m_doorbell != nullptr && m_head.load(std::memory_order_acquire) == tail) {
                m_doorbell->signal_data();
            }
        }
        record_drop(in.size() - k);
        return k;
    }

    /// Consumer side (single thread). Try-pop; empty packet if the ring is empty.
    auto pop() -> queue_type {
        const auto head = m_head.load(std::memory_order_relaxed); // only the consumer writes head
        const auto tail = m_tail.load(std::memory_order_acquire); // observe producer publication
        if (head == tail) {
            return {};
        } // empty
        // reverse doorbell: was the ring FULL before this pop? (consumer owns head -> fresh;
        // tail is acquire-loaded, and a stale-low tail can only UNDER-report full, never falsely
        // report it.) If so, this pop is the full->not-full edge — the moment to wake an
        // AWAIT_OUTPUT producer. Only computed when a producer worker is wired (a
        // component-to-component connection); free-standing inputs skip it.
        auto* producer_doorbell = m_producer_doorbell.load(std::memory_order_acquire);
        bool was_full = false;
        if (producer_doorbell != nullptr) {
            const auto cap = depth() < m_ring.size() ? depth() : m_ring.size(); // match add_data's clamp
            was_full = (tail - head) >= cap;
        }
        queue_type result = std::move(m_ring[head & m_mask]);
        // Release: our read of the slot happens-before the producer that acquire-loads
        // this new head and then overwrites the slot one lap later.
        m_head.store(head + 1, std::memory_order_release);
        const auto& [buffer, ts_val, md] = result;
        (void)ts_val;
        (void)md;
        m_stats.record_transfer(buffer.size() * sizeof(value_type));
        update_queue_depth_metric(tail - (head + 1));
        // signal_data() fences then bails lock-free unless the producer is armed (i.e. actually
        // AWAIT_OUTPUT-sleeping) — off the steady-state path. The producer's arm + can_send
        // re-check closes the pre-sleep race; this signal closes the post-sleep one.
        if (was_full) {
            producer_doorbell->signal_data();
        }
        return result;
    }

    auto record_drop(std::size_t count = 1) -> void {
        if (count == 0) {
            return;
        }
        m_stats.record_drop(count);
        // COPY the callback under the lock, then invoke it OUTSIDE. m_mtx is a plain
        // (non-recursive) mutex that set_overflow_callback() also takes, so holding it across the
        // call deadlocked the producer on the most natural idiom there is: a callback that
        // disarms or rate-limits itself by re-setting the callback from inside itself.
        overflow_callback callback;
        {
            const auto lock = std::scoped_lock{m_mtx};
            callback = m_overflow_callback;
        }
        if (callback) {
            // USER code, invoked on the producer's send path. An
            // exception here would unwind out through add_data()/add_batch() and
            // output_port::send_data()/send_batch() into the producer's worker — a drop is
            // a normal bounded-backpressure event, so it must never become a producer
            // fault. Contain it and count it (no logger at this layer, and a per-drop log
            // on a saturated port would be a flood); read the count via
            // overflow_callback_errors().
            try {
                callback(count);
            } catch (...) { // NOLINT(bugprone-empty-catch) — counted below; see comment
                m_overflow_callback_errors.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    static constexpr std::size_t k_cacheline = 64;           ///< keep producer/consumer indices off one line
    std::vector<queue_type> m_ring;                          ///< ring storage; size is a power of two
    std::size_t m_mask{0};                                   ///< size - 1 (index mask)
    alignas(k_cacheline) std::atomic<std::size_t> m_head{0}; ///< consumer index (consumer writes)
    alignas(k_cacheline) std::atomic<std::size_t> m_tail{0}; ///< producer index (producer writes)

}; // class input_port<Buf>

} // namespace composite
