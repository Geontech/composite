// Lifecycle — completion semantics: on_finished(reason), is_finished()/finished_reason(),
// component::wait_until_finished(), application::wait_until_finished(), and the finished/finish_reason
// fields in property_state(). Verifies: FINISH -> completed; a throw -> error; an external stop() is
// NOT "finished"; restart clears the status; the app-level join waits for every component.
#include "composite/buffers/buffer.hpp"
#include "composite/core/application.hpp"
#include "composite/core/component.hpp"
#include "composite/core/pipeline_component.hpp"
#include "composite/metrics/registry.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
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

// FINISHes on its first process() call; on_finished() sleeps to hold the completion tail open,
// so a concurrent property write can be raced against it. Records is_running() at entry (must be
// true: the park coordinator must not have published EXITING before the tail runs) and the instant
// it returns (a racing write must not return before this).
class slow_finisher : public component {
public:
    explicit slow_finisher(std::string_view id) : component(id) {}
    auto process() -> retval override { return retval::FINISH; }
    auto on_finished(finish_reason) -> void override {
        m_running_at_entry.store(is_running(), std::memory_order_relaxed);
        m_tail_started.store(true, std::memory_order_release); // let a waiting writer race the sleep below
        std::this_thread::sleep_for(200ms);
        m_finished_ns.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_release);
    }
    std::atomic<bool> m_running_at_entry{false};
    std::atomic<bool> m_tail_started{false};
    std::atomic<long long> m_finished_ns{0};
    component::auto_stop m_auto_stop{*this};
};

// pipeline_component subclass whose finalize() (main worker thread, submission order) disables
// itself via a RUNTIME `enabled` write after its first packet -- the pipeline_component analogue of
// a worker self-write, and the shape most likely to leak a pool if disabling doesn't reap it.
class self_disabling_pipeline : public pipeline_component<mutable_buffer<int>, mutable_buffer<int>> {
public:
    explicit self_disabling_pipeline(std::string_view id, int workers)
        : pipeline_component(id, "in", "out", workers) {}

protected:
    auto work(in_t in, timestamp, const composite::metadata&) -> out_t override { return in; }
    auto finalize(out_t&, timestamp, const composite::metadata&) -> bool override {
        if (m_seen.fetch_add(1, std::memory_order_relaxed) == 0) {
            set_properties(json{{"enabled", false}}, config_type::RUNTIME); // self-disable, worker thread
        }
        return true;
    }

public:
    std::atomic<int> m_seen{0};
    component::auto_stop m_auto_stop{*this};
};

// REGRESSION: an external stop() must drain an ADMITTED property write before reaping subclass
// worker resources -- a writer already inside on_park_requested() may still be touching them.
// on_worker_start() allocates a resource; on_worker_stop() frees it and reports via `reaped`.
// process() spins until released. on_park_requested()'s FIRST call captures the resource pointer
// and blocks until released; a SECOND call (stop()'s own park request) releases process() instead.
class poke_resource : public component {
public:
    explicit poke_resource(std::string_view id) : component(id) {}
    auto process() -> retval override {
        m_processing.store(true, std::memory_order_release);
        while (!m_process_release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return retval::NOOP;
    }
    auto on_worker_start() -> void override { m_resource = new int(42); }
    auto on_park_requested() -> void override {
        if (m_pokes.fetch_add(1, std::memory_order_acq_rel) == 0) {
            m_captured = m_resource;
            m_poke_entered.store(true, std::memory_order_release);
            while (!m_poke_release.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        } else {
            m_process_release.store(true, std::memory_order_release); // stop()'s own park request
        }
    }
    auto on_worker_stop() -> void override {
        delete m_resource;
        m_resource = nullptr;
        reaped.store(true, std::memory_order_release);
    }
    std::atomic<bool> m_processing{false};
    std::atomic<bool> m_process_release{false};
    std::atomic<bool> m_poke_entered{false};
    std::atomic<bool> m_poke_release{false};
    std::atomic<bool> reaped{false};
    std::atomic<int> m_pokes{0};
    int* m_resource{nullptr};
    int* m_captured{nullptr};
    component::auto_stop m_auto_stop{*this};
};

struct gen_cfg {
    int gen{0};
    COMPOSITE_FIELDS(gen_cfg, (gen, runtime));
};

// REGRESSION: a set_properties() issued from on_finished() must have its on_apply run BEFORE
// completion is published -- finish_worker() drains staged config<T> reactions after on_finished(),
// on this same (worker) thread, since no loop-top will ever run again to do it.
class finishing_writer : public component {
public:
    explicit finishing_writer(std::string_view id) : component(id) {
        add_config(m_cfg, config_type::RUNTIME);
        m_cfg.on_apply([this](const gen_cfg&, const changes<gen_cfg>&) {
            m_applied.fetch_add(1, std::memory_order_relaxed);
        });
    }
    auto process() -> retval override { return retval::FINISH; }
    auto on_finished(finish_reason) -> void override { set_properties(json{{"gen", 1}}, config_type::RUNTIME); }
    config<gen_cfg> m_cfg{};
    std::atomic<int> m_applied{0};
    component::auto_stop m_auto_stop{*this};
};

auto live_thread_count() -> std::size_t {
    return static_cast<std::size_t>(
        std::distance(std::filesystem::directory_iterator("/proc/self/task"), std::filesystem::directory_iterator{}));
}
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

    // ---- REGRESSION: a concurrent property write during the completion tail must wait for the
    //      tail (on_finished/send_eos), not race it -- and is_running() must stay true throughout ----
    {
        auto c = std::make_shared<slow_finisher>("slowfin");
        c->start();
        for (int i = 0; i < 2000 && !c->m_tail_started.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(1ms);
        }
        check(c->m_tail_started.load(std::memory_order_acquire), "tail-park: on_finished() reached");

        std::atomic<long long> write_returned_ns{0};
        std::thread writer([&] {
            c->set_properties(json{{"yield_interval", 4}}, config_type::RUNTIME);
            write_returned_ns.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                                    std::memory_order_release);
        });
        writer.join();

        check(c->wait_until_finished(2s), "tail-park: component finished");
        check(c->m_running_at_entry.load(std::memory_order_relaxed),
              "tail-park: is_running() was true on entry to on_finished (EXITING not published early)");
        check(write_returned_ns.load(std::memory_order_acquire) >= c->m_finished_ns.load(std::memory_order_acquire),
              "tail-park: the property write returned only AFTER on_finished()'s tail completed");
    }

