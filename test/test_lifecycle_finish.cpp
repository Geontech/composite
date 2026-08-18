// Lifecycle — completion semantics: on_finished(reason), is_finished()/finished_reason(),
// component::wait_until_finished(), application::wait_until_finished(), and the finished/finish_reason
// fields in property_state(). Verifies: FINISH -> completed; a throw -> error; an external stop() is
// NOT "finished"; restart clears the status; the app-level join waits for every component.
#include "composite/core/application.hpp"
#include "composite/core/component.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace composite;
using namespace std::chrono_literals;
using json = composite::properties::json;
using composite::properties::config_type;

namespace {
int g_failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

// FINISHes on the Nth process() call, or throws there if `do_throw`.
class finisher : public component {
public:
    finisher(std::string_view id, int finish_after, bool do_throw)
        : component(id), m_finish_after(finish_after), m_throw(do_throw) {}
    auto process() -> retval override {
        if (++m_calls >= m_finish_after) {
            if (m_throw) {
                throw std::runtime_error("boom");
            }
            return retval::FINISH;
        }
        return retval::NORMAL; // spin toward the finish (no idle)
    }
    auto on_finished(finish_reason r) -> void override {
        m_on_finished_reason.store(r, std::memory_order_relaxed);
        m_on_finished_calls.fetch_add(1, std::memory_order_relaxed);
    }
    std::atomic<int> m_calls{0};
    int m_finish_after;
    bool m_throw;
    std::atomic<finish_reason> m_on_finished_reason{finish_reason::none};
    std::atomic<int> m_on_finished_calls{0};
    component::auto_stop m_auto_stop{*this};
};

// Never self-finishes; idles on NOOP until stopped.
class forever : public component {
public:
    explicit forever(std::string_view id) : component(id) {}
    auto process() -> retval override { return retval::NOOP; }
    component::auto_stop m_auto_stop{*this};
};

// Throws on its first `throws_before_ok` process() calls, then returns NOOP (recovered). With an
// error-restart policy the worker retries past the throws; without one, it finishes on the first.
class flaky : public component {
public:
    flaky(std::string_view id, int throws_before_ok) : component(id), m_throws(throws_before_ok) {}
    auto process() -> retval override {
        if (m_calls.fetch_add(1, std::memory_order_relaxed) < m_throws) {
            throw std::runtime_error("transient");
        }
        return retval::NOOP; // recovered -> idle
    }
    std::atomic<int> m_calls{0};
    int m_throws;
    component::auto_stop m_auto_stop{*this};
};

// on_worker_start() throws (models a pipeline_component whose pool fails to start). start() must
// propagate the throw AND leave wait_until_finished() non-blocking (no stranded m_worker_done).
class bad_start : public component {
public:
    explicit bad_start(std::string_view id) : component(id) {}
    auto process() -> retval override { return retval::NOOP; }
    auto on_worker_start() -> void override { throw std::runtime_error("worker resource init failed"); }
    component::auto_stop m_auto_stop{*this};
};

// Models pipeline_component's real failure mode: on_worker_start() spawns pool threads then
// throws PARTWAY (a std::thread ctor hitting resource exhaustion). Without the fix the throw skips
// on_worker_stop(), so (a) the already-spawned threads leak and (b) a RETRY's m_pool.clear() destroys
// still-joinable threads -> std::terminate. With the fix the throw path reaps the partial pool, so a
// retry is safe. Note the m_pool.clear() at the top of on_worker_start() — that is exactly the line
// that aborts on retry if a prior partial pool was not joined.
class pool_bad_start : public component {
public:
    explicit pool_bad_start(std::string_view id) : component(id) {}
    auto process() -> retval override { return retval::NOOP; }
    auto on_worker_start() -> void override {
        m_pool.clear(); // <-- std::terminate on retry if a prior partial pool was left joinable
        m_stop.store(false, std::memory_order_release);
        for (int i = 0; i < 3; ++i) {
            m_pool.emplace_back([this] {
                while (!m_stop.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(1ms);
                }
            });
        }
        if (m_fail.load(std::memory_order_acquire)) {
            throw std::runtime_error("pool init failed partway"); // partial pool now up
        }
    }
    auto on_worker_stop() -> void override {
        m_stop.store(true, std::memory_order_release);
        for (auto& t : m_pool) {
            if (t.joinable()) {
                t.join();
            }
        }
        m_pool.clear();
        m_stops.fetch_add(1, std::memory_order_relaxed);
    }
    std::atomic<bool> m_fail{true};
    std::atomic<bool> m_stop{false};
    std::atomic<int> m_stops{0};
    std::vector<std::thread> m_pool;
    component::auto_stop m_auto_stop{*this};
};
} // namespace

