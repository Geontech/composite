// `enabled` as a framework spec/status virtual. Proves: a RUNTIME write
// IS the start/stop action; the read reports desired (spec) AND running (observed) with
// no stale mirror (desync unrepresentable); re-enabling after a DIRECT stop restarts
// even though the desired value did not change (the re-enable no-op trap is gone); the
// legacy two-step set_properties(INITIALIZE)+apply_lifecycle_changes() reconcile still
// works; and get_property<bool>("enabled") shims to the desired state. Own main();
// explicit checks. Linked against composite::composite.
#include <composite/core/component.hpp>

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace composite;
using composite::properties::config_type;
using json = composite::properties::json;

class noop_comp : public component {
public:
    explicit noop_comp(std::string_view id) : component(id) {}
    auto process() -> retval override {
        m_iters.fetch_add(1, std::memory_order_relaxed);
        return retval::NOOP;
    }
    std::atomic<long> m_iters{0};
    component::auto_stop m_auto_stop{*this};
};

// process() returns FINISH on its FIRST call (self-stop, no explicit stop_locked), then
// NOOP — so a restart keeps it running. Exercises the FINISH self-stop path where the
// worker exits the park (EXITING) but its jthread handle is not yet reset.
class finish_once_comp : public component {
public:
    explicit finish_once_comp(std::string_view id) : component(id) {}
    auto process() -> retval override {
        m_iters.fetch_add(1, std::memory_order_release);
        return m_finished.exchange(true) ? retval::NOOP : retval::FINISH;
    }
    std::atomic<bool> m_finished{false};
    std::atomic<long> m_iters{0};
    component::auto_stop m_auto_stop{*this};
};

static int g_fails = 0;
static void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_fails;
    }
}
template <typename Pred>
static bool wait_until(Pred pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::yield();
    }
    return pred();
}

int main() {
    spdlog::set_level(spdlog::level::off);
    using namespace std::chrono_literals;

    // ---- (1) a RUNTIME enabled write IS the start/stop action (no apply_lifecycle_changes) ----
    {
        noop_comp c{"a"};
        check(!c.is_running(), "not running before any enable");
        c.set_properties(json{{"enabled", true}}, config_type::RUNTIME);
        check(wait_until([&] { return c.is_running(); }, 2s), "RUNTIME enabled=true starts the component");
        check(c.is_enabled(), "desired enabled == true");
        c.set_properties(json{{"enabled", false}}, config_type::RUNTIME);
        check(!c.is_running(), "RUNTIME enabled=false stops immediately (write IS action)");
        check(!c.is_enabled(), "desired enabled == false");
    }

    // ---- (2) re-enable after a DIRECT stop restarts (no-op re-enable trap) ----
    {
        noop_comp c{"b"};
        c.set_properties(json{{"enabled", true}}, config_type::RUNTIME);
        check(wait_until([&] { return c.is_running(); }, 2s), "started");
        c.stop(); // direct stop (NOT via the enabled write): observed stops, desired stays true
        check(!c.is_running() && c.is_enabled(),
              "direct stop: not running yet still desired-enabled (no stale mirror to desync)");
        // The desired value is still true (unchanged), but the write must STILL restart —
        // the old model would no-op here and leave it stuck stopped.
        c.set_properties(json{{"enabled", true}}, config_type::RUNTIME);
        check(wait_until([&] { return c.is_running(); }, 2s),
              "re-enable after a direct stop RESTARTS (no-op trap fixed)");
    }

    // ---- (3) property_state reports desired + observed truthfully (desync unrepresentable) ----
    {
        noop_comp c{"d"};
        c.set_properties(json{{"enabled", true}}, config_type::RUNTIME);
        wait_until([&] { return c.is_running(); }, 2s);
        const json st = c.property_state();
        check(st["enabled"] == true && st["running"] == true, "running: enabled+running both true");
        c.stop();
        const json st2 = c.property_state();
        check(st2["enabled"] == true && st2["running"] == false,
              "after direct stop: enabled(desired)=true, running(observed)=false");
    }

    // ---- (4) legacy two-step set_properties(INITIALIZE) + apply_lifecycle_changes() reconcile ----
    {
        noop_comp c{"e"};
        c.set_properties(json{{"enabled", true}}); // INITIALIZE: records desired, does NOT start
        check(!c.is_running(), "INITIALIZE enabled does not start immediately");
        check(c.is_enabled(), "INITIALIZE recorded desired = true");
        c.apply_lifecycle_changes(); // reconcile -> start
        check(wait_until([&] { return c.is_running(); }, 2s), "apply_lifecycle_changes() reconciles to start");
        c.set_properties(json{{"enabled", false}}); // INITIALIZE: desired false
        c.apply_lifecycle_changes();                // reconcile -> stop
        check(!c.is_running(), "apply_lifecycle_changes() reconciles to stop");
    }

    // ---- (5) get_property<bool>("enabled") shims to the desired state ----
    {
        noop_comp c{"f"};
        check(c.get_property<bool>("enabled") == true, "get_property enabled = desired (default true)");
        c.set_properties(json{{"enabled", false}}, config_type::RUNTIME);
        check(c.get_property<bool>("enabled") == false, "get_property enabled tracks desired");
    }

    // ---- (6) a non-boolean enabled is rejected ----
    {
        noop_comp c{"g"};
        bool threw = false;
        try {
            c.set_properties(json{{"enabled", 1}}, config_type::RUNTIME);
        } catch (const std::exception&) {
            threw = true;
        }
        check(threw, "non-boolean enabled rejected");
    }

    // ---- (7) re-enable after a FINISH self-stop restarts (reconcile uses park liveness,
    //          not the stale jthread handle — else the no-op trap returns for self-stops) ----
    {
        finish_once_comp c{"finish"};
        c.set_properties(json{{"enabled", true}}, config_type::RUNTIME); // start; process() FINISHes once
        check(wait_until([&] { return !c.is_running() && c.m_iters.load(std::memory_order_acquire) >= 1; }, 2s),
              "component self-stopped after process() returned FINISH");
        check(c.is_enabled(), "still desired-enabled after a FINISH self-stop");
        // The desired value is unchanged (still true), and the worker handle is leftover
        // (not reset) — the write must STILL restart it.
        c.set_properties(json{{"enabled", true}}, config_type::RUNTIME);
        check(wait_until([&] { return c.is_running(); }, 2s),
              "re-enable after a FINISH self-stop RESTARTS (park-liveness reconcile, not stale handle)");
    }

    if (g_fails != 0) {
        std::fprintf(stderr, "%d enabled-lifecycle check(s) FAILED\n", g_fails);
        return 1;
    }
    std::puts("ENABLED SPEC/STATUS LIFECYCLE TESTS PASSED");
    return 0;
}
