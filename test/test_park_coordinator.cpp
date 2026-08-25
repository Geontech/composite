#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <composite/core/park.hpp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stop_token>
#include <thread>
#include <vector>

using composite::park_coordinator;

// ---- watchdog: abort loudly on deadlock instead of hanging CI silently ----
//
// PROGRESS-BASED, not wall-clock. A deadlock is "stopped making progress", not "took a long time",
// and the two are different things here: these cases are timing-sensitive by design, and under a
// sanitizer the worker thread can win the scheduler badly enough that the writer's park handshake
// crawls — one observed failure completed all 200k parks having spun 4.5M reader iterations against
// ~300k on a normal run. A wall-clock bound turned that into a "deadlock" report roughly 1 in 25
// runs, measured, with and without any of the recent park changes.
//
// So the watchdog now trips only if the phase's progress counter has not advanced for `stall_secs`.
// A real deadlock still trips it immediately (progress stops dead); a merely slow, starved run does
// not, because it keeps advancing. Raising the wall-clock number instead would have traded a false
// positive for a weaker guard — this keeps the guard sharp and removes the false positive.
static std::atomic<unsigned long long> g_progress{0};

/// SCOPED, and owning its own thread. The previous version detached the thread and shared one
/// global "done" flag across every phase, so a watchdog could miss the brief window in which that
/// flag was true between phases and then keep running against the NEXT phase — with the previous
/// phase's timeout and label. One stop flag per watchdog, joined at the phase boundary, makes that
/// structurally impossible.
class watchdog {
public:
    watchdog(int stall_secs, const char* what) {
        g_progress.store(0, std::memory_order_release);
        m_thread = std::thread([this, stall_secs, what] {
            auto last = g_progress.load(std::memory_order_acquire);
            int quiet_ticks = 0;
            constexpr int k_ticks_per_sec = 10;
            while (!m_done.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000 / k_ticks_per_sec));
                const auto now = g_progress.load(std::memory_order_acquire);
                if (now != last) {
                    last = now;
                    quiet_ticks = 0;
                    continue;
                }
                if (++quiet_ticks >= stall_secs * k_ticks_per_sec) {
                    std::fprintf(stderr, "WATCHDOG: '%s' made no progress for %ds -> deadlock\n", what, stall_secs);
                    std::abort();
                }
            }
        });
    }
    watchdog(const watchdog&) = delete;
    auto operator=(const watchdog&) -> watchdog& = delete;
    watchdog(watchdog&&) = delete;
    auto operator=(watchdog&&) -> watchdog& = delete;
    ~watchdog() {
        m_done.store(true, std::memory_order_release);
        if (m_thread.joinable()) {
            m_thread.join(); // no watchdog outlives the phase that armed it
        }
    }

private:
    std::atomic_bool m_done{false};
    std::thread m_thread;
};

/// Called by each phase's hot loop so the watchdog can tell "slow" from "stuck".
static inline void watchdog_progress() {
    g_progress.fetch_add(1, std::memory_order_relaxed);
}

// Shared, PLAIN (non-atomic) data the worker reads with no lock and the writer
// mutates only while the worker is parked. A torn read or a missing happens-before
// edge shows up as a failed invariant (stress) or a TSan report.
struct sentinel {
    std::array<uint64_t, 64> pattern{};
    uint64_t tag{};
};

// ---------- T-A: park-visibility sentinel ----------
static void test_visibility() {
    const watchdog wd{30, "T-A visibility"};
    park_coordinator coord{"vis"};
    sentinel data;
    std::atomic_bool stop{false};
    std::atomic<uint64_t> torn{0}, mismatch{0}, reads{0};

    std::thread worker([&] {
        coord.worker_started();
        park_coordinator::exit_guard eg{coord};
        while (!stop.load(std::memory_order_acquire)) {
            coord.park_point();            // park here if requested; see new data on return
            uint64_t p0 = data.pattern[0]; // lock-free reads
            for (uint64_t v : data.pattern)
                if (v != p0) torn.fetch_add(1, std::memory_order_relaxed);
            if (data.tag != p0) mismatch.fetch_add(1, std::memory_order_relaxed);
            reads.fetch_add(1, std::memory_order_relaxed);
        }
    });

    constexpr uint64_t N = 200000;
    for (uint64_t k = 1; k <= N; ++k) {
        coord.with_worker_parked([&] {
            data.tag = k;
            for (auto& v : data.pattern)
                v = k;
        });
        watchdog_progress(); // a starved-but-advancing writer is not a deadlock
    }
    stop.store(true, std::memory_order_release);
    worker.join();
    std::printf("T-A visibility: %llu parks, %llu reads, torn=%llu mismatch=%llu\n", (unsigned long long)N,
                (unsigned long long)reads.load(), (unsigned long long)torn.load(), (unsigned long long)mismatch.load());
    assert(torn.load() == 0 && mismatch.load() == 0);
}

