/*
 * Copyright (C) 2025-2026 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "composite/buffers/aligned_mem.hpp"
#include "composite/buffers/buffer.hpp"
#include "composite/buffers/external_buffer.hpp"

#include <atomic>
#include <cstddef>
#include <cstring>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace composite {

namespace detail {
/// Architecture-appropriate spin hint for a brief busy-wait (portable: x86 PAUSE, ARM YIELD,
/// else a no-op). Used by overlap_ring's bounded backpressure spin before it falls back to yield.
inline auto cpu_relax() noexcept -> void {
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#endif
}

/// Tiny spinlock serializing overlap_ring's per-FRAME slot-count/protected-watermark transitions.
/// It is NOT on the per-sample write() path or the can_write_fast backpressure check (both stay
/// lock-free); it only makes the acquire-side (emit) and release-side (slot deleter) updates of
/// {slots_in_use, oldest_protected} mutually exclusive, closing the lost-update window where a
/// release resetting the watermark could race an acquire's watermark decision. BasicLockable so it
/// works with std::lock_guard.
class watermark_lock {
public:
    auto lock() noexcept -> void {
        while (m_flag.test_and_set(std::memory_order_acquire)) { cpu_relax(); }
    }
    auto unlock() noexcept -> void { m_flag.clear(std::memory_order_release); }
private:
    std::atomic_flag m_flag = ATOMIC_FLAG_INIT;
};
} // namespace detail

/**
 * @brief Lock-free overlapped framing ring for one producer and many short-lived frame readers.
 *
 * A single producer writes a stream of `T` samples into a contiguous ring; readers pull fixed-size,
 * overlapping frames (frame_size samples, advancing by hop_size = frame_size - overlap) as
 * zero-copy `immutable_buffer<T>` views into the ring. A frame view holds a slot; while ANY frame
 * is outstanding the producer will not overwrite the samples it covers (a "protected region"
 * watermark gives drop-on-backpressure rather than corruption). The ring carries a `frame_size`
 * tail mirror of its own prefix so a frame that straddles the wrap is still read contiguously.
 *
 * Concurrency: write() is single-producer and the per-sample write path plus the can_write_fast
 * backpressure check are lock-free (atomic write head + protected-watermark). Frame emission/release
 * is safe from any thread (the slot deleter runs on the last reader's thread); a short spinlock
 * (watermark_lock) serializes ONLY the per-frame slot-count + protected-watermark transition, so a
 * release can't lose a concurrent acquire's watermark update (which would leave a held frame
 * unprotected). `T` must be trivially copyable (the ring is raw sample memory).
 *
 * This is the framework-side generalization of the components' former framer_pool: the byte/format
 * conversion that used to live in `write_samples` is now injected by the caller via write()'s
 * writer callback, so the ring itself is format-agnostic and reusable.
 */
template <typename T>
class overlap_ring : public std::enable_shared_from_this<overlap_ring<T>> {
public:
    enum class drop_reason {
        NONE,
        BATCH_TOO_LARGE,
        BACKPRESSURE_TIMEOUT
    };

    struct diagnostics {
        std::size_t slots_in_use{0};
        std::size_t total_slots{0};
        std::size_t write_head{0};
        std::size_t ring_size{0};
        std::size_t oldest_protected_sample{0};
        std::size_t available_space{0};
        drop_reason last_drop_reason{drop_reason::NONE};
        std::size_t last_drop_sample_count{0};
    };

    struct frame_slot {
        std::atomic<bool> in_use{false};
        std::atomic<std::size_t> start_sample{0};
    };

    overlap_ring(std::size_t frame_size, std::size_t overlap, std::size_t frame_count) :
      m_frame_size(frame_size),
      m_frame_count(frame_count),
      m_overlap(overlap),
      m_slots(frame_count) {
        if (frame_size == 0) {
            throw std::invalid_argument("overlap_ring: frame_size must be > 0");
        }
        if (overlap >= frame_size) {
            throw std::invalid_argument("overlap_ring: overlap must be < frame_size");
        }
        if (frame_count == 0) {
            throw std::invalid_argument("overlap_ring: frame_count must be > 0");
        }

        m_hop_size = m_frame_size - m_overlap;
        m_ring_size = frame_count * m_hop_size + m_overlap;
        m_ring_capacity = m_ring_size + m_frame_size;  // tail mirror for contiguous wrapped frames

        m_ring = std::make_shared<composite::aligned_mem<T>>(64, m_ring_capacity);
        m_oldest_protected.store(std::numeric_limits<std::size_t>::max(), std::memory_order_relaxed);
    }

