#include <composite/core/park.hpp>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using composite::park_coordinator;

// ---- watchdog: abort loudly on deadlock instead of hanging CI silently ----
static std::atomic_bool g_done{false};
static void arm_watchdog(int secs, const char* what) {
    std::thread([secs, what]{
        for (int i = 0; i < secs * 10; ++i) {
            if (g_done.load(std::memory_order_acquire)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::fprintf(stderr, "WATCHDOG: '%s' hung -> deadlock\n", what);
        std::abort();
    }).detach();
}

// Shared, PLAIN (non-atomic) data the worker reads with no lock and the writer
// mutates only while the worker is parked. A torn read or a missing happens-before
// edge shows up as a failed invariant (stress) or a TSan report.
struct sentinel { std::array<uint64_t, 64> pattern{}; uint64_t tag{}; };

// ---------- T-A: park-visibility sentinel ----------
static void test_visibility() {
    g_done.store(false); arm_watchdog(30, "T-A visibility");
    park_coordinator coord{"vis"};
    sentinel data;
    std::atomic_bool stop{false};
    std::atomic<uint64_t> torn{0}, mismatch{0}, reads{0};

    std::thread worker([&]{
        coord.worker_started();
        park_coordinator::exit_guard eg{coord};
        while (!stop.load(std::memory_order_acquire)) {
            coord.park_point();                 // park here if requested; see new data on return
            uint64_t p0 = data.pattern[0];      // lock-free reads
            for (uint64_t v : data.pattern) if (v != p0) torn.fetch_add(1, std::memory_order_relaxed);
            if (data.tag != p0) mismatch.fetch_add(1, std::memory_order_relaxed);
            reads.fetch_add(1, std::memory_order_relaxed);
        }
    });

    constexpr uint64_t N = 200000;
    for (uint64_t k = 1; k <= N; ++k) {
        coord.with_worker_parked([&]{ data.tag = k; for (auto& v : data.pattern) v = k; });
    }
    stop.store(true, std::memory_order_release);
    worker.join();
    g_done.store(true, std::memory_order_release);
    std::printf("T-A visibility: %llu parks, %llu reads, torn=%llu mismatch=%llu\n",
                (unsigned long long)N, (unsigned long long)reads.load(),
                (unsigned long long)torn.load(), (unsigned long long)mismatch.load());
    assert(torn.load() == 0 && mismatch.load() == 0);
}

// ---------- T-B: reentrancy (nested park) + concurrent REST reader ----------
static void test_reentrancy_and_reader() {
    g_done.store(false); arm_watchdog(30, "T-B reentrancy+reader");
    park_coordinator coord{"reentrant"};
    sentinel data; uint64_t tag2 = 0;
    std::atomic_bool stop{false};
    std::atomic<uint64_t> bad_reads{0};

    std::thread worker([&]{
        coord.worker_started();
        park_coordinator::exit_guard eg{coord};
        while (!stop.load(std::memory_order_acquire)) { coord.park_point(); volatile uint64_t t = data.tag; (void)t; }
    });
    // REST reader: shared lock; must never see a torn (tag, pattern[0]) pair.
    std::thread reader([&]{
        while (!stop.load(std::memory_order_acquire)) {
            coord.with_reader_lock([&]{
                uint64_t t = data.tag, p0 = data.pattern[0];
                if (t != 0 && t != p0) bad_reads.fetch_add(1, std::memory_order_relaxed);
            });
        }
    });

    for (uint64_t k = 1; k <= 100000; ++k) {
        coord.with_worker_parked([&]{
            data.tag = k; for (auto& v : data.pattern) v = k;
            // NESTED park on the same thread -> must run inline via the park-owner key,
            // not deadlock on m_data_mtx and not re-drive the state machine.
            coord.with_worker_parked([&]{ tag2 = k; });
        });
        assert(tag2 == k);
    }
    stop.store(true, std::memory_order_release);
    worker.join(); reader.join();
    g_done.store(true, std::memory_order_release);
    std::printf("T-B reentrancy+reader: nested parks OK, reader bad_reads=%llu\n",
                (unsigned long long)bad_reads.load());
    assert(bad_reads.load() == 0);
}

// ---------- T-C: park-vs-stop/start churn ----------
static void test_park_vs_stop() {
    g_done.store(false); arm_watchdog(40, "T-C park-vs-stop");
    park_coordinator coord{"churn"};
    coord.set_timeout(std::chrono::milliseconds(500));
    sentinel data;
    std::atomic_bool worker_stop{false}, all_stop{false};
    std::atomic<uint64_t> applied{0}, threw{0};

    auto worker_fn = [&]{
        coord.worker_started();
        park_coordinator::exit_guard eg{coord};
        while (!worker_stop.load(std::memory_order_acquire)) { coord.park_point(); volatile uint64_t t = data.tag; (void)t; }
    };
    std::thread worker(worker_fn);

    // Writers driving parks throughout the churn.
    std::vector<std::thread> writers;
    for (int w = 0; w < 3; ++w) writers.emplace_back([&]{
        uint64_t k = 0;
        while (!all_stop.load(std::memory_order_acquire)) {
            try { coord.with_worker_parked([&]{ data.tag = ++k; }); applied.fetch_add(1, std::memory_order_relaxed); }
            catch (const std::exception&) { threw.fetch_add(1, std::memory_order_relaxed); }
        }
    });

    // Lifecycle churn: stop+join+settle+restart, while writers keep parking.
    for (int round = 0; round < 300; ++round) {
        worker_stop.store(true, std::memory_order_release);
        coord.cancel_waiters();              // release any writer blocked waiting to park
        worker.join();                        // worker exits -> EXITING
        coord.settle_stopped();               // -> NO_WORKER
        worker_stop.store(false, std::memory_order_release);
        worker = std::thread(worker_fn);      // -> worker_started -> RUNNING
    }
    all_stop.store(true, std::memory_order_release);
    for (auto& t : writers) t.join();
    worker_stop.store(true, std::memory_order_release);
    coord.cancel_waiters();
    worker.join();
    g_done.store(true, std::memory_order_release);
    std::printf("T-C park-vs-stop: 300 churn rounds, applied=%llu threw=%llu (no deadlock)\n",
                (unsigned long long)applied.load(), (unsigned long long)threw.load());
    // Contract: a park write racing a worker stop/restart takes the inline path
    // (worker is EXITING/NO_WORKER, not read-active) — it must NOT throw. A
    // non-zero count means a park request was lost and the writer hit the bounded
    // timeout spuriously. Explicit check (not assert(): NDEBUG would void it).
    if (threw.load() != 0) {
        std::fprintf(stderr, "T-C FAIL: %llu park writes spuriously threw (expected 0)\n",
                     (unsigned long long)threw.load());
        std::abort();
    }
}


// ---------- A throwing fn (bad property value) must NOT leave the worker PARKED ----------
static void test_throw_resumes() {
    g_done.store(false); arm_watchdog(20, "H9 throw-resumes");
    park_coordinator coord{"throw"};
    coord.set_timeout(std::chrono::milliseconds(500));
    sentinel data; std::atomic_bool stop{false}; std::atomic<uint64_t> iters{0};
    std::thread worker([&]{
        coord.worker_started();
        park_coordinator::exit_guard eg{coord};
        while (!stop.load(std::memory_order_acquire)) { coord.park_point(); iters.fetch_add(1, std::memory_order_relaxed); }
    });
    for (int k = 0; k < 5000; ++k) {
        bool caught = false;
        try { coord.with_worker_parked([&]{ data.tag = 1; throw std::runtime_error("bad value"); }); }
        catch (const std::exception&) { caught = true; }
        assert(caught);
        // If the worker were stuck PARKED, this normal write would time out and throw.
        coord.with_worker_parked([&]{ data.tag = 2; });
        assert(data.tag == 2);
    }
    stop.store(true, std::memory_order_release);
    coord.cancel_waiters();
    worker.join();
    g_done.store(true, std::memory_order_release);
    std::printf("H9 throw-resumes: 5000 throwing writes, worker stayed live (iters=%llu)\n",
                (unsigned long long)iters.load());
}

int main() {
    test_visibility();
    test_reentrancy_and_reader();
    test_park_vs_stop();
    test_throw_resumes();
    std::printf("ALL PARK TESTS PASSED\n");
    return 0;
}
