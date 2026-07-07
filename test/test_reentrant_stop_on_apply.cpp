// Regression for the reentrant-stop deadlock: a lifecycle-touching config<T> on_apply that calls stop()
// must not self-deadlock.
//
// The stopped-writer drain fix routes the inline reaction drain through park::with_worker_parked(), which takes an
// in_flight_guard. If the drained on_apply calls stop(), stop_locked() -> drain_in_flight() would
// spin forever waiting for the caller's OWN guard to drop (park.hpp). The fix: stop_locked() skips
// drain_in_flight() when the current thread is the park owner (owned_by_current_thread()), since it
// already holds m_data_mtx and external park calls are excluded.
//
// Reachable state: a component that has SELF-FINISHED but is not yet joined (EXITING, m_thread still
// set — the window auto-FINISH produces). has_worker() is false there, so a property write takes
// the INLINE drain path; but stop_locked() does NOT early-return (m_thread is set), so it reaches
// drain_in_flight(). A never-started/plain-stopped component early-returns at !m_thread.has_value(),
// which is why test_config_stopped_writers never caught this. Own main(); a watchdog fails fast if
// the deadlock regresses instead of hanging the suite.
#include <composite/core/component.hpp>

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace composite;
using namespace std::chrono_literals;
using composite::properties::config_type;
using json = composite::properties::json;

namespace {
int g_fail = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_fail;
    }
}

struct scfg {
    int gen{0};
    COMPOSITE_FIELDS(scfg, (gen, runtime));
};

// on_apply calls stop() on itself — a supported lifecycle-touching reaction.
class self_stopper : public component {
public:
    explicit self_stopper(std::string_view id) : component(id) {
        add_config(m_cfg, config_type::RUNTIME);
        m_cfg.on_apply([this](const scfg&, const changes<scfg>& ch) {
            if (ch.changed(&scfg::gen)) {
                m_on_apply_ran.store(true, std::memory_order_release);
                stop(); // must NOT deadlock even when run during the inline drain in EXITING
            }
        });
    }
    // Self-finish immediately: the worker returns FINISH, then sits EXITING + unjoined (nothing
    // joins it) until an explicit stop()/destruction — the exact window the deadlock needs.
    auto process() -> retval override { return retval::FINISH; }

    config<scfg> m_cfg{};
    std::atomic<bool> m_on_apply_ran{false};
    component::auto_stop m_auto_stop{*this}; // MUST be last
};
} // namespace

int main() {
    spdlog::set_level(spdlog::level::off);

    // Watchdog: on a deadlock regression, fail fast rather than hang the whole ctest run.
    std::thread{[] {
        std::this_thread::sleep_for(10s);
        std::fprintf(stderr, "FAIL: timeout — reentrant stop() from on_apply deadlocked\n");
        std::_Exit(1);
    }}.detach();

    auto c = std::make_shared<self_stopper>("selfstop");
    c->start();

    // Wait for the worker to self-FINISH, then poll until the park actually settles out of RUNNING
    // (EXITING can lag the wait_until_finished signal). Now: EXITING + m_thread still set.
    check(c->wait_until_finished(2s), "worker self-finished");
    for (int i = 0; i < 2000 && c->is_running(); ++i) {
        std::this_thread::sleep_for(1ms);
    }
    check(!c->is_running(), "park settled to EXITING/NO_WORKER (inline-drain path)");
    check(c->is_finished(), "component reports finished");

    // The property write stages a config<T> reaction; with no live worker it is drained INLINE, and
    // its on_apply calls stop() -> stop_locked() -> (pre-fix) drain_in_flight() spins on our own
    // guard. With the fix this returns promptly.
    c->set_properties(json{{"gen", 1}}, config_type::RUNTIME);

    check(c->m_on_apply_ran.load(std::memory_order_acquire), "on_apply ran (reaction drained)");
    std::puts("REENTRANT-STOP-ON-APPLY OK: lifecycle-touching on_apply calling stop() during the "
              "inline drain in the EXITING state did not deadlock");
    return g_fail ? 1 : 0;
}