    /**
     * @brief Write @p sample_count samples into the ring (single-producer, lock-free hot path).
     *
     * The ring computes placement and handles wraparound + the tail mirror; the caller supplies the
     * actual sample production via @p writer, invoked as `writer(T* dst, std::size_t src_index,
     * std::size_t count)` to produce/convert @p count samples (source samples [src_index, src_index
     * + count)) into the contiguous destination @p dst. For a non-wrapping write @p writer is called
     * once; for a wrapping write it is called twice (the second with the source offset of the split).
     *
     * Backpressure: if writing would overrun the protected region (samples covered by an outstanding
     * frame), the call spins briefly, then yields, then — still blocked — drops the batch and returns
     * false (recording BACKPRESSURE_TIMEOUT). A batch larger than the ring is dropped immediately
     * (BATCH_TOO_LARGE). A zero-count write is a successful no-op.
     *
     * @return true if the samples were written; false if the batch was dropped.
     */
    template <typename Writer>
    auto write(std::size_t sample_count, Writer&& writer) -> bool {
        if (sample_count == 0) {
            return true;  // valid no-op
        }
        if (sample_count > m_ring_size) {
            m_drop_reason.store(drop_reason::BATCH_TOO_LARGE, std::memory_order_relaxed);
            m_last_drop_sample_count.store(sample_count, std::memory_order_relaxed);
            return false;
        }

        auto write_head = m_write_head.load(std::memory_order_relaxed);

        if (!can_write_fast(write_head, sample_count)) {
            constexpr int SPIN_COUNT = 1000;
            bool ready = false;
            for (int i = 0; i < SPIN_COUNT && !ready; ++i) {
                detail::cpu_relax();
                ready = can_write_fast(write_head, sample_count);
            }
            if (!ready) {
                constexpr int MAX_YIELDS = 100;
                for (int i = 0; i < MAX_YIELDS && !ready; ++i) {
                    std::this_thread::yield();
                    ready = can_write_fast(write_head, sample_count);
                }
            }
            if (!ready) {
                m_drop_reason.store(drop_reason::BACKPRESSURE_TIMEOUT, std::memory_order_relaxed);
                m_last_drop_sample_count.store(sample_count, std::memory_order_relaxed);
                return false;
            }
        }

        auto* ring_base = m_ring->data();
        const std::size_t ring_pos = write_head % m_ring_size;

        // Produce the samples; track which prefix bytes were touched so the tail mirror can refresh.
        std::size_t prefix_written_end;  // exclusive end of the ring-prefix region this write wrote
        if (ring_pos + sample_count <= m_ring_size) {
            // No wrap: wrote [ring_pos, ring_pos + sample_count).
            writer(ring_base + ring_pos, std::size_t{0}, sample_count);
            prefix_written_end = (ring_pos < m_frame_size) ? (ring_pos + sample_count) : 0;
        } else {
            // Wraps: split; the second chunk lands in the prefix [0, second_chunk).
            const std::size_t first_chunk = m_ring_size - ring_pos;
            const std::size_t second_chunk = sample_count - first_chunk;
            writer(ring_base + ring_pos, std::size_t{0}, first_chunk);
            writer(ring_base + 0, first_chunk, second_chunk);
            prefix_written_end = second_chunk;
        }

        // Refresh the tail mirror. A frame that wraps past m_ring_size is read contiguously from the
        // tail region [m_ring_size, m_ring_size + m_frame_size), which must ALWAYS equal the ring
        // prefix [0, m_frame_size). Refresh after EVERY write that touched the prefix (not only on
        // wrap): a later non-wrapping write into the prefix would otherwise leave the mirror stale.
        // Clamp the copied range to the tail capacity (m_frame_size).
        if (const std::size_t lo = (ring_pos + sample_count <= m_ring_size) ? ring_pos : 0;
            prefix_written_end > lo) {
            const std::size_t hi = std::min(prefix_written_end, m_frame_size);
            if (hi > lo) {
                std::memcpy(ring_base + m_ring_size + lo, ring_base + lo, (hi - lo) * sizeof(T));
            }
        }

        // Publish: release so frame readers see the written samples.
        m_write_head.store(write_head + sample_count, std::memory_order_release);
        return true;
    }

