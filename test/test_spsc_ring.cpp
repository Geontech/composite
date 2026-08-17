// Correctness of the lock-free SPSC input-port ring under real contention
// (one producer thread, one consumer thread). Verifies:
//   A) throttled producer (never full): the consumer receives EVERY packet in
//      exact order with intact multi-element payloads — no loss/dup/reorder/tear.
//   B) flooded producer (ring overflows): drops are allowed, but the received
//      subsequence is still strictly increasing with intact payloads — the drop
//      path doesn't corrupt, duplicate, or reorder.
// Run under TSan (no data race on slots/indices) and ASan/UBSan.
#include "composite/buffers/buffer.hpp"
#include "composite/ports/input_port.hpp"
#include "composite/ports/output_port.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <span>
#include <thread>
#include <vector>

using namespace composite;

namespace {
constexpr std::uint64_t N = 1'000'000;
constexpr std::size_t PAYLOAD = 4; // each packet carries its seq in all 4 elements

auto fill(mutable_buffer<std::uint64_t>& b, std::uint64_t seq) -> void {
    for (auto& v : b.as_span()) {
        v = seq;
    }
}
// Returns the packet's seq, or sets *bad if the payload isn't uniform (torn read).
auto read_seq(const mutable_buffer<std::uint64_t>& b, std::atomic<bool>& bad) -> std::uint64_t {
    auto span = b.as_span();
    const std::uint64_t seq = span.empty() ? 0 : span[0];
    if (span.size() != PAYLOAD) {
        bad.store(true, std::memory_order_relaxed);
    }
    for (auto v : span) {
        if (v != seq) {
            bad.store(true, std::memory_order_relaxed);
        }
    }
    return seq;
}
} // namespace