int main() {
    // ---- FINISH -> completed ----
    {
        auto c = std::make_shared<finisher>("done", 1, /*throw=*/false);
        c->start();
        check(c->wait_until_finished(10s), "completed: wait_until_finished returns");
        check(c->is_finished(), "completed: is_finished");
        check(c->finished_reason() == finish_reason::completed, "completed: reason");
        check(c->m_on_finished_calls.load() == 1, "completed: on_finished fired exactly once");
        check(c->m_on_finished_reason.load() == finish_reason::completed, "completed: on_finished reason");
        check(!c->is_running(), "completed: not running after finish");
        auto st = c->property_state();
        check(st.value("finished", false) == true, "completed: property_state finished");
        check(st.value("finish_reason", std::string{}) == "completed", "completed: property_state reason");
    }

    // ---- throw -> error ----
    {
        auto c = std::make_shared<finisher>("err", 1, /*throw=*/true);
        c->start();
        check(c->wait_until_finished(10s), "error: wait returns");
        check(c->finished_reason() == finish_reason::error, "error: reason");
        check(c->m_on_finished_reason.load() == finish_reason::error, "error: on_finished reason");
        check(c->property_state().value("finish_reason", std::string{}) == "error", "error: property_state reason");
    }

    // ---- external stop() is NOT 'finished' ----
    {
        auto c = std::make_shared<forever>("run");
        c->start();
        for (int i = 0; i < 200 && !c->is_running(); ++i) {
            std::this_thread::sleep_for(1ms);
        }
        check(c->is_running(), "forever: running");
        check(!c->is_finished(), "forever: not finished while running");
        c->stop();
        check(c->wait_until_finished(5s), "forever: wait returns after stop");
        check(!c->is_finished(), "forever: an external stop is not 'finished'");
        check(c->finished_reason() == finish_reason::none, "forever: reason none after external stop");
        check(c->property_state().value("finished", true) == false, "forever: property_state not finished");
    }

    // ---- restart clears the finished status, then finishes again ----
    {
        auto c = std::make_shared<finisher>("re", 1, /*throw=*/false);
        c->start();
        check(c->wait_until_finished(10s) && c->is_finished(), "restart: finished after first run");
        const int first_count = c->m_on_finished_calls.load();
        c->start(); // start_locked clears finish status and restarts the worker
        check(c->wait_until_finished(10s), "restart: second run finishes");
        check(c->m_on_finished_calls.load() == first_count + 1, "restart: on_finished fired again (status was reset)");
    }

    // ---- application::wait_until_finished waits for every component ----
    {
        application app{"batch"};
        app.add_component(std::make_shared<finisher>("f1", 1, false));
        app.add_component(std::make_shared<finisher>("f2", 3, false));
        app.start();
        check(app.wait_until_finished(10s), "app: all components finished");
        for (auto& c : app.components()) {
            check(c->is_finished(), "app: each component finished");
        }
    }

    // ---- error_policy = restart-with-backoff: recovers from transient errors ----
    {
        auto c = std::make_shared<flaky>("recover", /*throws=*/2);
        c->set_properties(json{{"error_restart_max", 5}, {"error_restart_backoff_ms", 1}}, config_type::INITIALIZE);
        c->start();
        // Poll until it gets PAST the throws (recovered) or time out.
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (c->m_calls.load() <= 2 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(2ms);
        }
        check(c->m_calls.load() > 2, "restart-backoff: recovered past the transient throws");
        check(c->is_running(), "restart-backoff: still running after recovery");
        check(!c->is_finished(), "restart-backoff: did NOT finish (retries absorbed the errors)");
        c->stop();
    }

    // ---- error_policy = restart-with-backoff: gives up after max consecutive failures ----
    {
        auto c = std::make_shared<flaky>("giveup", /*throws=*/1000); // always throws
        c->set_properties(json{{"error_restart_max", 2}, {"error_restart_backoff_ms", 1}}, config_type::INITIALIZE);
        c->start();
        check(c->wait_until_finished(10s), "restart-backoff giveup: finishes after exhausting retries");
        check(c->finished_reason() == finish_reason::error, "restart-backoff giveup: reason=error");
        // 1 initial failure + 2 retries = 3 process() calls before giving up.
        check(c->m_calls.load() == 3, "restart-backoff giveup: exactly max+1 attempts");
    }

    // ---- REGRESSION: restart resets the consecutive-error counter (no early give-up) ----
    {
        auto c = std::make_shared<flaky>("errreset", /*throws=*/1000); // always throws
        c->set_properties(json{{"error_restart_max", 2}, {"error_restart_backoff_ms", 1}}, config_type::INITIALIZE);
        c->start();
        check(c->wait_until_finished(10s), "err-reset: run 1 gave up");
        check(c->m_calls.load() == 3, "err-reset: run 1 = max+1 (3) attempts");
        c->start(); // restart MUST reset m_error_restarts, else run 2 gives up on the first throw
        check(c->wait_until_finished(10s), "err-reset: run 2 gave up");
        check(c->m_calls.load() == 6, "err-reset: run 2 got a fresh 3 attempts (counter reset on restart)");
    }

    // ---- REGRESSION: a throwing on_worker_start() must not hang wait_until_finished() ----
    {
        auto c = std::make_shared<bad_start>("badstart");
        bool threw = false;
        try {
            c->start();
        } catch (...) {
            threw = true;
        }
        check(threw, "onstart-throw: start() propagated the exception");
        check(!c->is_running(), "onstart-throw: not running (no worker spawned)");
        // With the bug (m_worker_done stranded false) this bounded wait returns false / hangs.
        check(c->wait_until_finished(2s), "onstart-throw: wait_until_finished returns immediately (no hang)");
    }

    // ---- REGRESSION: a throwing on_worker_start() reaps its PARTIAL resources; retry is safe ----
    {
        auto c = std::make_shared<pool_bad_start>("poolbadstart");
        bool threw = false;
        try {
            c->start();
        } catch (...) {
            threw = true;
        }
        check(threw, "partial-start reap: first start() propagated the on_worker_start throw");
        check(c->m_stops.load() == 1,
              "partial-start reap: on_worker_stop ran on the throw (partial pool reaped, not leaked)");
        check(!c->is_running(), "partial-start reap: not running after the failed start");
        check(c->finished_reason() == finish_reason::error,
              "partial-start reap: failed start reported as error (not 'none')");
        check(c->wait_until_finished(2s),
              "partial-start reap: wait_until_finished returns (m_worker_done not stranded)");
        // RETRY: with the leak, on_worker_start()'s m_pool.clear() would std::terminate on a joinable
        // prior pool. With the reap the vector is empty, so the (now succeeding) retry runs cleanly.
        c->m_fail.store(false, std::memory_order_release);
        bool threw2 = false;
        try {
            c->start();
        } catch (...) {
            threw2 = true;
        }
        check(!threw2, "partial-start reap: retry after a failed start did NOT throw / std::terminate");
        // is_running() becomes true once the worker registers with the park — NOT synchronous with
        // start() returning (the window is wide under a sanitizer's slower thread startup), so poll
        // like the other cases in this file rather than asserting immediately.
        for (int i = 0; i < 500 && !c->is_running(); ++i) {
            std::this_thread::sleep_for(1ms);
        }
        check(c->is_running(), "partial-start reap: retry started the worker");
        c->stop();
        check(c->m_stops.load() == 2,
              "partial-start reap: on_worker_stop ran exactly once per start (throw + stop), no double/zero");
    }

    if (g_failures) {
        std::printf("\n%d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::puts("LIFECYCLE FINISH OK: on_finished(reason), is_finished/finished_reason, "
              "component + application wait_until_finished, restart-clears-status, property_state");
    return 0;
}
