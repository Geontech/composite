// Loop-top execution model: a config<T> on_apply reaction runs on the WORKER
// thread at loop-top, BEFORE process() — not synchronously on the writer thread. This
// proves: (1) the reaction's thread == the process() thread (the model's defining property;
// the old inline model ran it on the REST/writer thread); (2) process() never observes
// a new config value alongside stale derived state, even under a PATCH flood; (3) an
// idle (NOOP-backing-off) component reacts within one wake, not after the full NOOP
// delay. Run under TSan. Own main(); explicit checks. Linked against composite::composite.
#include <composite/core/component.hpp>

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace composite;
using composite::properties::config_type;
using json = composite::properties::json;

struct cfg_t {
    int gen{0};
    COMPOSITE_FIELDS(cfg_t, (gen, runtime));
};

// for the reentrant-write test: on_apply for `trigger` reentrantly sets `derived`.
struct re_cfg {
    int trigger{0};
    int derived{0};
    COMPOSITE_FIELDS(re_cfg, (trigger, runtime), (derived, runtime));
};

class plan_comp : public component {
public:
    explicit plan_comp(std::string_view id) : component(id) {
        add_config(m_cfg, config_type::RUNTIME);
        // "rebuild derived state" reaction: m_plan_gen mirrors the committed gen. By design
        // this runs on the worker at loop-top, so process() below always sees them equal.
        m_cfg.on_apply([this](const cfg_t&, const changes<cfg_t>& ch) {
            if (ch.changed(&cfg_t::gen)) {
                m_reaction_tid = std::this_thread::get_id(); // worker-thread-only write
                m_plan_gen.store(m_cfg->gen, std::memory_order_relaxed);
            }
        });
    }

    auto process() -> retval override {
        if (!m_process_tid_set) {
            m_process_tid = std::this_thread::get_id();
            m_process_tid_set = true;
        }
        const int g = m_cfg->gen;                                 // config value (park-protected)
        const int p = m_plan_gen.load(std::memory_order_relaxed); // derived state
        if (g != p) {
            m_violations.fetch_add(1, std::memory_order_relaxed);
        }
        m_iters.fetch_add(1, std::memory_order_release);
        return m_noop ? retval::NOOP : retval::NORMAL;
    }

    config<cfg_t> m_cfg{};
    std::atomic<int> m_plan_gen{0};
    std::atomic<int> m_violations{0};
    std::atomic<long> m_iters{0};
    bool m_noop{false};
    bool m_process_tid_set{false};
    std::thread::id m_process_tid{};
    std::thread::id m_reaction_tid{};
    component::auto_stop m_auto_stop{*this}; // MUST be last
};

// on_apply reentrantly set_properties()es another field (a supported worker-self-write);
// the reentrant change's reaction must observe changed() (not a clobbered empty diff).
class reentrant_comp : public component {
public:
    explicit reentrant_comp(std::string_view id) : component(id) {
        add_config(m_cfg, config_type::RUNTIME);
        m_cfg.on_apply([this](const re_cfg&, const changes<re_cfg>& ch) {
            if (ch.changed(&re_cfg::trigger)) {
                // reentrant worker-self-write: stages a fresh reaction for `derived`
                set_properties(json{{"derived", m_cfg->trigger * 2}}, config_type::RUNTIME);
            }
            if (ch.changed(&re_cfg::derived)) {
                m_derived_reaction_saw_change.store(true, std::memory_order_release);
            }
        });
    }
    auto process() -> retval override {
        m_iters.fetch_add(1, std::memory_order_release);
        return retval::NOOP;
    }
    config<re_cfg> m_cfg{};
    std::atomic<bool> m_derived_reaction_saw_change{false};
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

// Spin until pred() or the deadline; returns pred()'s final value.
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

    // ---- (1)+(2) flood: reaction runs on the worker thread; no stale-derived-state ----
    {
        plan_comp c{"flood"};
        c.start();
        // wait until the worker is actually running (so set_properties defers to it,
        // rather than inline-draining on this thread during the start window).
        check(wait_until([&] { return c.m_iters.load(std::memory_order_acquire) > 0; }, std::chrono::seconds(2)),
              "worker started and ran process()");

        constexpr int N = 2000;
        for (int i = 1; i <= N; ++i) {
            c.set_properties(json{{"gen", i}}, config_type::RUNTIME);
        }
        // the last (coalesced) reaction drains at a worker loop-top within a wake
        check(wait_until([&] { return c.m_plan_gen.load(std::memory_order_acquire) == N; }, std::chrono::seconds(2)),
              "final reaction applied (plan_gen == last gen)");

        c.stop();
        check(c.m_violations.load() == 0, "process() NEVER saw a new config value with stale derived state");
        check(c.m_reaction_tid == c.m_process_tid,
              "reaction ran on the WORKER thread (same as process()), not the writer thread");
        check(c.m_cfg->gen == N, "config value reached the last write (synchronous swap)");
    }

    // ---- (3) idle/NOOP component reacts within one wake, not after the NOOP delay ----
    {
        plan_comp c{"idle"};
        c.m_noop = true;
        // a deliberately long NOOP backoff: if the worker were NOT woken on a property
        // write, the reaction would lag by ~this long.
        c.set_properties(json{{"noop_thread_delay", 3'000'000'000LL}}, config_type::INITIALIZE); // 3 s
        c.start();
        check(wait_until([&] { return c.m_iters.load(std::memory_order_acquire) > 0; }, std::chrono::seconds(2)),
              "idle worker ran at least one (NOOP) iteration");

        c.set_properties(json{{"gen", 42}}, config_type::RUNTIME);
        const bool reacted = wait_until([&] { return c.m_plan_gen.load(std::memory_order_acquire) == 42; },
                                        std::chrono::milliseconds(500)); // << 3 s NOOP delay
        check(reacted, "idle component reacted within one wake (NOOP backoff was interrupted)");
        c.stop();
    }

    // ---- (4) a reaction staged just before stop() still runs (drained on the stop path) ----
    {
        plan_comp c{"stopdrain"};
        c.start();
        check(wait_until([&] { return c.m_iters.load(std::memory_order_acquire) > 0; }, std::chrono::seconds(2)),
              "worker running");
        // PATCH then IMMEDIATELY stop, without waiting for the worker to drain at loop-top.
        c.set_properties(json{{"gen", 7}}, config_type::RUNTIME);
        c.stop();
        check(c.m_plan_gen.load() == 7,
              "reaction staged before stop() ran (worker loop-top OR stop-path drain), not dropped");
    }

    // ---- (5) reentrant set_properties() from inside on_apply: the reentrant change's
    //          reaction observes changed() (the staged diff is not clobbered) ----
    {
        reentrant_comp c{"reentrant"};
        c.start();
        check(wait_until([&] { return c.m_iters.load(std::memory_order_acquire) > 0; }, std::chrono::seconds(2)),
              "reentrant worker running");
        c.set_properties(json{{"trigger", 5}}, config_type::RUNTIME);
        // cascade: trigger reaction -> set derived=10 -> derived reaction sees changed(derived)
        const bool ok = wait_until(
            [&] { return c.m_derived_reaction_saw_change.load(std::memory_order_acquire) && c.m_cfg->derived == 10; },
            std::chrono::seconds(2));
        c.stop();
        check(ok, "reentrant write's reaction observed changed(derived) (diff not clobbered by the outer drain)");
    }

    if (g_fails != 0) {
        std::fprintf(stderr, "%d loop-top check(s) FAILED\n", g_fails);
        return 1;
    }
    std::puts("CONFIG<T> LOOP-TOP TESTS PASSED");
    return 0;
}