int main() {
    // ---- A) throttled: exact FIFO, zero loss ----
    {
        output_port<mutable_buffer<std::uint64_t>> out{"o"};
        input_port<mutable_buffer<std::uint64_t>> in{"i", 64}; // tiny ring -> heavy contention
        if (!out.connect(&in)) {
            std::puts("FAIL: connect (A)");
            return 1;
        }

        std::atomic<bool> go{false}, bad{false};
        std::atomic<std::uint64_t> recv{0};
        std::thread cons([&] {
            while (!go.load(std::memory_order_acquire)) {
            }
            std::uint64_t expect = 0;
            while (expect < N) {
                auto [buf, ts, md] = in.get_data();
                if (buf.size() == 0) {
                    continue;
                } // empty -> retry
                if (read_seq(buf, bad) != expect) {
                    bad.store(true, std::memory_order_relaxed);
                }
                ++expect;
                recv.fetch_add(1, std::memory_order_relaxed);
            }
        });

        go.store(true, std::memory_order_release);
        for (std::uint64_t s = 0; s < N; ++s) {
            while (in.is_full()) {
                std::this_thread::yield();
            } // throttle => no drops
            auto b = make_mutable<std::uint64_t>(PAYLOAD);
            fill(b, s);
            out.send_data(std::move(b), timestamp{});
        }
        cons.join();
        if (bad.load()) {
            std::puts("FAIL: corruption/torn-read/reorder (A)");
            return 1;
        }
        if (recv.load() != N) {
            std::printf("FAIL: lost packets (A) recv=%llu N=%llu\n", (unsigned long long)recv.load(),
                        (unsigned long long)N);
            return 1;
        }
        std::printf("SPSC throttled (no-drop) OK: %llu packets exact-order, payloads intact\n", (unsigned long long)N);
    }

    // ---- B) flooded: drops allowed, but FIFO/integrity of survivors holds ----
    {
        output_port<mutable_buffer<std::uint64_t>> out{"o"};
        input_port<mutable_buffer<std::uint64_t>> in{"i", 64};
        if (!out.connect(&in)) {
            std::puts("FAIL: connect (B)");
            return 1;
        }

        std::atomic<bool> producer_done{false}, bad{false};
        std::atomic<std::uint64_t> recv{0};
        std::thread cons([&] {
            std::uint64_t last = 0;
            bool first = true;
            for (;;) {
                auto [buf, ts, md] = in.get_data();
                if (buf.size() == 0) {
                    if (producer_done.load(std::memory_order_acquire) && in.size() == 0) {
                        break;
                    }
                    continue;
                }
                const std::uint64_t seq = read_seq(buf, bad);
                if (!first && seq <= last) {
                    bad.store(true, std::memory_order_relaxed);
                } // dup/reorder
                last = seq;
                first = false;
                recv.fetch_add(1, std::memory_order_relaxed);
            }
        });

        for (std::uint64_t s = 0; s < N; ++s) {
            auto b = make_mutable<std::uint64_t>(PAYLOAD);
            fill(b, s);
            out.send_data(std::move(b), timestamp{}); // no throttle -> ring overflows, drops
        }
        producer_done.store(true, std::memory_order_release);
        cons.join();
        if (bad.load()) {
            std::puts("FAIL: reorder/dup/corruption on the drop path (B)");
            return 1;
        }
        std::printf("SPSC flooded (drops) OK: received %llu of %llu, strictly increasing + intact\n",
                    (unsigned long long)recv.load(), (unsigned long long)N);
    }

    // ---- C) consumer drains via get_batch(): exact FIFO preserved ----
    {
        using port_t = input_port<mutable_buffer<std::uint64_t>>;
        output_port<mutable_buffer<std::uint64_t>> out{"o"};
        port_t in{"i", 256};
        if (!out.connect(&in)) {
            std::puts("FAIL: connect (C)");
            return 1;
        }

        std::atomic<bool> go{false}, bad{false};
        std::atomic<std::uint64_t> recv{0};
        std::thread cons([&] {
            while (!go.load(std::memory_order_acquire)) {
            }
            std::array<port_t::queue_type, 64> batch;
            std::uint64_t expect = 0;
            while (expect < N) {
                const std::size_t k = in.get_batch(std::span<port_t::queue_type>(batch.data(), batch.size()));
                for (std::size_t i = 0; i < k; ++i) {
                    if (read_seq(std::get<0>(batch[i]), bad) != expect) {
                        bad.store(true, std::memory_order_relaxed);
                    }
                    ++expect;
                    recv.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
        go.store(true, std::memory_order_release);
        for (std::uint64_t s = 0; s < N; ++s) {
            while (in.is_full()) {
                std::this_thread::yield();
            }
            auto b = make_mutable<std::uint64_t>(PAYLOAD);
            fill(b, s);
            out.send_data(std::move(b), timestamp{});
        }
        cons.join();
        if (bad.load()) {
            std::puts("FAIL: corruption/reorder via get_batch (C)");
            return 1;
        }
        if (recv.load() != N) {
            std::puts("FAIL: get_batch lost packets (C)");
            return 1;
        }
        std::printf("SPSC get_batch (throttled) OK: %llu packets exact-order via batched drain\n",
                    (unsigned long long)N);
    }

    // ---- D) producer send_batch(): exact FIFO via one-shot batched enqueue ----
    {
        output_port<mutable_buffer<std::uint64_t>> out{"o"};
        input_port<mutable_buffer<std::uint64_t>> in{"i", 256};
        if (!out.connect(&in)) {
            std::puts("FAIL: connect (D)");
            return 1;
        }

        std::atomic<bool> go{false}, bad{false};
        std::atomic<std::uint64_t> recv{0};
        std::thread cons([&] {
            while (!go.load(std::memory_order_acquire)) {
            }
            std::array<decltype(in)::queue_type, 64> batch;
            std::uint64_t expect = 0;
            while (expect < N) {
                const auto count = in.get_batch(std::span{batch});
                for (std::size_t i = 0; i < count; ++i) {
                    const auto& buf = std::get<0>(batch[i]);
                    if (read_seq(buf, bad) != expect) {
                        bad.store(true, std::memory_order_relaxed);
                    }
                    ++expect;
                    recv.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
        go.store(true, std::memory_order_release);
        constexpr std::size_t B = 32;
        for (std::uint64_t s = 0; s < N;) {
            const std::size_t k = static_cast<std::size_t>(std::min<std::uint64_t>(B, N - s));
            std::vector<mutable_buffer<std::uint64_t>> bufs;
            bufs.reserve(k);
            for (std::size_t i = 0; i < k; ++i) {
                auto b = make_mutable<std::uint64_t>(PAYLOAD);
                fill(b, s + i);
                bufs.push_back(std::move(b));
            }
            while (in.available_capacity() < k) {
                std::this_thread::yield();
            } // room -> no drops
            out.send_batch(std::span<mutable_buffer<std::uint64_t>>(bufs), timestamp{});
            s += k;
        }
        cons.join();
        if (bad.load()) {
            std::puts("FAIL: send_batch reorder/corruption (D)");
            return 1;
        }
        if (recv.load() != N) {
            std::puts("FAIL: send_batch lost packets (D)");
            return 1;
        }
        std::printf("SPSC send_batch+get_batch OK: %llu packets exact-order through both batch ends\n",
                    (unsigned long long)N);
    }

    // ---- E) Regression: raising depth() above the ring capacity on a
    //         NON-EMPTY ring must NOT let the producer overwrite unread slots.
    //         (Deterministic; producer then consumer, driven from this thread.)
    {
        output_port<mutable_buffer<std::uint64_t>> out{"o"};
        input_port<mutable_buffer<std::uint64_t>> in{"i", 4}; // physical ring capacity 4
        if (!out.connect(&in)) {
            std::puts("FAIL: connect (E)");
            return 1;
        }
        for (std::uint64_t s = 0; s < 4; ++s) { // fill the ring
            auto b = make_mutable<std::uint64_t>(PAYLOAD);
            fill(b, s);
            out.send_data(std::move(b), timestamp{});
        }
        in.depth(8);                            // raise soft limit (ring stays 4: non-empty)
        for (std::uint64_t s = 4; s < 8; ++s) { // must be DROPPED, not overwrite 0..3
            auto b = make_mutable<std::uint64_t>(PAYLOAD);
            fill(b, s);
            out.send_data(std::move(b), timestamp{});
        }
        for (std::uint64_t expect = 0; expect < 4; ++expect) { // original 4 intact, in order
            auto [buf, ts, md] = in.get_data();
            if (buf.size() != PAYLOAD || buf[0] != expect) {
                std::printf("FAIL (E): got %s seq %llu, expected %llu (unread slot overwritten)\n",
                            buf.size() == 0 ? "empty" : "value", (unsigned long long)(buf.size() ? buf[0] : 0),
                            (unsigned long long)expect);
                return 1;
            }
        }
        auto [tail_buf, tts, tmd] = in.get_data();
        if (tail_buf.size() != 0) {
            std::puts("FAIL (E): ring not empty after draining 4 (phantom data)");
            return 1;
        }
        std::puts("SPSC depth-clamp OK: depth>capacity dropped overflow; unread slots intact");
    }

    return 0;
}
