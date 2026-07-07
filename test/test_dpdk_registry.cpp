// Unit + concurrency test for the EAL-free DPDK port_registry (the bookkeeping +
// locking extracted from the DPDK manager so the queue-allocation race fix
// is testable WITHOUT a NIC / libdpdk). Run under ThreadSanitizer: the concurrent
// allocate/release must show no data race and never hand the same queue to two
// owners simultaneously. Own main(); explicit checks (NDEBUG-safe).
#include <composite/dpdk/port_registry.hpp>

#include <array>
#include <atomic>
#include <cstdio>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

using composite::dpdk::port_registry;
using composite::dpdk::queue_lease;

int main() {
    int fails = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok) {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++fails;
        }
    };

    // ---- single-threaded correctness (incl. the duplicate-detection fixes) ----
    {
        port_registry r;
        check(r.register_port("eth0", 0, 4, nullptr), "register eth0");
        check(!r.register_port("eth0", 1, 4, nullptr), "duplicate interface rejected");
        check(!r.register_port("eth1", 0, 4, nullptr), "duplicate port_id rejected");
        check(r.register_port("eth1", 1, 2, nullptr), "register eth1");
        check(r.port_count() == 2, "port_count == 2");
        check(r.get_port_id_for_interface("eth0") == std::optional<uint16_t>(0), "port id of eth0");
        check(!r.get_port_id_for_interface("nope").has_value(), "unknown interface -> nullopt");
        check(r.is_port_configured(1) && !r.is_port_configured(9), "is_port_configured");
        check(r.register_mempool("mp", nullptr) && !r.register_mempool("mp", nullptr), "mempool dup rejected");

        check(r.is_queue_available("eth0", 0), "q0 available");
        check(r.allocate_queue("eth0", 0), "allocate q0");
        check(!r.allocate_queue("eth0", 0), "re-allocate q0 rejected");
        check(!r.is_queue_available("eth0", 0), "q0 no longer available");
        check(!r.allocate_queue("eth0", 9), "out-of-range queue rejected");
        check(r.release_queue("eth0", 0), "release q0");
        check(!r.release_queue("eth0", 0), "double-release rejected");

        check(r.allocate_next_available_queue("eth1") == std::optional<uint16_t>(0), "next -> q0");
        check(r.allocate_next_available_queue("eth1") == std::optional<uint16_t>(1), "next -> q1");
        check(!r.allocate_next_available_queue("eth1").has_value(), "exhausted -> nullopt");

        r.clear();
        check(r.port_count() == 0, "clear empties the registry");
    }

    // ---- concurrency: a queue is never owned by two threads at once ----
    {
        constexpr uint16_t QUEUES = 8;
        port_registry r;
        r.register_port("eth0", 0, QUEUES, nullptr);
        std::array<std::atomic<int>, QUEUES> owner;
        for (auto& o : owner) {
            o.store(-1, std::memory_order_relaxed);
        }
        std::atomic<bool> bad{false};
        std::atomic<bool> go{false};

        auto worker = [&](int id) {
            while (!go.load(std::memory_order_acquire)) {
            }
            for (int i = 0; i < 200000; ++i) {
                auto q = r.allocate_next_available_queue("eth0");
                if (!q) {
                    continue;
                } // all queues momentarily busy
                if (owner[*q].exchange(id, std::memory_order_acq_rel) != -1) {
                    bad.store(true, std::memory_order_relaxed); // someone else held it -> double alloc!
                }
                for (int s = 0; s < 4; ++s) {
                    asm volatile("" ::: "memory");
                } // brief hold (widen the window)
                if (owner[*q].load(std::memory_order_acquire) != id) {
                    bad.store(true, std::memory_order_relaxed);
                }
                owner[*q].store(-1, std::memory_order_release);
                if (!r.release_queue("eth0", *q)) {
                    bad.store(true, std::memory_order_relaxed); // we held it; release must succeed
                }
            }
        };

        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back(worker, t);
        }
        go.store(true, std::memory_order_release);
        for (auto& t : threads) {
            t.join();
        }
        check(!bad.load(), "no queue double-allocated under 8-thread contention");
    }

    // ---- queue_lease RAII: auto-release, move semantics, reset ----
    {
        port_registry r;
        r.register_port("eth0", 0, 2, nullptr);

        {
            auto lease = r.lease_next_available_queue("eth0");
            check(lease.valid() && lease.queue_id() == 0, "lease holds q0");
            check(!r.is_queue_available("eth0", 0), "q0 busy while leased");
        }
        check(r.is_queue_available("eth0", 0), "q0 freed when lease left scope");

        // move-construct transfers ownership; moved-from is empty and releases nothing
        {
            auto a = r.lease_queue("eth0", 1);
            check(a.valid(), "lease q1");
            queue_lease b{std::move(a)};
            check(b.valid() && b.queue_id() == 1, "ownership moved to b");
            check(!a.valid(), "moved-from lease is empty"); // NOLINT(bugprone-use-after-move)
            check(!r.is_queue_available("eth0", 1), "q1 still held by b after move");
        }
        check(r.is_queue_available("eth0", 1), "q1 freed when b left scope (single release)");

        // move-assign releases the lease being overwritten
        {
            auto held0 = r.lease_queue("eth0", 0);
            auto held1 = r.lease_queue("eth0", 1);
            check(!r.is_queue_available("eth0", 0) && !r.is_queue_available("eth0", 1), "both queues held");
            held0 = std::move(held1); // overwriting held0 must release q0
            check(r.is_queue_available("eth0", 0), "q0 released by move-assign");
            check(held0.valid() && held0.queue_id() == 1, "held0 now owns q1");
            check(!held1.valid(), "held1 emptied by move"); // NOLINT(bugprone-use-after-move)
        }
        check(r.is_queue_available("eth0", 1), "q1 freed at scope end");

        // explicit reset() is idempotent
        {
            auto lease = r.lease_queue("eth0", 0);
            check(lease.valid(), "lease q0");
            lease.reset();
            check(!lease.valid() && r.is_queue_available("eth0", 0), "reset releases q0");
            lease.reset(); // no double-release / crash
            check(r.is_queue_available("eth0", 0), "double reset is a no-op");
        }

        // a failed allocation yields a falsy lease that releases nothing
        {
            auto a = r.lease_queue("eth0", 9); // out of range
            check(!a.valid(), "out-of-range lease is falsy");
            auto b = r.lease_queue("nope", 0); // unknown interface
            check(!b.valid(), "unknown-interface lease is falsy");
        }
    }

    // ---- concurrency: RAII auto-release never double-allocates a queue ----
    {
        constexpr uint16_t QUEUES = 8;
        port_registry r;
        r.register_port("eth0", 0, QUEUES, nullptr);
        std::array<std::atomic<int>, QUEUES> owner;
        for (auto& o : owner) {
            o.store(-1, std::memory_order_relaxed);
        }
        std::atomic<bool> bad{false};
        std::atomic<bool> go{false};

        auto worker = [&](int id) {
            while (!go.load(std::memory_order_acquire)) {
            }
            for (int i = 0; i < 200000; ++i) {
                auto lease = r.lease_next_available_queue("eth0");
                if (!lease) {
                    continue;
                } // all queues momentarily busy
                const uint16_t q = lease.queue_id();
                if (owner[q].exchange(id, std::memory_order_acq_rel) != -1) {
                    bad.store(true, std::memory_order_relaxed); // double alloc!
                }
                for (int s = 0; s < 4; ++s) {
                    asm volatile("" ::: "memory");
                } // brief hold
                if (owner[q].load(std::memory_order_acquire) != id) {
                    bad.store(true, std::memory_order_relaxed);
                }
                owner[q].store(-1, std::memory_order_release);
                // lease destructor releases the queue here (RAII)
            }
        };

        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t) {
            threads.emplace_back(worker, t);
        }
        go.store(true, std::memory_order_release);
        for (auto& t : threads) {
            t.join();
        }
        check(!bad.load(), "no queue double-allocated via leases under 8-thread contention");
        for (uint16_t q = 0; q < QUEUES; ++q) {
            check(r.is_queue_available("eth0", q), "every queue freed by lease RAII at the end");
        }
    }

    if (fails != 0) {
        std::fprintf(stderr, "%d port_registry check(s) FAILED\n", fails);
        return 1;
    }
    std::puts("DPDK PORT_REGISTRY TESTS PASSED");
    return 0;
}