    // ---- REGRESSION: a pipeline_component that disables itself (from the worker thread, mid-run)
    //      must reap its pool and flip its state gauge just like a self-finish, even though it exits
    //      with finish_reason::none (stop_locked() is not in that path) ----
    {
        constexpr int W = 4;
        const auto baseline = live_thread_count();

        auto pipe = std::make_shared<self_disabling_pipeline>("selfdis", W);
        output_port<mutable_buffer<int>> feeder{"feed"};
        auto* in = pipe->get_port<input_port_base>("in");
        check(in != nullptr && feeder.connect(in), "selfdis: connect feeder");

        pipe->start();
        for (int i = 0; i < 500 && !pipe->is_running(); ++i) {
            std::this_thread::sleep_for(1ms);
        }
        check(pipe->is_running(), "selfdis: pipeline running (main worker + pool up)");
        check(live_thread_count() > baseline, "selfdis: thread count grew while running");

        for (int s = 0; s < 3; ++s) {
            auto b = make_mutable<int>(1);
            b.as_span()[0] = s;
            feeder.send_data(std::move(b), timestamp{});
        }

        for (int i = 0; i < 2000 && pipe->is_running(); ++i) {
            std::this_thread::sleep_for(1ms);
        }
        check(!pipe->is_running(), "selfdis: pipeline stopped running after disabling itself");
        check(pipe->wait_until_finished(2s), "selfdis: worker exited after the self-disable");

        std::size_t after = live_thread_count();
        for (int i = 0; i < 500 && after > baseline; ++i) {
            std::this_thread::sleep_for(1ms);
            after = live_thread_count();
        }
        check(after == baseline, "selfdis: pool threads joined -- thread count back to baseline");

        bool found_state = false;
        for (const auto& m : metrics::registry::instance().snapshot_by_label("component_id", pipe->id())) {
            if (m.name == "composite.component.state") {
                found_state = true;
                check(std::get<double>(m.value) == 0.0, "selfdis: composite.component.state gauge reads 0");
            }
        }
        check(found_state, "selfdis: found the component's state gauge");

        feeder.disconnect(in);
    }

    // ---- REGRESSION: stop() must drain an admitted property writer before reaping subclass
    //      worker resources -- not race it ----
    {
        auto c = std::make_shared<poke_resource>("poke");
        c->start();
        for (int i = 0; i < 2000 && !c->m_processing.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(1ms);
        }
        check(c->m_processing.load(std::memory_order_acquire), "poke: process() entered");

        std::thread writer([&] { c->set_properties(json{{"yield_interval", 4}}, config_type::RUNTIME); });
        for (int i = 0; i < 2000 && !c->m_poke_entered.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(1ms);
        }
        check(c->m_poke_entered.load(std::memory_order_acquire), "poke: writer admitted into on_park_requested");

        std::thread stopper([&] { c->stop(); });

        const auto deadline = std::chrono::steady_clock::now() + 300ms;
        bool premature = false;
        while (std::chrono::steady_clock::now() < deadline) {
            if (c->reaped.load(std::memory_order_acquire)) {
                premature = true;
                break;
            }
            std::this_thread::yield();
        }
        check(!premature, "poke: resources NOT reaped while the admitted writer still holds them");

        c->m_poke_release.store(true, std::memory_order_release);
        writer.join();
        stopper.join();

        check(c->reaped.load(std::memory_order_acquire), "poke: resources reaped once the writer released");
        check(!c->is_running(), "poke: not running after stop() completes");
    }

    // ---- REGRESSION: a set_properties() issued from on_finished() applies before completion is
    //      published ----
    {
        auto c = std::make_shared<finishing_writer>("tailreact");
        c->start();
        check(c->wait_until_finished(10s), "tail-reaction: worker finished");
        check(c->m_applied.load(std::memory_order_relaxed) == 1, "tail-reaction: on_apply ran exactly once");
        check(c->m_cfg->gen == 1, "tail-reaction: field reflects the on_finished() write");
    }

    if (g_failures) {
        std::printf("\n%d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::puts("LIFECYCLE FINISH OK: on_finished(reason), is_finished/finished_reason, "
              "component + application wait_until_finished, restart-clears-status, property_state, "
              "a concurrent property write parking through the completion tail, a self-disabling "
              "pipeline_component reaping its pool + gauge, an external stop() draining an admitted "
              "writer before reaping, and a self-write from on_finished() applying before completion");
    return 0;
}
