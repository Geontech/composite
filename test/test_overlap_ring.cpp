// Lifted from the components' framer_pool: the framework overlap_ring<T> — a lock-free
// overlapped framing ring. Verifies the generic guarantees the framer relied on:
//   - construction validation; hop-aligned emission; not-ready -> nullopt;
//   - frame contents + overlap correctness across multiple hops;
//   - WRAPAROUND CONTIGUITY via the tail mirror (a frame straddling the wrap reads correct
//     contiguous samples — the mirror regression, now guarded where the code lives);
//   - slot conflict (a held slot blocks re-emit; frees on release);
//   - drop-on-backpressure (protected region) + batch-too-large;
//   - a concurrent producer + frame reader (run under TSan/ASan).
#include "composite/buffers/overlap_ring.hpp"

#include <atomic>
#include <complex>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

using cf = std::complex<float>;
using composite::overlap_ring;

namespace {
int g_failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL: %s\n", what); ++g_failures; }
}

// Write `count` samples whose value at absolute index a is {a, -a}. `base` is the absolute index of
// the first sample of this batch (the ring's write head before the call), so the producer can stamp
// each sample with its absolute index regardless of the wrap split.
auto write_seq(const std::shared_ptr<overlap_ring<cf>>& ring, std::size_t base, std::size_t count) -> bool {
    return ring->write(count, [base](cf* dst, std::size_t src_idx, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            const auto a = static_cast<float>(base + src_idx + i);
            dst[i] = cf{a, -a};
        }
    });
}

// A frame starting at `start` must contain absolute samples [start, start+frame_size).
auto frame_ok(const composite::immutable_buffer<cf>& f, std::size_t start, std::size_t frame_size) -> bool {
    if (f.size() != frame_size) { return false; }
    const auto* p = f.data();
    for (std::size_t i = 0; i < frame_size; ++i) {
        const auto a = static_cast<float>(start + i);
        if (p[i].real() != a || p[i].imag() != -a) { return false; }
    }
    return true;
}
} // namespace

