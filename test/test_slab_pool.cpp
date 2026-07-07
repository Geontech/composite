// Lock-free slab_pool free list (Treiber stack, index+tag, ABA-safe) under
// multi-threaded acquire/release. Verifies:
//   - no double-allocation: a buffer handed to one thread is never simultaneously
//     held by another (each thread stamps its id and re-checks it after a spin);
//   - no corruption / lost buffers: accounting returns to baseline (outstanding 0,
//     available == capacity) after all threads finish.
// Run under TSan (no data race on the lock-free list) and ASan/UBSan.
#include "composite/buffers/slab_pool.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

using namespace composite;

int main() {
    constexpr std::size_t CAP = 64;   // buffers in the pool
    constexpr std::size_t ELEMS = 16; // floats per buffer (slot >= 64B holds the link)
    constexpr int THREADS = 8;
    constexpr std::uint64_t PER_THREAD = 200000;

    auto pool = slab_pool<std::uint64_t>::create(ELEMS, CAP);
    std::atomic<bool> bad{false};
    std::atomic<std::uint64_t> total_ops{0};

    std::vector<std::thread> ts;
    for (int t = 0; t < THREADS; ++t) {
        ts.emplace_back([&, t] {
            const std::uint64_t id = static_cast<std::uint64_t>(t) + 1;
            std::uint64_t ops = 0;
            for (std::uint64_t i = 0; i < PER_THREAD; ++i) {
                auto buf = pool->acquire();
                if (!buf) {
                    continue;
                } // pool momentarily empty
                (*buf)[0] = id; // stamp ownership
                for (int s = 0; s < 4; ++s) {
                    asm volatile("" ::: "memory");
                } // brief hold (widens the race window without -Wvolatile)
                if ((*buf)[0] != id) {
                    bad.store(true, std::memory_order_relaxed);
                } // double-alloc!
                ++ops;
                // buf released here (lock-free push back to the pool)
            }
            total_ops.fetch_add(ops, std::memory_order_relaxed);
        });
    }
    for (auto& th : ts) {
        th.join();
    }

    if (bad.load()) {
        std::puts("FAIL: double-allocation / torn ownership stamp");
        return 1;
    }
    if (pool->outstanding() != 0) {
        std::printf("FAIL: outstanding=%zu after all released\n", pool->outstanding());
        return 1;
    }
    if (pool->available() != CAP) {
        std::printf("FAIL: available=%zu expected capacity=%zu (lost buffers)\n", pool->available(), CAP);
        return 1;
    }
    std::printf("SLAB_POOL lock-free OK: %d threads, %llu ops, outstanding=0, available=%zu/%zu\n", THREADS,
                (unsigned long long)total_ops.load(), pool->available(), CAP);
    return 0;
}
