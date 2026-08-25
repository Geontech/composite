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

namespace composite {
// Friend seam declared in slab_pool.hpp: release() is deliberately private (deleter-only), and
// its refusal path is unreachable through the public API — the pool's own deleters always hand
// back the pointer they were built with. This reaches it so the always-on validation is testable.
struct slab_pool_test_access {
    template <typename T>
    static void release(slab_pool<T>& pool, T* ptr) {
        pool.release(ptr);
    }
};
} // namespace composite

// 0.5.1 regression: release() validation must be ALWAYS on (it used to be assert-only, so a
// release build pushed a garbage index and corrupted the free stack). An invalid pointer is
// refused — counted in invalid_releases(), accounting untouched, free list intact.
static int test_invalid_release() {
    constexpr std::size_t ELEMS = 16;
    constexpr std::size_t CAP = 4;
    auto pool = slab_pool<std::uint64_t>::create(ELEMS, CAP);

    auto held = pool->acquire();
    if (!held || pool->outstanding() != 1) {
        std::puts("FAIL(invalid_release): setup acquire");
        return 1;
    }

    std::uint64_t foreign = 0;
    slab_pool_test_access::release(*pool, &foreign);          // not from this pool
    slab_pool_test_access::release(*pool, held->data() + 1);  // in-slab but stride-misaligned

    if (pool->invalid_releases() != 2) {
        std::printf("FAIL(invalid_release): invalid_releases=%zu expected 2\n", pool->invalid_releases());
        return 1;
    }
    if (pool->outstanding() != 1 || pool->available() != CAP - 1) {
        std::printf("FAIL(invalid_release): refusal touched accounting (outstanding=%zu available=%zu)\n",
                    pool->outstanding(), pool->available());
        return 1;
    }

    held.reset(); // the genuine release path still works after refusals
    if (pool->outstanding() != 0 || pool->available() != CAP) {
        std::printf("FAIL(invalid_release): valid release broken (outstanding=%zu available=%zu)\n",
                    pool->outstanding(), pool->available());
        return 1;
    }

    // Free list uncorrupted: every slot is still individually acquirable, then exhaustion is clean.
    std::vector<std::optional<external_buffer<std::uint64_t>>> all;
    for (std::size_t i = 0; i < CAP; ++i) {
        all.emplace_back(pool->acquire());
        if (!all.back()) {
            std::printf("FAIL(invalid_release): free list lost slot %zu of %zu\n", i, CAP);
            return 1;
        }
    }
    if (pool->acquire()) {
        std::puts("FAIL(invalid_release): pool over-provisioned after refusals (corrupt free list)");
        return 1;
    }
    std::printf("INVALID-RELEASE OK: 2 refusals counted, accounting intact, %zu/%zu slots live\n", CAP, CAP);
    return 0;
}

int main() {
    if (const int rc = test_invalid_release(); rc != 0) {
        return rc;
    }
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