int main() {
    // ---- construction validation ----
    try { (void)std::make_shared<overlap_ring<cf>>(0, 4, 8); check(false, "frame_size==0 should throw"); }
    catch (const std::invalid_argument&) {}
    try { (void)std::make_shared<overlap_ring<cf>>(8, 8, 8); check(false, "overlap>=frame_size should throw"); }
    catch (const std::invalid_argument&) {}
    try { (void)std::make_shared<overlap_ring<cf>>(8, 4, 0); check(false, "frame_count==0 should throw"); }
    catch (const std::invalid_argument&) {}

    // ---- basic write + emit + contents ----
    {
        constexpr std::size_t FS = 8, OV = 4, FC = 4, HOP = FS - OV;  // hop=4, ring_size=4*4+4=20
        auto ring = std::make_shared<overlap_ring<cf>>(FS, OV, FC);
        check(ring->frame_size() == FS && ring->hop_size() == HOP && ring->head() == 0, "geometry");
        check(!ring->try_emit_frame(0).has_value(), "emit before data -> nullopt");
        check(write_seq(ring, 0, FS), "write FS samples");
        auto f0 = ring->try_emit_frame(0);
        check(f0.has_value() && frame_ok(*f0, 0, FS), "frame@0 contents");
        // hop alignment is enforced.
        bool threw = false;
        try { (void)ring->try_emit_frame(1); } catch (const std::logic_error&) { threw = true; }
        check(threw, "non-hop-aligned start throws");
        // zero-count write is a successful no-op that does not advance the head.
        const auto h = ring->head();
        check(write_seq(ring, h, 0), "zero-count write is a no-op");
        check(ring->head() == h, "zero-count write does not advance head");
        // a batch exactly equal to ring_size is accepted (boundary of the watermark check).
        auto exact = std::make_shared<overlap_ring<cf>>(FS, OV, FC);
        check(write_seq(exact, 0, exact->ring_size()), "exact ring_size fill accepted");
    }

    // ---- multiframe + overlap correctness ----
    {
        constexpr std::size_t FS = 8, OV = 4, FC = 8, HOP = FS - OV;
        auto ring = std::make_shared<overlap_ring<cf>>(FS, OV, FC);
        std::size_t total = 0;
        for (int k = 0; k < 4; ++k) { check(write_seq(ring, total, HOP), "write hop"); total += HOP; }
        check(write_seq(ring, total, OV), "write trailing overlap"); total += OV;  // enough for 4 frames
        for (std::size_t s = 0; s + FS <= total; s += HOP) {
            auto f = ring->try_emit_frame(s);
            check(f.has_value() && frame_ok(*f, s, FS), "multiframe contents");
        }
    }

    // ---- WRAPAROUND CONTIGUITY (tail mirror) ----
    {
        constexpr std::size_t FS = 8, OV = 4, FC = 4;  // ring_size = 20
        auto ring = std::make_shared<overlap_ring<cf>>(FS, OV, FC);
        // Write 24 samples in two batches (each <= ring_size=20), wrapping the ring at 20.
        check(write_seq(ring, 0, 16), "write 16");
        check(write_seq(ring, 16, 8), "write 8 (wraps at 20)");
        // Frame@16 spans [16,24): ring positions 16..19 then the wrapped 0..3, read contiguously
        // from the tail mirror. Correct contents prove the mirror is fresh.
        auto fw = ring->try_emit_frame(16);
        check(fw.has_value() && frame_ok(*fw, 16, FS), "wrap-straddling frame@16 reads contiguous samples");
    }

    // ---- slot conflict: a held slot blocks re-emit; frees on release ----
    {
        constexpr std::size_t FS = 8, OV = 4, FC = 4;
        auto ring = std::make_shared<overlap_ring<cf>>(FS, OV, FC);
        check(write_seq(ring, 0, FS), "write FS");
        auto held = ring->try_emit_frame(0);                 // slot 0 now in use
        check(held.has_value(), "emit frame@0");
        check(!ring->try_emit_frame(0).has_value(), "re-emit while slot held -> nullopt");
        held.reset();                                        // release the frame -> slot freed
        check(ring->try_emit_frame(0).has_value(), "re-emit after release -> ok");
    }

    // ---- drop-on-backpressure (protected region) + batch-too-large ----
    {
        constexpr std::size_t FS = 8, OV = 4, FC = 4;  // ring_size = 20
        auto ring = std::make_shared<overlap_ring<cf>>(FS, OV, FC);
        // Batch larger than the ring is dropped immediately.
        check(!write_seq(ring, 0, 21), "batch > ring_size dropped");
        check(ring->get_diagnostics().last_drop_reason == overlap_ring<cf>::drop_reason::BATCH_TOO_LARGE,
              "BATCH_TOO_LARGE recorded");
        // Fill the ring, hold frame@0 (protects [0,8)), then a write that would overwrite the
        // protected region is dropped (BACKPRESSURE_TIMEOUT); releasing the frame lets it through.
        check(write_seq(ring, 0, 20), "fill ring_size");   // head=20, ring full
        auto held = ring->try_emit_frame(0);
        check(held.has_value(), "hold frame@0 (protects [0,8))");
        check(!write_seq(ring, 20, 8), "write into protected region dropped");
        check(ring->get_diagnostics().last_drop_reason == overlap_ring<cf>::drop_reason::BACKPRESSURE_TIMEOUT,
              "BACKPRESSURE_TIMEOUT recorded");
        held.reset();
        check(write_seq(ring, 20, 8), "write succeeds after frame released");
    }

    // ---- concurrent produce/emit (worker thread) vs frame release (downstream thread) ----
    // This is the ACTUAL cross-thread race in this design: the framer writes + emits on its worker
    // thread, while a downstream consumer destroys the immutable_buffer<T> frames on its own thread,
    // running the slot deleter (slot release + oldest_protected update) concurrently with the
    // producer's write/can_write/try_emit. Frames are emitted right after their samples are written
    // (so they are protected and never overwritten while outstanding) and are content-verified ON
    // THE PRODUCER while held; the consumer only releases. Run under TSan (lock-free slot/watermark
    // atomics) and ASan (the external_buffer release path). Accounting must return to baseline.
    {
        constexpr std::size_t FS = 64, OV = 16, FC = 32, HOP = FS - OV;
        auto ring = std::make_shared<overlap_ring<cf>>(FS, OV, FC);
        std::mutex m;
        std::vector<composite::immutable_buffer<cf>> handoff;  // frames awaiting release by the consumer
        std::atomic<bool> done{false};
        std::atomic<bool> bad{false};

        std::thread consumer([&] {
            while (true) {
                composite::immutable_buffer<cf> f;
                {
                    std::scoped_lock lk{m};
                    if (!handoff.empty()) { f = std::move(handoff.back()); handoff.pop_back(); }
                }
                if (f.has_data()) {
                    // READ the frame's samples on THIS (consumer) thread, then release it (destroyed
                    // at loop end -> slot_deleter runs here). This read is the other side of the
                    // producer-overwrite-vs-consumer-read race that the watermark acquire/release
                    // closes; TSan validates the happens-before. Verify internal contiguity (a torn
                    // read across the wrap mirror would break it).
                    const auto* p = f.data();
                    const auto base = p[0].real();
                    for (std::size_t i = 0; i < f.size(); ++i) {
                        if (p[i].real() != base + static_cast<float>(i)) {
                            bad.store(true, std::memory_order_relaxed);
                        }
                    }
                } else if (done.load(std::memory_order_acquire)) {
                    break;
                } else {
                    std::this_thread::yield();
                }
            }
        });

        std::size_t total = 0;     // samples successfully written
        std::size_t next = 0;      // next frame start to emit
        for (int i = 0; i < 20000; ++i) {
            if (write_seq(ring, total, HOP)) { total += HOP; }      // may drop under backpressure
            while (ring->head() >= next + FS) {
                auto f = ring->try_emit_frame(next);
                if (!f) { break; }                                  // slot still held by consumer
                if (!frame_ok(*f, next, FS)) { bad.store(true, std::memory_order_relaxed); }  // verified while HELD
                next += HOP;
                std::scoped_lock lk{m};
                handoff.push_back(std::move(*f));
            }
        }
        done.store(true, std::memory_order_release);
        consumer.join();
        // Drain anything left, then accounting must be back to baseline.
        handoff.clear();
        check(!bad.load(), "every emitted frame had correct contents while held");
        check(ring->get_diagnostics().slots_in_use == 0, "all frame slots released after run");
    }

    if (g_failures) { std::printf("\n%d FAILURE(S)\n", g_failures); return 1; }
    std::puts("OVERLAP_RING OK: construction, emit, overlap, wraparound mirror, slot conflict, "
              "backpressure, and concurrent producer/reader all correct");
    return 0;
}