    /**
     * @brief Try to emit the frame starting at absolute sample index @p absolute_start.
     *
     * @p absolute_start must be hop-aligned. Returns std::nullopt if the frame's samples have not
     * been fully written yet, or if the slot for that frame is still held by a downstream reader.
     * On success, returns a zero-copy immutable_buffer<T> view of frame_size samples; the slot is
     * released (unprotecting those samples) when the last copy of the returned buffer is destroyed.
     */
    auto try_emit_frame(std::size_t absolute_start) -> std::optional<composite::immutable_buffer<T>> {
        if (absolute_start % m_hop_size != 0) {
            throw std::logic_error(std::format(
                "overlap_ring::try_emit_frame: absolute_start ({}) is not aligned to hop_size ({})",
                absolute_start, m_hop_size));
        }

        const auto write_head = m_write_head.load(std::memory_order_acquire);
        if (absolute_start + m_frame_size > write_head) {
            return std::nullopt;  // frame data not fully written yet
        }

        const auto frame_num = absolute_start / m_hop_size;
        const auto slot_idx = frame_num % m_frame_count;
        auto& slot = m_slots[slot_idx];

        {
            // Serialize the reserve + watermark bump against concurrent releases (the lost-update
            // fix): otherwise a release seeing prev_count==1 could reset oldest=MAX in the window
            // between this acquire's watermark decision and its slot-count increment, leaving this
            // just-acquired frame in-use but unprotected -> the producer wraps and overwrites it.
            std::lock_guard<detail::watermark_lock> wm{m_wm_lock};
            bool expected = false;
            if (!slot.in_use.compare_exchange_strong(expected, true,
                    std::memory_order_acquire, std::memory_order_relaxed)) {
                return std::nullopt;  // slot busy: downstream still holding the previous frame here
            }
            slot.start_sample.store(absolute_start, std::memory_order_relaxed);
            // Single producer acquires in strictly increasing start order, so any existing in-use
            // frame is older; only an empty->non-empty transition moves the oldest-protected mark.
            if (m_slots_in_use.fetch_add(1, std::memory_order_relaxed) == 0) {
                m_oldest_protected.store(absolute_start, std::memory_order_release);
            }
        }

        const auto ring_offset = absolute_start % m_ring_size;  // contiguous on wrap via the tail mirror
        auto* data_ptr = m_ring->data() + ring_offset;

        auto deleter = slot_deleter{.pool = this->shared_from_this(), .slot_index = slot_idx};
        return composite::immutable_buffer<T>(
            composite::external_buffer<T>(data_ptr, m_frame_size, deleter));
    }

    auto frame_size() const noexcept -> std::size_t { return m_frame_size; }
    auto hop_size() const noexcept -> std::size_t { return m_hop_size; }
    auto overlap() const noexcept -> std::size_t { return m_overlap; }
    auto ring_size() const noexcept -> std::size_t { return m_ring_size; }

    /// Lock-free read of the running write head (total samples written).
    auto head() const noexcept -> std::size_t {
        return m_write_head.load(std::memory_order_acquire);
    }

    auto get_diagnostics() const -> diagnostics {
        auto write_head = m_write_head.load(std::memory_order_acquire);
        auto slots_in_use = m_slots_in_use.load(std::memory_order_relaxed);
        auto oldest = m_oldest_protected.load(std::memory_order_relaxed);

        if (slots_in_use == 0) {
            oldest = write_head;  // no slots in use -> oldest_protected is meaningless
        }

        std::size_t available_space = 0;
        if (oldest == std::numeric_limits<std::size_t>::max()) {
            available_space = m_ring_size;
        } else if (write_head >= oldest) {
            available_space = m_ring_size - (write_head - oldest);
        }

        return diagnostics{
            .slots_in_use = slots_in_use,
            .total_slots = m_frame_count,
            .write_head = write_head,
            .ring_size = m_ring_size,
            .oldest_protected_sample = oldest,
            .available_space = available_space,
            .last_drop_reason = m_drop_reason.load(std::memory_order_relaxed),
            .last_drop_sample_count = m_last_drop_sample_count.load(std::memory_order_relaxed),
        };
    }

private:
    struct slot_deleter {
        std::shared_ptr<overlap_ring<T>> pool;
        std::size_t slot_index;