// ---------- T-B: reentrancy (nested park) + concurrent REST reader ----------
static void test_reentrancy_and_reader() {
    const watchdog wd{30, "T-B reentrancy+reader"};
    park_coordinator coord{"reentrant"};
    sentinel data;
    uint64_t tag2 = 0;
    std::atomic_bool stop{false};
    std::atomic<uint64_t> bad_reads{0};

    std::thread worker([&] {
        coord.worker_started();
        park_coordinator::exit_guard eg{coord};
        while (!stop.load(std::memory_order_acquire)) {
            coord.park_point();
            volatile uint64_t t = data.tag;
            (void)t;
        }
    });
    // REST reader: shared lock; must never see a torn (tag, pattern[0]) pair.
    std::thread reader([&] {
        while (!stop.load(std::memory_order_acquire)) {
            coord.with_reader_lock([&] {
                uint64_t t = data.tag, p0 = data.pattern[0];
                if (t != 0 && t != p0) bad_reads.fetch_add(1, std::memory_order_relaxed);
            });
        }
    });

    for (uint64_t k = 1; k <= 100000; ++k) {
        coord.with_worker_parked([&] {
            data.tag = k;
            for (auto& v : data.pattern)
                v = k;
            // NESTED park on the same thread -> must run inline via the park-owner key,
            // not deadlock on m_data_mtx and not re-drive the state machine.
            coord.with_worker_parked([&] { tag2 = k; });
        });
        assert(tag2 == k);
        watchdog_progress();
    }
    stop.store(true, std::memory_order_release);
    worker.join();
    reader.join();
    std::printf("T-B reentrancy+reader: nested parks OK, reader bad_reads=%llu\n",
                (unsigned long long)bad_reads.load());
    assert(bad_reads.load() == 0);
}