        auto operator()(T* /*data*/) const noexcept -> void {
            // Serialized with acquire (emit) so slot-count and watermark stay consistent.
            std::lock_guard<detail::watermark_lock> wm{pool->m_wm_lock};
            const auto released_start =
                pool->m_slots[slot_index].start_sample.load(std::memory_order_relaxed);
            pool->m_slots[slot_index].in_use.store(false, std::memory_order_release);
            pool->m_slots_in_use.fetch_sub(1, std::memory_order_relaxed);
            // Recompute only when we released the CURRENT oldest (race-free read under the lock);
            // releasing a newer frame leaves the watermark correct. recompute_oldest_locked() sets
            // MAX when no frame remains. Its RELEASE store pairs with can_write_fast's acquire, so a
            // producer overwrite of a now-unprotected region happens-after this thread's frame reads.
            if (released_start == pool->m_oldest_protected.load(std::memory_order_relaxed)) {
                pool->recompute_oldest_locked();
            }
        }
    };

    /// Fast O(1) backpressure check: don't overwrite the protected region.
    ///
    /// ACQUIRE-loads m_oldest_protected so it synchronizes-with the RELEASE store the slot deleter
    /// makes when a frame is freed (see slot_deleter / recompute_oldest_locked). That
    /// pairing is what gives the producer's subsequent overwrite of a just-freed ring region a
    /// happens-after edge against the consumer thread's last read of that frame's samples — without
    /// it the overwrite races the read (benign on x86/TSO, a genuine torn-read UB on weakly-ordered
    /// targets such as ARM, where this ring is also built).
    auto can_write_fast(std::size_t write_head, std::size_t count) const noexcept -> bool {
        if (count == 0) {
            return true;
        }
        const auto oldest = m_oldest_protected.load(std::memory_order_acquire);
        if (oldest == std::numeric_limits<std::size_t>::max()) {
            return true;  // no slots in use
        }
        return (write_head - oldest + count) <= m_ring_size;
    }

    /// Recompute oldest_protected = min(start_sample) over in-use slots (MAX if none). Caller MUST
    /// hold m_wm_lock, so the in_use/start_sample scan is a consistent snapshot. RELEASE store pairs
    /// with can_write_fast's acquire load (the ARM ordering guarantee for the freed region).
    auto recompute_oldest_locked() noexcept -> void {
        std::size_t oldest = std::numeric_limits<std::size_t>::max();
        for (const auto& slot : m_slots) {
            if (slot.in_use.load(std::memory_order_relaxed)) {
                const auto start = slot.start_sample.load(std::memory_order_relaxed);
                if (oldest == std::numeric_limits<std::size_t>::max() || start < oldest) {
                    oldest = start;
                }
            }
        }
        m_oldest_protected.store(oldest, std::memory_order_release);
    }

    std::size_t m_frame_size{};
    std::size_t m_frame_count{};
    std::size_t m_ring_size{};
    std::size_t m_ring_capacity{};
    std::size_t m_overlap{};
    std::size_t m_hop_size{};
    std::shared_ptr<composite::aligned_mem<T>> m_ring{nullptr};

    std::atomic<std::size_t> m_write_head{0};
    std::atomic<std::size_t> m_oldest_protected{std::numeric_limits<std::size_t>::max()};
    std::atomic<std::size_t> m_slots_in_use{0};
    std::vector<frame_slot> m_slots;
    mutable detail::watermark_lock m_wm_lock;  // serializes emit/release watermark+count transitions

    std::atomic<drop_reason> m_drop_reason{drop_reason::NONE};
    std::atomic<std::size_t> m_last_drop_sample_count{0};
}; // class overlap_ring

} // namespace composite