// ---------- T-C: park-vs-stop/start churn ----------
static void test_park_vs_stop() {
    const watchdog wd{40, "T-C park-vs-stop"};
    park_coordinator coord{"churn"};
    coord.set_timeout(std::chrono::milliseconds(500));
    sentinel data;
    std::atomic_bool worker_stop{false}, all_stop{false};
    std::atomic<uint64_t> applied{0}, threw{0};

    auto worker_fn = [&] {
        coord.worker_started();
        park_coordinator::exit_guard eg{coord};
        while (!worker_stop.load(std::memory_order_acquire)) {
            coord.park_point();
            volatile uint64_t t = data.tag;
            (void)t;
        }
    };
    std::thread worker(worker_fn);

    // Writers driving parks throughout the churn.
    std::vector<std::thread> writers;
    for (int w = 0; w < 3; ++w)
        writers.emplace_back([&] {
            uint64_t k = 0;
            while (!all_stop.load(std::memory_order_acquire)) {
                try {
                    coord.with_worker_parked([&] { data.tag = ++k; });
                    applied.fetch_add(1, std::memory_order_relaxed);
                } catch (const std::exception&) {
                    threw.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });

    // Lifecycle churn: stop+join+settle+restart, while writers keep parking.
    for (int round = 0; round < 300; ++round) {
        worker_stop.store(true, std::memory_order_release);
        coord.cancel_waiters(); // release any writer blocked waiting to park
        worker.join();          // worker exits -> EXITING
        coord.settle_stopped(); // -> NO_WORKER
        worker_stop.store(false, std::memory_order_release);
        worker = std::thread(worker_fn); // -> worker_started -> RUNNING
        watchdog_progress();
    }
    all_stop.store(true, std::memory_order_release);
    for (auto& t : writers)
        t.join();
    worker_stop.store(true, std::memory_order_release);
    coord.cancel_waiters();
    worker.join();
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
    const watchdog wd{20, "H9 throw-resumes"};
    park_coordinator coord{"throw"};
    coord.set_timeout(std::chrono::milliseconds(500));
    sentinel data;
    std::atomic_bool stop{false};
    std::atomic<uint64_t> iters{0};
    std::thread worker([&] {
        coord.worker_started();
        park_coordinator::exit_guard eg{coord};
        while (!stop.load(std::memory_order_acquire)) {
            coord.park_point();
            iters.fetch_add(1, std::memory_order_relaxed);
        }
    });
    for (int k = 0; k < 5000; ++k) {
        bool caught = false;
        try {
            coord.with_worker_parked([&] {
                data.tag = 1;
                throw std::runtime_error("bad value");
            });
        } catch (const std::exception&) {
            caught = true;
        }
        assert(caught);
        // If the worker were stuck PARKED, this normal write would time out and throw.
        coord.with_worker_parked([&] { data.tag = 2; });
        assert(data.tag == 2);
        watchdog_progress();
    }
    stop.store(true, std::memory_order_release);
    coord.cancel_waiters();
    worker.join();
    std::printf("H9 throw-resumes: 5000 throwing writes, worker stayed live (iters=%llu)\n",
                (unsigned long long)iters.load());
}

// ---------- 0.5.1 regression: a stale data signal must not truncate wait_backoff ----------
//
// The error-restart backoff waits on the park CV. signal_data() can land in the window between
// a producer's armed-load and the worker's unconditional disarm; that leaves m_data_pending set
// with nobody consuming it. wait_for_data() is satisfied immediately by the stale flag — so a
// backoff served through it collapses to zero. wait_backoff() must serve the full delay (data
// never wakes it) while still waking promptly for stop/park.
static void test_backoff_ignores_stale_data() {
    using namespace std::chrono;
    const watchdog wd{20, "B1 backoff-stale-data"};
    park_coordinator coord{"backoff"};

    // Plant the stale flag exactly as the race does: signal while armed, then disarm unconsumed.
    const auto plant_stale_flag = [&coord] {
        coord.arm_doorbell();
        coord.signal_data();
        coord.disarm_doorbell();
    };

    // The data-honoring wait IS satisfied immediately by the stale flag — this is the old
    // backoff behavior, kept as a baseline so this test documents the difference it pins.
    plant_stale_flag();
    std::stop_source unset;
    const auto t0 = steady_clock::now();
    coord.wait_for_data(milliseconds(500), unset.get_token());
    const auto data_wait = duration_cast<milliseconds>(steady_clock::now() - t0);
    if (data_wait >= milliseconds(400)) {
        std::fprintf(stderr, "B1 FAIL: stale flag did not satisfy wait_for_data (baseline broke: %lldms)\n",
                     static_cast<long long>(data_wait.count()));
        std::abort();
    }
    watchdog_progress();

    // The backoff wait must serve the full delay despite the same stale flag.
    plant_stale_flag();
    const auto t1 = steady_clock::now();
    coord.wait_backoff(milliseconds(150), unset.get_token());
    const auto served = duration_cast<milliseconds>(steady_clock::now() - t1);
    if (served < milliseconds(120)) {
        std::fprintf(stderr, "B1 FAIL: wait_backoff truncated by stale data flag (%lldms of 150ms)\n",
                     static_cast<long long>(served.count()));
        std::abort();
    }
    watchdog_progress();

    // ...and the stale flag was consumed: the next data wait times out instead of firing early.
    const auto t2 = steady_clock::now();
    coord.wait_for_data(milliseconds(150), unset.get_token());
    const auto after = duration_cast<milliseconds>(steady_clock::now() - t2);
    if (after < milliseconds(120)) {
        std::fprintf(stderr, "B1 FAIL: wait_backoff leaked the stale flag into the next wait (%lldms)\n",
                     static_cast<long long>(after.count()));
        std::abort();
    }
    watchdog_progress();

    // Stop still wakes the backoff promptly (cancel_waiters notifies the CV under the mutex).
    std::stop_source ss;
    std::thread stopper([&] {
        std::this_thread::sleep_for(milliseconds(30));
        ss.request_stop();
        coord.cancel_waiters();
    });
    const auto t3 = steady_clock::now();
    coord.wait_backoff(seconds(30), ss.get_token());
    stopper.join();
    const auto woke = duration_cast<milliseconds>(steady_clock::now() - t3);
    if (woke >= seconds(20)) {
        std::fprintf(stderr, "B1 FAIL: stop did not wake wait_backoff (%lldms)\n",
                     static_cast<long long>(woke.count()));
        std::abort();
    }
    std::printf("B1 backoff-stale-data: served=%lldms (>=120 required), stop woke in %lldms\n",
                static_cast<long long>(served.count()), static_cast<long long>(woke.count()));
}

int main() {
    test_visibility();
    test_reentrancy_and_reader();
    test_park_vs_stop();
    test_throw_resumes();
    test_backoff_ignores_stale_data();
    std::printf("ALL PARK TESTS PASSED\n");
    return 0;
}
