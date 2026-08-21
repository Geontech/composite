/*
 * Copyright (C) 2024-2026 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

// Failure-containment contract (frozen for v0.5): a user callback must never unwind across
// a worker, destructor, or graph-teardown boundary, and a committed property batch stays
// committed when its reaction fails. Before this suite, a throwing config<T> on_apply
// escaped the worker loop-top (std::terminate), escaped ~component / ~auto_stop
// (std::terminate from a destructor), and a failed stop() inside
// application::remove_component() skipped the disconnect loops that make the removal safe.
//
// Also covers the port layer: input_port::depth() must not replace the physical ring while a producer
// is connected (an "observed empty" ring is not producer exclusion), and the power-of-two
// rounding must not spin forever on an out-of-range depth.
//
// Own main(); explicit checks. Linked against composite::composite.
#include <composite/core/application.hpp>
#include <composite/core/component.hpp>
#include <composite/ports/input_port.hpp>
#include <composite/ports/output_port.hpp>

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <unistd.h> // _exit on the fail-fast path below
#include <utility>
#include <vector>

using namespace composite;
using composite::properties::config_type;
using json = composite::properties::json;

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

/// How long slow_wake_comp's on_park_requested() blocks, in case (17c). File scope because a
/// function-local class cannot hold a static data member.
constexpr auto k_slow_wake_hook = std::chrono::milliseconds(300);

struct boom_cfg {
    int gen{0};
    COMPOSITE_FIELDS(boom_cfg, (gen, runtime));
};

// on_apply throws on every change. `m_reactions` counts entries so a test can prove the
// hook actually ran (and therefore that the throw was really raised and contained).
class throwing_reaction_comp : public component {
public:
    explicit throwing_reaction_comp(std::string_view id, bool nonstandard = false) : component(id) {
        add_config(m_cfg, config_type::RUNTIME);
        m_cfg.on_apply([this, nonstandard](const boom_cfg&, const changes<boom_cfg>& ch) {
            if (!ch.changed(&boom_cfg::gen)) {
                return;
            }
            m_reactions.fetch_add(1, std::memory_order_release);
            if (nonstandard) {
                throw 42; // NOT derived from std::exception — the catch(...) arm
            }
            throw std::runtime_error("on_apply deliberately failed");
        });
    }

    auto process() -> retval override {
        m_iters.fetch_add(1, std::memory_order_release);
        return retval::NOOP;
    }

    config<boom_cfg> m_cfg{};
    std::atomic<int> m_reactions{0};
    std::atomic<long> m_iters{0};
    component::auto_stop m_auto_stop{*this}; // MUST be last
};

// Minimal producer/consumer pair for the removal/disconnect and depth tests.
class producer_comp : public component {
public:
    explicit producer_comp(std::string_view id) : component(id) { add_port(m_out); }
    auto process() -> retval override { return retval::NOOP; }
    output_port<immutable_buffer<float>> m_out{"out"};
    component::auto_stop m_auto_stop{*this};
};

class consumer_comp : public component {
public:
    explicit consumer_comp(std::string_view id) : component(id) { add_port(m_in); }
    auto process() -> retval override { return retval::NOOP; }
    input_port<immutable_buffer<float>> m_in{"in", 8};
    component::auto_stop m_auto_stop{*this};
};

int main() {
    spdlog::set_level(spdlog::level::off);

    // ---- (1) worker loop-top: a throwing on_apply must not kill the worker ----
    {
        throwing_reaction_comp c{"looptop"};
        c.start();
        check(wait_until([&] { return c.m_iters.load(std::memory_order_acquire) > 0; }, std::chrono::seconds(2)),
              "worker started");

        const long before = c.m_iters.load(std::memory_order_acquire);
        c.set_properties(json{{"gen", 1}}, config_type::RUNTIME);
        check(wait_until([&] { return c.m_reactions.load(std::memory_order_acquire) == 1; }, std::chrono::seconds(2)),
              "throwing on_apply ran at the worker loop-top");
        // The worker must still be looping AFTER the reaction threw.
        check(
            wait_until([&] { return c.m_iters.load(std::memory_order_acquire) > before + 1; }, std::chrono::seconds(2)),
            "worker SURVIVED a throwing on_apply and kept iterating");
        // The committed value stays committed (policy: log, do not roll back).
        check(c.m_cfg->gen == 1, "committed config value survives its failed reaction");

        // A contained reaction must not re-arm: a second write reacts exactly once more.
        c.set_properties(json{{"gen", 2}}, config_type::RUNTIME);
        check(wait_until([&] { return c.m_reactions.load(std::memory_order_acquire) == 2; }, std::chrono::seconds(2)),
              "second write reacted once (contained failure did not re-arm or spin)");
        c.stop();
    }

    // ---- (2) stopped/inline drain: set_properties must not surface the reaction failure ----
    {
        throwing_reaction_comp c{"inline"};
        bool threw = false;
        try {
            // No worker: the drain runs inline on THIS thread inside set_properties().
            c.set_properties(json{{"gen", 5}}, config_type::RUNTIME);
        } catch (...) {
            threw = true;
        }
        check(!threw, "set_properties() did NOT rethrow a failed reaction on a stopped component");
        check(c.m_reactions.load() == 1, "inline drain ran the throwing reaction");
        check(c.m_cfg->gen == 5, "committed value survives the failed inline reaction");
    }

    // ---- (2b) same, for a NON-std exception (the catch(...) arm) ----
    {
        throwing_reaction_comp c{"inline_nonstd", /*nonstandard=*/true};
        bool threw = false;
        try {
            c.set_properties(json{{"gen", 5}}, config_type::RUNTIME);
        } catch (...) {
            threw = true;
        }
        check(!threw, "set_properties() contained a NON-std exception from on_apply");
        check(c.m_cfg->gen == 5, "committed value survives a non-std reaction failure");
    }

    // ---- (3) destructor: a reaction staged just before teardown must not terminate ----
    {
        auto c = std::make_unique<throwing_reaction_comp>("dtor");
        c->start();
        check(wait_until([&] { return c->m_iters.load(std::memory_order_acquire) > 0; }, std::chrono::seconds(2)),
              "dtor-case worker started");
        // Stage the reaction and destroy immediately: the drain happens on the stop path
        // reached from ~auto_stop / ~component. Before containment this was a throw from a
        // destructor, i.e. std::terminate.
        c->set_properties(json{{"gen", 9}}, config_type::RUNTIME);
        c.reset();
        check(true, "component destruction survived a staged throwing reaction");
    }

    // ---- (4) application::clear(): one failing component must not abandon the rest ----
    {
        application app{"clear_app"};
        auto bad = std::make_shared<throwing_reaction_comp>("bad");
        auto good = std::make_shared<throwing_reaction_comp>("good");
        check(app.add_component(bad), "added bad component");
        check(app.add_component(good), "added good component");
        bad->start();
        good->start();
        check(wait_until([&] { return good->m_iters.load(std::memory_order_acquire) > 0; }, std::chrono::seconds(2)),
              "clear-case workers started");
        // Stage a throwing reaction on the FIRST component in the vector.
        bad->set_properties(json{{"gen", 3}}, config_type::RUNTIME);

        bool threw = false;
        try {
            app.clear();
        } catch (...) {
            threw = true;
        }
        check(!threw, "application::clear() did not propagate a component teardown failure");
        check(!good->is_running(), "clear() still stopped the component AFTER the failing one");
    }

    // ---- (5) remove_component(): edges are disconnected even when teardown misbehaves ----
    {
        application app{"remove_app"};
        auto prod = std::make_shared<producer_comp>("prod");
        auto cons = std::make_shared<consumer_comp>("cons");
        check(app.add_component(prod), "added producer");
        check(app.add_component(cons), "added consumer");
        check(prod->connect("out", cons, "in"), "connected prod.out -> cons.in");
        check(!prod->connections().empty(), "producer has a live edge before removal");

        auto removed = app.remove_component("cons");
        check(removed != nullptr, "remove_component() returned the target");
        // The critical invariant: no edge may survive into a component whose last owner is
        // about to drop it, regardless of how its stop went.
        check(prod->connections().empty(), "remove_component() disconnected every peer edge");
        removed.reset();
        check(prod->connections().empty(), "producer still has no edges after the target is destroyed");
    }

    // ---- (6) overflow callback: a throwing user callback must not fault the producer ----
    {
        input_port<immutable_buffer<float>> in{"in", 2};
        output_port<immutable_buffer<float>> out{"out"};
        check(out.connect(&in), "connected the free-standing port pair");
        in.register_port_metrics("containment_test"); // the drop counters are null until registered
        in.set_overflow_callback([](std::size_t) { throw std::runtime_error("overflow callback failed"); });

        bool threw = false;
        try {
            // depth 2: the 3rd and 4th sends overflow and invoke the throwing callback.
            for (int i = 0; i < 4; ++i) {
                out.send_data(make_immutable<float>(1), timestamp{});
            }
        } catch (...) {
            threw = true;
        }
        check(!threw, "a throwing overflow callback did NOT unwind into the producer's send path");
        check(in.overflow_callback_errors() > 0, "contained overflow-callback failures are counted");
        check(in.stats().packets_dropped() > 0, "the drop itself is still recorded");
    }

    // ---- (7) depth() must not replace the ring while a producer is connected ----
    {
        input_port<immutable_buffer<float>> in{"in", 4};
        output_port<immutable_buffer<float>> out{"out"};
        check(in.available_capacity() == 4, "initial physical capacity");
        check(out.connect(&in), "claimed the input");

        // The ring is EMPTY here — the exact condition the old code used to justify
        // replacing the storage underneath a live producer.
        check(in.pending() == 0, "ring is empty at the moment of the resize attempt");
        in.depth(1024);
        check(in.depth() == 1024, "depth() still moved the SOFT limit while connected");
        // Physical capacity is clamped to the ring, so the effective bound is unchanged.
        check(in.available_capacity() == 4, "physical ring was NOT reallocated under a connected producer");

        // After disconnect the input is unclaimed, so a grow is legal again (setup-time).
        out.disconnect(&in);
        in.depth(1024);
        check(in.available_capacity() == 1024, "an UNCLAIMED, empty input can still grow its ring");
    }

    // ---- (8) power-of-two rounding saturates instead of spinning forever ----
    {
        check(detail::round_up_pow2(0) == 1, "round_up_pow2(0) == 1");
        check(detail::round_up_pow2(1) == 1, "round_up_pow2(1) == 1");
        check(detail::round_up_pow2(3) == 4, "round_up_pow2(3) == 4");
        check(detail::round_up_pow2(1024) == 1024, "round_up_pow2 is idempotent on a power of two");
        constexpr std::size_t k_max_pow2 = std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1);
        // Each of these hung the process before the fix (p <<= 1 reaches 0 and never
        // reaches n). Reaching the check at all is the assertion.
        check(detail::round_up_pow2(k_max_pow2) == k_max_pow2, "round_up_pow2 at the largest power of two");
        check(detail::round_up_pow2(k_max_pow2 + 1) == k_max_pow2, "round_up_pow2 SATURATES above 2^63 (no hang)");
        check(detail::round_up_pow2(std::numeric_limits<std::size_t>::max()) == k_max_pow2,
              "round_up_pow2(SIZE_MAX) saturates (no hang)");
    }

    // ---- (9) has_property() is the config loader's per-component globals filter ----
    {
        throwing_reaction_comp c{"props"};
        check(c.has_property("gen"), "has_property() sees a config<T> FIELD");
        check(c.has_property("enabled"), "has_property() sees the `enabled` framework virtual");
        check(c.has_property("noop_thread_delay"), "has_property() sees a base-registered property");
        check(!c.has_property("max_packet_size"), "has_property() rejects a key this component does not define");

        // Strict application (what the loader now does) must reject the unknown key.
        bool rejected = false;
        try {
            c.set_properties(json{{"max_packet_size", 8192}}, config_type::INITIALIZE, /*allow_unknown=*/false);
        } catch (const std::exception&) {
            rejected = true;
        }
        check(rejected, "an unknown key is REJECTED when allow_unknown=false");
    }

    // ---- (10) finish_error() carries the detail behind finish_reason::error ----
    {
        class failing_comp : public component {
        public:
            explicit failing_comp(std::string_view id) : component(id) {}
            auto process() -> retval override { throw std::runtime_error("process blew up"); }
            component::auto_stop m_auto_stop{*this};
        };

        failing_comp c{"failer"};
        check(c.finish_error().empty(), "finish_error() is empty before any failure");
        c.start();
        check(wait_until([&] { return c.finished_reason() == finish_reason::error; }, std::chrono::seconds(2)),
              "worker self-terminated with reason=error");
        check(c.finish_error() == "process blew up", "finish_error() reports the exception's what()");
        const auto state = c.property_state();
        check(state.contains("finish_error") && state["finish_error"] == "process blew up",
              "property_state() exposes finish_error for a control plane");
        c.stop();
    }

    // ---- (11) a throwing on_park_requested() must NOT skip the worker join ----
    // Regression for the trap that containment itself introduced: stop_locked() calls this
    // user hook one line before the single join site. Swallowing its exception at the
    // stop() boundary alone would trade a loud terminate for a silent use-after-free —
    // ~component would return with the worker thread still running.
    {
        class bad_wake_comp : public component {
        public:
            explicit bad_wake_comp(std::string_view id) : component(id) {}
            auto process() -> retval override {
                m_iters.fetch_add(1, std::memory_order_release);
                return retval::NOOP;
            }
            auto on_park_requested() -> void override {
                m_wake_calls.fetch_add(1, std::memory_order_release);
                throw std::runtime_error("wake hook deliberately failed");
            }
            std::atomic<long> m_iters{0};
            std::atomic<int> m_wake_calls{0};
            component::auto_stop m_auto_stop{*this};
        };

        bad_wake_comp c{"badwake"};
        c.start();
        check(wait_until([&] { return c.m_iters.load(std::memory_order_acquire) > 0; }, std::chrono::seconds(2)),
              "bad-wake worker started");
        c.stop(); // must return with the worker JOINED, not merely stop-requested
        check(c.m_wake_calls.load() > 0, "on_park_requested() actually ran (and threw)");
        check(!c.is_running(), "stop() JOINED the worker despite a throwing on_park_requested()");

        // The join is what matters: after stop() returns, the worker must be gone for good.
        const long settled = c.m_iters.load(std::memory_order_acquire);
        check(!wait_until([&] { return c.m_iters.load(std::memory_order_acquire) > settled; },
                          std::chrono::milliseconds(200)),
              "no worker iterations occur after stop() returned");
    }

    // ---- (11b) the same hook failure reached via DESTRUCTION (the silent path) ----
    // stop() above throws outright without the fix; the destructor route instead goes
    // through stop_contained(), where an unguarded hook failure would be SWALLOWED and the
    // worker left running against freed memory. There is no safe way to assert on a
    // use-after-free from inside the process, so the assertion here is the run itself:
    // under ASan this case is a heap-use-after-free if the join is ever skipped again.
    {
        class bad_wake_comp2 : public component {
        public:
            explicit bad_wake_comp2(std::string_view id) : component(id) {}
            auto process() -> retval override {
                m_iters.fetch_add(1, std::memory_order_release);
                return retval::NOOP;
            }
            auto on_park_requested() -> void override { throw std::runtime_error("wake hook failed in dtor path"); }
            std::atomic<long> m_iters{0};
            component::auto_stop m_auto_stop{*this};
        };

        auto c = std::make_unique<bad_wake_comp2>("badwake_dtor");
        c->start();
        check(wait_until([&] { return c->m_iters.load(std::memory_order_acquire) > 0; }, std::chrono::seconds(2)),
              "dtor-path bad-wake worker started");
        c.reset(); // ~auto_stop -> stop_contained(): must still join before the object dies
        check(true, "destruction with a throwing on_park_requested() joined the worker");
    }

    // ---- (12) a throwing on_worker_stop() must not abort the rest of teardown ----
    {
        class bad_stop_comp : public component {
        public:
            explicit bad_stop_comp(std::string_view id) : component(id) {}
            auto process() -> retval override { return retval::NOOP; }
            auto on_worker_stop() -> void override { throw std::runtime_error("on_worker_stop deliberately failed"); }
            component::auto_stop m_auto_stop{*this};
        };

        bad_stop_comp c{"badstop"};
        c.start();
        bool threw = false;
        try {
            c.stop();
        } catch (...) {
            threw = true;
        }
        check(!threw, "stop() contained a throwing on_worker_stop()");
        check(!c.is_running(), "the worker is still joined when on_worker_stop() throws");
    }

    // ---- (13) partial admission must not over-count transferred packets ----
    {
        input_port<immutable_buffer<float>> in{"in", 2};
        output_port<immutable_buffer<float>> out{"out"};
        check(out.connect(&in), "connected for the batch-stat check");
        in.register_port_metrics("batch_stat_test");  // counters are null until registered
        out.register_port_metrics("batch_stat_test"); // ditto on the producer side

        // Offer 5 into a depth-2 ring: 2 admitted, 3 rejected.
        std::vector<immutable_buffer<float>> bufs;
        bufs.reserve(5);
        for (int i = 0; i < 5; ++i) {
            bufs.push_back(make_immutable<float>(4));
        }
        out.send_batch(std::span<immutable_buffer<float>>{bufs}, timestamp{});

        check(out.stats().packets_transferred() == 2, "send_batch recorded only the ADMITTED prefix as transferred");
        check(in.stats().packets_dropped() == 3, "the rejected suffix is counted as dropped");
        check(out.stats().packets_transferred() + in.stats().packets_dropped() == 5,
              "transferred + dropped equals packets offered");
    }

    // ---- (13b) same for the MUTABLE send_batch, on both of its admission branches ----
    {
        // mutable -> mutable: add_batch moves straight into the ring.
        input_port<mutable_buffer<float>> in{"in", 2};
        output_port<mutable_buffer<float>> out{"out"};
        check(out.connect(&in), "connected mutable->mutable for the batch-stat check");
        in.register_port_metrics("mut_batch_test");
        out.register_port_metrics("mut_batch_test");

        std::vector<mutable_buffer<float>> bufs;
        bufs.reserve(5);
        for (int i = 0; i < 5; ++i) {
            bufs.push_back(make_mutable<float>(4));
        }
        out.send_batch(std::span<mutable_buffer<float>>{bufs}, timestamp{});
        check(out.stats().packets_transferred() == 2, "mutable send_batch recorded only the admitted prefix");
        check(out.stats().packets_transferred() + in.stats().packets_dropped() == 5,
              "mutable path: transferred + dropped equals packets offered");
    }
    {
        // mutable -> immutable: buffers are PROMOTED through the scratch vector, a
        // different add_batch overload — it must account for the prefix too.
        input_port<immutable_buffer<float>> in{"in", 2};
        output_port<mutable_buffer<float>> out{"out"};
        check(out.connect(&in), "connected mutable->immutable for the batch-stat check");
        in.register_port_metrics("promo_batch_test");
        out.register_port_metrics("promo_batch_test");

        std::vector<mutable_buffer<float>> bufs;
        bufs.reserve(5);
        for (int i = 0; i < 5; ++i) {
            bufs.push_back(make_mutable<float>(4));
        }
        out.send_batch(std::span<mutable_buffer<float>>{bufs}, timestamp{});
        check(out.stats().packets_transferred() == 2, "promoting send_batch recorded only the admitted prefix");
        check(out.stats().packets_transferred() + in.stats().packets_dropped() == 5,
              "promotion path: transferred + dropped equals packets offered");
    }

    // ---- (14) a slow process() makes stop() REPORT, and the join still completes ----
    // The join cannot be abandoned (~component is one of stop()'s callers), so the fix for a
    // wedged process() is diagnosability, not a timeout. Hold process() past the reporting
    // interval and prove two things: stop() emits a warning naming the component instead of
    // waiting mutely, and it still joins cleanly once process() lets go.
    {
        class slow_stop_comp : public component {
        public:
            explicit slow_stop_comp(std::string_view id) : component(id) {}
            auto process() -> retval override {
                m_in_process.store(true, std::memory_order_release);
                // Deliberately ignore the stop token for longer than the 5s report interval.
                while (!m_release.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                return retval::NOOP;
            }
            std::atomic<bool> m_in_process{false};
            std::atomic<bool> m_release{false};
            component::auto_stop m_auto_stop{*this};
        };

        slow_stop_comp c{"slowstop"};
        c.start();
        check(wait_until([&] { return c.m_in_process.load(std::memory_order_acquire); }, std::chrono::seconds(2)),
              "slow-stop worker entered process()");

        // Release process() from another thread AFTER the first report interval has elapsed, so
        // stop() is forced through at least one reporting cycle.
        std::thread releaser{[&c] {
            std::this_thread::sleep_for(std::chrono::milliseconds(5500));
            c.m_release.store(true, std::memory_order_release);
        }};

        const auto t0 = std::chrono::steady_clock::now();
        c.stop(); // must wait (loudly), then join
        const auto waited = std::chrono::steady_clock::now() - t0;
        releaser.join();

        check(waited >= std::chrono::seconds(5), "stop() waited for the un-cooperative process(), did not abandon it");
        check(!c.is_running(), "stop() JOINED the worker once process() returned");
    }

    // ---- (15) try_stop(): bounded, honest about failure, and RETRYABLE ----
    // The contract that matters: a false return means "still running, do not destroy me", and a
    // later stop still completes cleanly. Anything less and the bounded path would just relocate
    // the hang into a destructor.
    {
        class held_comp : public component {
        public:
            explicit held_comp(std::string_view id) : component(id) {}
            auto process() -> retval override {
                m_in_process.store(true, std::memory_order_release);
                while (!m_release.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                return retval::NOOP;
            }
            std::atomic<bool> m_in_process{false};
            std::atomic<bool> m_release{false};
            component::auto_stop m_auto_stop{*this};
        };

        // (a) a cooperative component stops well inside the budget
        {
            throwing_reaction_comp c{"trystop_ok"};
            c.start();
            check(wait_until([&] { return c.m_iters.load(std::memory_order_acquire) > 0; }, std::chrono::seconds(2)),
                  "try_stop: cooperative worker started");
            check(c.try_stop(std::chrono::seconds(2)), "try_stop() returned true for a cooperative component");
            check(!c.is_running(), "try_stop() success means fully stopped");
            check(c.try_stop(std::chrono::seconds(2)), "try_stop() on an already-stopped component is true");
        }

        // (b) a wedged component reports false, stays running, and is NOT torn down
        {
            held_comp c{"trystop_wedged"};
            c.start();
            check(wait_until([&] { return c.m_in_process.load(std::memory_order_acquire); }, std::chrono::seconds(2)),
                  "try_stop: wedged worker entered process()");

            const auto t0 = std::chrono::steady_clock::now();
            const bool stopped = c.try_stop(std::chrono::milliseconds(300));
            const auto waited = std::chrono::steady_clock::now() - t0;

            check(!stopped, "try_stop() returned FALSE for a component that would not stop");
            check(waited < std::chrono::seconds(2), "try_stop() respected its budget instead of waiting it out");
            check(c.is_running(), "a component that did not stop is left RUNNING, not half-torn-down");

            // Retry after the worker is freed: the latched request means it exits, and the
            // deferred teardown completes.
            c.m_release.store(true, std::memory_order_release);
            check(c.try_stop(std::chrono::seconds(5)), "a retry after the worker frees up COMPLETES the stop");
            check(!c.is_running(), "retry left the component fully stopped");
        }
    }

    // ---- (16) application::try_stop(): reports the wedged ids, stops everything else ----
    {
        class held_comp2 : public component {
        public:
            explicit held_comp2(std::string_view id) : component(id) {}
            auto process() -> retval override {
                while (!m_release.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                return retval::NOOP;
            }
            std::atomic<bool> m_release{false};
            component::auto_stop m_auto_stop{*this};
        };

        application app{"trystop_app"};
        auto wedged = std::make_shared<held_comp2>("wedged");
        auto fine = std::make_shared<throwing_reaction_comp>("fine");
        check(app.add_component(wedged), "added the wedged component");
        check(app.add_component(fine), "added the cooperative component");
        app.start();
        check(wait_until([&] { return fine->m_iters.load(std::memory_order_acquire) > 0; }, std::chrono::seconds(2)),
              "app try_stop: graph running");

        const auto ids = app.try_stop(std::chrono::milliseconds(500));
        check(ids.size() == 1 && ids.front() == "wedged", "application::try_stop() named exactly the wedged component");
        check(!fine->is_running(), "the cooperative component still stopped despite the wedged one");
        check(wedged->is_running(), "the wedged component is left running, not destroyed");

        // Release it so the fixture can tear down without a blocked destructor.
        wedged->m_release.store(true, std::memory_order_release);
        check(app.try_stop(std::chrono::seconds(5)).empty(), "a retry stops the whole application");
    }

    // ---- (17) the try_stop BUDGET is real, not nominal ----
    // Regression for two ways a "bounded" API can quietly overrun: a contended lifecycle lock
    // consuming the budget before the attempt starts, and a multi-component signalling pass
    // charged on top of the deadline rather than against it.
    {
        class held_comp3 : public component {
        public:
            explicit held_comp3(std::string_view id) : component(id) {}
            auto process() -> retval override {
                m_entered.store(true, std::memory_order_release);
                while (!m_release.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                return retval::NOOP;
            }
            std::atomic<bool> m_entered{false};
            std::atomic<bool> m_release{false};
            component::auto_stop m_auto_stop{*this};
        };

        // (a) N wedged components must cost ONE budget, not N budgets.
        {
            constexpr int k_components = 4;
            constexpr auto k_budget = std::chrono::milliseconds(400);
            application app{"budget_app"};
            std::vector<std::shared_ptr<held_comp3>> comps;
            for (int i = 0; i < k_components; ++i) {
                comps.push_back(std::make_shared<held_comp3>("wedge" + std::to_string(i)));
                check(app.add_component(comps.back()), "budget: component added");
            }
            app.start();
            // Every worker must be INSIDE the blocking process() before we attempt the stop.
            // The worker loop tests the stop token at its top, so a component signalled before
            // it first entered process() exits cleanly and is (correctly) not reported wedged.
            check(wait_until(
                      [&] {
                          for (const auto& c : comps) {
                              if (!c->m_entered.load(std::memory_order_acquire)) {
                                  return false;
                              }
                          }
                          return true;
                      },
                      std::chrono::seconds(3)),
                  "budget: every worker is inside process() before the stop attempt");

            const auto t0 = std::chrono::steady_clock::now();
            const auto ids = app.try_stop(k_budget);
            const auto waited = std::chrono::steady_clock::now() - t0;

            check(ids.size() == k_components, "budget: every wedged component is reported");
            // A per-component budget would take k_components * k_budget; a shared deadline takes
            // roughly one. Allow generous slack for scheduling, but far below the serial cost.
            check(waited < k_budget * 2, "budget: a SHARED deadline, not one budget per component");

            for (auto& c : comps) {
                c->m_release.store(true, std::memory_order_release);
            }
            check(app.try_stop(std::chrono::seconds(5)).empty(), "budget: retry stops them all");
        }

        // (b) REAL lifecycle-lock contention: a concurrent stop() holds m_lifecycle_mtx for as
        //     long as the wedged worker runs, so try_stop() must give up on the LOCK within its
        //     budget rather than inheriting the other caller's unbounded wait.
        {
            auto c = std::make_shared<held_comp3>("lockheld");
            c->start();
            check(wait_until([&] { return c->m_entered.load(std::memory_order_acquire); }, std::chrono::seconds(3)),
                  "budget: worker inside process() before the contention test");

            // This thread parks in the UNBOUNDED stop(), holding m_lifecycle_mtx throughout.
            std::thread blocker{[c] { c->stop(); }};
            std::this_thread::sleep_for(std::chrono::milliseconds(200)); // let it take the lock

            const auto t0 = std::chrono::steady_clock::now();
            const bool ok = c->try_stop(std::chrono::milliseconds(300));
            const auto waited = std::chrono::steady_clock::now() - t0;

            check(!ok, "contention: try_stop() reports false when it cannot take the lifecycle lock");
            check(waited < std::chrono::seconds(2),
                  "contention: try_stop() bounded its LOCK acquisition instead of inheriting the blocker's wait");

            c->m_release.store(true, std::memory_order_release);
            blocker.join();
            check(!c->is_running(), "contention: the blocking stop() completed once the worker returned");
        }

        // (c) a property write must not slip in behind the drain and race teardown. With the
        //     admission gate closed for the duration of the stop, a writer arriving mid-teardown
        //     waits for it to finish rather than running concurrently with on_worker_stop().
        {
            class gated_comp : public component {
            public:
                explicit gated_comp(std::string_view id) : component(id) {
                    add_property("knob", m_knob, config_type::RUNTIME);
                }
                auto process() -> retval override { return retval::NOOP; }
                // Teardown POLLS for overlap for its whole duration. Sampling once would almost
                // never catch a writer, which is how an earlier version of this test passed even
                // with the admission gate removed.
                auto on_worker_stop() -> void override {
                    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
                    while (std::chrono::steady_clock::now() < until) {
                        if (m_writer_inside.load(std::memory_order_acquire)) {
                            m_writer_during_teardown.store(true, std::memory_order_release);
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }
                // Runs INSIDE with_worker_parked (so it is counted in-flight). Holds the flag for
                // a real interval, giving the poll above something to observe.
                auto property_change_handler(const json& /*diff*/) -> void override {
                    m_writer_inside.store(true, std::memory_order_release);
                    std::this_thread::sleep_for(std::chrono::milliseconds(40));
                    m_writer_inside.store(false, std::memory_order_release);
                }
                int m_knob{0};
                std::atomic<bool> m_tearing_down{false};
                std::atomic<bool> m_writer_inside{false};
                std::atomic<bool> m_writer_during_teardown{false};
                component::auto_stop m_auto_stop{*this};
            };

            gated_comp c{"gated"};
            c.start();
            check(wait_until([&] { return c.is_running(); }, std::chrono::seconds(2)), "gate: component running");

            // Fire property writes continuously while the stop tears down.
            std::atomic<bool> stop_writing{false};
            std::thread writer{[&c, &stop_writing] {
                int n = 0;
                while (!stop_writing.load(std::memory_order_acquire)) {
                    try {
                        c.set_properties(json{{"knob", ++n}}, config_type::RUNTIME);
                    } catch (...) { // NOLINT(bugprone-empty-catch) — racing teardown; failures are fine
                    }
                }
            }};

            std::this_thread::sleep_for(std::chrono::milliseconds(50)); // writers in steady state
            c.stop();
            stop_writing.store(true, std::memory_order_release);
            writer.join();

            check(!c.m_writer_during_teardown.load(std::memory_order_acquire),
                  "gate: no property write was inside the component while on_worker_stop() ran");
        }
    }

    // ---- (17c) the SIGNALLING pass is charged against the budget, not added to it ----
    // application::try_stop() calls each component's on_park_requested() wake hook before it
    // starts collecting. That hook is user code and can be slow, so taking the deadline after
    // that loop silently turns a budget of T into a wall-clock cost of (hook time + T). Give
    // the hook real cost and assert the total stays near the hook time rather than exceeding it
    // by a whole budget.
    {
        class slow_wake_comp : public component {
        public:
            explicit slow_wake_comp(std::string_view id) : component(id) {}
            auto process() -> retval override {
                m_entered.store(true, std::memory_order_release);
                while (!m_release.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                return retval::NOOP;
            }
            auto on_park_requested() -> void override { std::this_thread::sleep_for(k_slow_wake_hook); }
            std::atomic<bool> m_entered{false};
            std::atomic<bool> m_release{false};
            component::auto_stop m_auto_stop{*this};
        };

        constexpr int k_n = 2;
        constexpr auto k_budget = std::chrono::milliseconds(400);
        const auto signalling_cost = k_slow_wake_hook * k_n;

        application app{"wake_budget_app"};
        std::vector<std::shared_ptr<slow_wake_comp>> comps;
        for (int i = 0; i < k_n; ++i) {
            comps.push_back(std::make_shared<slow_wake_comp>("slowwake" + std::to_string(i)));
            check(app.add_component(comps.back()), "wake budget: component added");
        }
        app.start();
        check(wait_until(
                  [&] {
                      for (const auto& c : comps) {
                          if (!c->m_entered.load(std::memory_order_acquire)) {
                              return false;
                          }
                      }
                      return true;
                  },
                  std::chrono::seconds(3)),
              "wake budget: workers inside process() before the stop attempt");

        const auto t0 = std::chrono::steady_clock::now();
        const auto ids = app.try_stop(k_budget);
        const auto waited = std::chrono::steady_clock::now() - t0;

        check(ids.size() == k_n, "wake budget: both wedged components reported");
        // The wake hook fires in BOTH passes now. It used to be skipped in pass 2 whenever the
        // stop was already latched, which saved one hook per component but meant a worker that
        // entered a custom, non-token-aware wait AFTER the first nudge never got a second one and
        // could not be stopped at all — see request_worker_exit_locked(). So a wedged component
        // costs ~2x the hook: that is the price of the nudge always landing.
        //
        // The property under test is unchanged: the deadline is taken BEFORE signalling, so the
        // signalling passes are charged against the budget rather than added on top.
        //   deadline before signalling: total ~= 2 * signalling_cost (budget spent by then)
        //   deadline after  signalling: total ~= 2 * signalling_cost + k_budget
        check(waited < (2 * signalling_cost) + (k_budget / 2),
              "wake budget: the wake-hook passes are charged AGAINST the budget, not added on top");

        for (auto& c : comps) {
            c->m_release.store(true, std::memory_order_release);
        }
        check(app.try_stop(std::chrono::seconds(5)).empty(), "wake budget: retry stops them all");
    }

    // ---- (18) admission gate under concurrent writers: many start/stop cycles ----
    // The narrow interleaving the gate closes (a writer testing admission, then being
    // closed-and-drained before it registers) cannot be PROVEN absent by a test — the real
    // argument is structural: the check and the in-flight registration happen under the same
    // mutex close_admission() takes, so a writer is either counted before the close or parked
    // behind it. What this case does is hammer the window from several threads across many
    // stop cycles, which is where TSan can see a teardown racing a writer if the gate ever
    // regresses. Run it under TSan for that value.
    {
        class churn_comp : public component {
        public:
            explicit churn_comp(std::string_view id) : component(id) {
                add_property("knob", m_knob, config_type::RUNTIME);
            }
            auto process() -> retval override { return retval::NOOP; }
            auto on_worker_stop() -> void override {
                // Touch state a racing writer would also touch, so a regression is a real race
                // rather than a benign overlap.
                m_teardowns.fetch_add(1, std::memory_order_acq_rel);
                m_scratch.assign(64, 'x');
            }
            auto property_change_handler(const json& /*diff*/) -> void override {
                m_scratch.assign(64, 'y');
                m_writes.fetch_add(1, std::memory_order_acq_rel);
            }
            int m_knob{0};
            std::string m_scratch;
            std::atomic<int> m_teardowns{0};
            std::atomic<int> m_writes{0};
            component::auto_stop m_auto_stop{*this};
        };

        churn_comp c{"churn"};
        std::atomic<bool> done{false};
        std::vector<std::thread> writers;
        for (int w = 0; w < 3; ++w) {
            writers.emplace_back([&c, &done, w] {
                int n = 0;
                while (!done.load(std::memory_order_acquire)) {
                    try {
                        c.set_properties(json{{"knob", ++n + w}}, config_type::RUNTIME);
                    } catch (...) { // NOLINT(bugprone-empty-catch) — racing lifecycle; expected
                    }
                }
            });
        }
        for (int cycle = 0; cycle < 25; ++cycle) {
            c.start();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            c.stop();
        }
        done.store(true, std::memory_order_release);
        for (auto& t : writers) {
            t.join();
        }
        check(c.m_teardowns.load() > 0, "gate churn: teardowns actually ran");
        check(c.m_writes.load() > 0, "gate churn: property writes actually landed");
        check(!c.is_running(), "gate churn: component ends stopped");
    }

    // ---- (19) a stop hook that writes a property must not deadlock behind its own gate ----
    // stop() closes admission and THEN calls user hooks. Without an owner bypass, a hook that
    // calls set_properties() classifies the stopping thread as an external writer and parks it
    // on a gate only that same thread can reopen — a hard deadlock, and one that worked before
    // the gate existed. Both hooks are exercised: on_park_requested (before the join) and
    // on_worker_stop (during teardown).
    {
        class writing_hooks_comp : public component {
        public:
            explicit writing_hooks_comp(std::string_view id) : component(id) {
                add_property("knob", m_knob, config_type::RUNTIME);
            }
            auto process() -> retval override { return retval::NOOP; }
            auto on_park_requested() -> void override {
                try {
                    set_properties(json{{"knob", 1}}, config_type::RUNTIME);
                    m_wake_write_ok.store(true, std::memory_order_release);
                } catch (...) { // NOLINT(bugprone-empty-catch) — a rejection is fine; a HANG is not
                }
            }
            auto on_worker_stop() -> void override {
                try {
                    set_properties(json{{"knob", 2}}, config_type::RUNTIME);
                    m_teardown_write_ok.store(true, std::memory_order_release);
                } catch (...) { // NOLINT(bugprone-empty-catch) — as above
                }
            }
            int m_knob{0};
            std::atomic<bool> m_wake_write_ok{false};
            std::atomic<bool> m_teardown_write_ok{false};
            component::auto_stop m_auto_stop{*this};
        };

        // Heap-allocated on purpose: if this ever regresses, the stop deadlocks AND so would the
        // destructor (auto_stop calls the same stop()), hanging the whole suite at scope exit.
        // On a wedged run we release the pointer and leak it rather than take the suite down.
        auto owned = std::make_unique<writing_hooks_comp>("hookwriter");
        auto& c = *owned;
        c.start();
        check(wait_until([&] { return c.is_running(); }, std::chrono::seconds(2)), "hook write: component running");

        // If the gate had no owner bypass this call would never return. Bound the whole thing on
        // another thread so a regression fails the suite instead of hanging CI forever.
        std::atomic<bool> returned{false};
        std::thread stopper{[&c, &returned] {
            c.stop();
            returned.store(true, std::memory_order_release);
        }};
        const bool finished =
            wait_until([&] { return returned.load(std::memory_order_acquire); }, std::chrono::seconds(10));
        check(finished, "hook write: stop() returned — a property-writing hook did not deadlock on its own gate");
        if (finished) {
            stopper.join();
        } else {
            stopper.detach();      // wedged: leak the thread rather than hang the suite on join
            (void)owned.release(); // and leak the component — its destructor would deadlock too
        }
        check(c.m_teardown_write_ok.load(std::memory_order_acquire),
              "hook write: the on_worker_stop() property write actually completed");
    }

    // ---- (20) a worker that DISABLES ITSELF must not join itself ----
    // `enabled` is advertised as a runtime property and worker-originated set_properties() is a
    // supported path, so a component turning itself off from process() is ordinary usage. The
    // reconcile used to run the full stop inline, which waited on the worker-done flag that only
    // this same thread could set — a permanent hang holding m_lifecycle_mtx, which then wedged
    // every later stop()/start() on the component too. A regression here HANGS.
    {
        class self_disable_comp : public component {
        public:
            explicit self_disable_comp(std::string_view id) : component(id) {}
            auto process() -> retval override {
                if (!std::exchange(m_done, false)) {
                    return retval::NOOP;
                }
                set_properties(json{{"enabled", false}}, config_type::RUNTIME);
                m_returned.store(true, std::memory_order_release);
                return retval::NOOP;
            }
            std::atomic<bool> m_returned{false};
            bool m_done{true};
            component::auto_stop m_auto_stop{*this};
        };

        self_disable_comp c{"selfdisable"};
        c.start();
        check(wait_until([&] { return c.m_returned.load(std::memory_order_acquire); }, std::chrono::seconds(5)),
              "self-disable: the worker's own set_properties({enabled:false}) returned");
        check(wait_until([&] { return !c.is_running(); }, std::chrono::seconds(5)),
              "self-disable: the worker actually stopped");
        // The lifecycle lock must still be usable — the old hang held it forever.
        check(c.try_stop(std::chrono::seconds(5)), "self-disable: a later stop still completes");
    }

    // ---- (21) a wedged teardown must not make the control plane unbounded ----
    // Two separate defects made POST /app/stop hang exactly when it is needed. The admission gate
    // waited with no timeout, so a property write during a teardown blocked forever; and
    // try_stop()'s signalling pass took each lifecycle lock UNTIMED, so a component already inside
    // an unbounded join stalled the whole bounded pass before the budget was ever consulted.
    {
        class wedged_comp : public component {
        public:
            explicit wedged_comp(std::string_view id) : component(id) { add_property("knob", m_knob); }
            auto process() -> retval override {
                m_entered.store(true, std::memory_order_release);
                while (!m_release.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                return retval::NOOP;
            }
            std::atomic<bool> m_entered{false};
            std::atomic<bool> m_release{false};
            double m_knob{1.0};
            component::auto_stop m_auto_stop{*this};
        };

        auto wedged = std::make_shared<wedged_comp>("wedged");
        application app{"wedged_app"};
        check(app.add_component(wedged), "wedged: component added");
        app.start();
        check(wait_until([&] { return wedged->m_entered.load(std::memory_order_acquire); }, std::chrono::seconds(5)),
              "wedged: worker is inside the wedged process()");

        // A thread parked forever in the unbounded join, holding m_lifecycle_mtx and the gate.
        std::thread stopper{[&] { wedged->stop(); }};
        std::this_thread::sleep_for(std::chrono::milliseconds(200)); // let it reach the join

        // (a) a property write must be REJECTED, not hang, while the gate is closed.
        {
            const auto t0 = std::chrono::steady_clock::now();
            bool threw = false;
            try {
                wedged->set_properties(json{{"knob", 2.0}}, config_type::RUNTIME);
            } catch (const std::exception&) {
                threw = true;
            }
            const auto waited = std::chrono::steady_clock::now() - t0;
            check(threw, "gate bound: a property write during teardown is rejected, not accepted");
            check(waited < std::chrono::seconds(30), "gate bound: the rejection is BOUNDED, not an unbounded wait");
        }

        // (b) the bounded stop must honour its budget even though pass 1 cannot take the lock.
        {
            const auto t0 = std::chrono::steady_clock::now();
            const auto ids = app.try_stop(std::chrono::milliseconds(500));
            const auto waited = std::chrono::steady_clock::now() - t0;
            check(ids.size() == 1, "signalling bound: the wedged component is reported as not stopped");
            check(waited < std::chrono::seconds(5),
                  "signalling bound: try_stop() returned within its budget despite an untakeable lifecycle lock");
        }

        wedged->m_release.store(true, std::memory_order_release);
        stopper.join();
    }

    // ---- (22) destruction must not abandon a writer PARKED AT THE ADMISSION GATE ----
    // A writer blocked on the gate has not incremented the in-flight count yet, so a teardown that
    // drains only that count sees zero and proceeds — and the waiter later wakes inside an
    // already-destroyed mutex and condition variable. Reachable with NO worker, where stop()
    // returns without draining writers at all.
    //
    // Two independent signals, so this is not ASan-only:
    //   - TIMING. Permanent closure must WAKE the queued writer, not leave it to time out. With
    //     the wake it returns in milliseconds; without it, it sits the full park timeout (5s).
    //   - MEMORY. Destruction must not return until the writer is out of the coordinator, which
    //     under ASan is a heap-use-after-free if it regresses.
    {
        struct gated_comp : component {
            explicit gated_comp(std::string_view id) : component(id) { add_property("knob", m_knob); }
            auto process() -> retval override { return retval::FINISH; }
            double m_knob{1.0};
            component::auto_stop m_auto_stop{*this};
        };

        std::atomic<bool> writer_entered{false};
        std::atomic<bool> writer_returned{false};
        std::atomic<bool> writer_rejected{false};

        auto* c = new gated_comp{"gatewait"}; // NOLINT(cppcoreguidelines-owning-memory)
        c->park_for_test().close_admission(); // hold it shut; only destruction reopens/retires it

        std::thread writer{[&] {
            writer_entered.store(true, std::memory_order_release);
            try {
                c->set_properties(json{{"knob", 2.0}}, config_type::RUNTIME);
            } catch (const std::exception&) {
                writer_rejected.store(true, std::memory_order_release); // rejected, as it must be
            }
            writer_returned.store(true, std::memory_order_release);
        }};

        check(wait_until([&] { return writer_entered.load(std::memory_order_acquire); }, std::chrono::seconds(5)),
              "gate wait: writer thread started");
        std::this_thread::sleep_for(std::chrono::milliseconds(300)); // let it park on the CV

        const auto t0 = std::chrono::steady_clock::now();
        delete c; // NOLINT(cppcoreguidelines-owning-memory) — must wake AND wait for the writer
        const auto teardown = std::chrono::steady_clock::now() - t0;
        writer.join();
        const auto total = std::chrono::steady_clock::now() - t0;

        check(writer_rejected.load(), "gate wait: the parked writer was rejected, not admitted");
        check(writer_returned.load(), "gate wait: the parked writer returned");
        // The park timeout is 5s. Without the wake-on-permanent-close the writer would still be
        // sitting on the CV here, so the join would take ~5s.
        check(total < std::chrono::seconds(3),
              "gate wait: permanent closure WOKE the parked writer instead of leaving it to time out");
        std::printf("gate wait: teardown %lldms, writer out after %lldms (park timeout is 5000ms)\n",
                    (long long)std::chrono::duration_cast<std::chrono::milliseconds>(teardown).count(),
                    (long long)std::chrono::duration_cast<std::chrono::milliseconds>(total).count());
    }

    // ---- (23) a post-close writer's exit must NOT satisfy an older writer's drain ----
    // The drain used to compare aggregate entry/exit TOTALS, which cannot express "these
    // particular callers finished" — exits are fungible. Cut at entered=N/exited=N-1 with writer A
    // still inside; writer B arrives afterwards, is rejected, and its exit pushes the total to N,
    // so the destructor concludes "drained" and frees the coordinator underneath A.
    //
    // Deterministic: A is pinned inside its property write by a blocking on_change, B is released
    // only after the teardown has begun, and the teardown must still be waiting when B is done.
    {
        struct pinned_comp : component {
            explicit pinned_comp(std::string_view id) : component(id) {
                add_property("knob", m_knob, config_type::RUNTIME).on_change([this](const json&) {
                    m_inside.store(true, std::memory_order_release);
                    while (!m_release.load(std::memory_order_acquire)) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }
                });
            }
            auto process() -> retval override { return retval::FINISH; }
            double m_knob{1.0};
            std::atomic<bool> m_inside{false};
            std::atomic<bool> m_release{false};
            component::auto_stop m_auto_stop{*this};
        };

        auto* c = new pinned_comp{"cohort"}; // NOLINT(cppcoreguidelines-owning-memory)
        std::atomic<bool> teardown_returned{false};
        std::atomic<bool> b_done{false};

        // Writer A: enters, and stays inside until released.
        std::thread a{[&] {
            try {
                c->set_properties(json{{"knob", 2.0}}, config_type::RUNTIME);
            } catch (const std::exception&) { // not expected, but must not abort the test
            }
        }};
        check(wait_until([&] { return c->m_inside.load(std::memory_order_acquire); }, std::chrono::seconds(5)),
              "cohort: writer A is inside its property write");

        // Teardown begins while A is inside. It must NOT return.
        std::thread destroyer{[&] {
            delete c; // NOLINT(cppcoreguidelines-owning-memory)
            teardown_returned.store(true, std::memory_order_release);
        }};
        std::this_thread::sleep_for(std::chrono::milliseconds(300)); // let it reach the cohort wait

        // Writer B: arrives AFTER the close, is rejected, and returns. Under the old totals-based
        // drain this exit is what wrongly satisfied A's ticket.
        std::thread b{[&] {
            try {
                c->set_properties(json{{"knob", 3.0}}, config_type::RUNTIME);
            } catch (const std::exception&) { // rejected: teardown in progress
            }
            b_done.store(true, std::memory_order_release);
        }};
        check(wait_until([&] { return b_done.load(std::memory_order_acquire); }, std::chrono::seconds(10)),
              "cohort: post-close writer B completed");

        // THE ASSERTION: B's exit must not have counted for A.
        //
        // FAIL FAST rather than continuing. If the teardown has already returned, the component
        // has been destroyed while writer A is still inside a property write on it — every
        // subsequent step (releasing A, joining it, unwinding) then operates on freed memory, and
        // the process hangs or aborts before it can report anything. Reporting the violation and
        // exiting is the only way this produces a usable diagnosis instead of a mystery.
        if (teardown_returned.load(std::memory_order_acquire)) {
            std::fprintf(stderr, "FAIL: cohort: teardown returned while writer A was still inside — a "
                                 "post-close writer's exit satisfied an older writer's drain\n");
            std::fflush(stderr);
            _exit(1);
        }
        check(true, "cohort: teardown still waiting for writer A after a post-close writer exited");

        c->m_release.store(true, std::memory_order_release);
        a.join();
        b.join();
        destroyer.join();
        check(teardown_returned.load(std::memory_order_acquire), "cohort: teardown completed once A returned");
    }

    // NOTE on the narrower window above: a caller that has ENTERED with_worker_parked() but has
    // not yet reached the gate mutex is covered by the entry ticket stamped at the top of that
    // function (see park.hpp). It is deliberately NOT stress-tested here. Any test that races a
    // fresh set_properties() against destruction is testing something the contract explicitly does
    // NOT promise — a call STARTED after destruction begins touches a freed object no matter what
    // the coordinator counts — so such a test faults by construction and would be measuring the
    // caller's lifetime bug, not ours. Case (22) covers the part that IS ours, deterministically.

    // ---- (23) a self-finishing worker whose every completion hook throws ----
    // thread_entry() is the std::jthread entry function, so ANY exception escaping it terminates
    // the process — and an escape would also skip the completion publication, stranding
    // wait_until_finished() and every later stop(). The loop is therefore no longer the outermost
    // frame: it returns a reason to a noexcept entry wrapper, the tail runs under that wrapper's
    // catch, and a completion_guard publishes m_worker_done before anything else can go wrong.
    //
    // This drives the whole tail with every hook hostile at once — the FINISH dispatch, EOS
    // propagation, and the resource reap — and asserts the two things that must survive it.
    {
        class hostile_finish_comp : public component {
        public:
            explicit hostile_finish_comp(std::string_view id) : component(id) { add_port(out); }
            auto process() -> retval override { return retval::FINISH; }
            auto on_finished(finish_reason) -> void override { throw std::runtime_error("on_finished threw"); }
            auto on_worker_stop() -> void override { throw std::runtime_error("on_worker_stop threw"); }
            output_port<immutable_buffer<float>> out{"out"};
            component::auto_stop m_auto_stop{*this};
        };

        hostile_finish_comp c{"hostilefinish"};
        c.start();
        // The decisive assertion: completion is published even though every hook on the way out
        // threw. If an exception escaped instead, this hangs (or the process is already dead).
        check(c.wait_until_finished(std::chrono::seconds(5)),
              "worker exit: completion was published despite every completion hook throwing");
        check(c.is_finished(), "worker exit: the component reports itself finished");
        check(c.finished_reason() == finish_reason::completed,
              "worker exit: a FINISH return is still reported as an orderly completion");
        // ...and the component is still stoppable afterwards, i.e. nothing was left half-torn-down.
        check(c.try_stop(std::chrono::seconds(5)), "worker exit: a later stop still completes");
    }

    // ---- (24) stacked pauses must not clobber the saved input depths with 0 ----
    // A worker error give-up pauses the inputs (depth -> 0) with no matching resume. The
    // natural operator remediation — disable, then re-enable — pauses AGAIN; if that second
    // pause overwrites the saved depth with the already-paused 0, the re-enable "restores" 0
    // and the component runs while silently discarding everything.
    {
        class giveup_comp : public component {
        public:
            explicit giveup_comp(std::string_view id) : component(id) { add_port(m_in); }
            auto process() -> retval override {
                if (auto pkt = m_in.try_get()) {
                    if (!m_threw.exchange(true)) {
                        throw std::runtime_error("first packet: boom"); // error_restart_max=0 -> give up
                    }
                    m_got.fetch_add(1, std::memory_order_acq_rel);
                }
                return retval::NOOP;
            }
            input_port<immutable_buffer<float>> m_in{"in", 8};
            std::atomic<bool> m_threw{false};
            std::atomic<int> m_got{0};
            component::auto_stop m_auto_stop{*this};
        };

        giveup_comp c{"pauseclobber"};
        output_port<immutable_buffer<float>> out{"out"};
        check(out.connect(&c.m_in), "pause-clobber: connected the feeding output");
        c.start();
        out.send_data(make_immutable<float>(1), timestamp{}); // first packet -> throw -> give up
        check(c.wait_until_finished(std::chrono::seconds(5)), "pause-clobber: worker gave up on the first error");
        check(c.finished_reason() == finish_reason::error, "pause-clobber: give-up is reported as an error finish");

        // Remediate the way an operator would: disable (second pause), then re-enable.
        c.set_properties(json{{"enabled", false}}, config_type::RUNTIME);
        c.set_properties(json{{"enabled", true}}, config_type::RUNTIME);

        for (int i = 0; i < 4; ++i) {
            out.send_data(make_immutable<float>(1), timestamp{});
        }
        check(wait_until([&] { return c.m_got.load(std::memory_order_acquire) >= 4; }, std::chrono::seconds(2)),
              "pause-clobber: the re-enabled component RECEIVES data (restore did not re-apply the paused 0)");
        c.stop();
    }

    // ---- (25) a direct start() after a give-up must resume the paused inputs ----
    // Same give-up pause, but restarted through the public start() (embedders that do not drive
    // `enabled`). start() must restore the saved depths — a worker running against inputs still
    // at depth 0 silently discards while the component reports running.
    {
        class giveup_comp2 : public component {
        public:
            explicit giveup_comp2(std::string_view id) : component(id) { add_port(m_in); }
            auto process() -> retval override {
                if (auto pkt = m_in.try_get()) {
                    if (!m_threw.exchange(true)) {
                        throw std::runtime_error("first packet: boom");
                    }
                    m_got.fetch_add(1, std::memory_order_acq_rel);
                }
                return retval::NOOP;
            }
            input_port<immutable_buffer<float>> m_in{"in", 8};
            std::atomic<bool> m_threw{false};
            std::atomic<int> m_got{0};
            component::auto_stop m_auto_stop{*this};
        };

        giveup_comp2 c{"giveupstart"};
        output_port<immutable_buffer<float>> out{"out"};
        check(out.connect(&c.m_in), "give-up restart: connected the feeding output");
        c.start();
        out.send_data(make_immutable<float>(1), timestamp{});
        check(c.wait_until_finished(std::chrono::seconds(5)), "give-up restart: worker gave up on the first error");

        c.start(); // direct restart — no reconcile in the path
        for (int i = 0; i < 4; ++i) {
            out.send_data(make_immutable<float>(1), timestamp{});
        }
        check(wait_until([&] { return c.m_got.load(std::memory_order_acquire) >= 4; }, std::chrono::seconds(2)),
              "give-up restart: direct start() resumed the paused inputs (data flows again)");
        c.stop();
    }

    // ---- (26) remove_component() must not hand over a target a live producer still points into ----
    // disconnect() parks the producer's worker; a wedged producer makes that park TIME OUT and
    // throw. The removal must then fail atomically — target re-registered, NOT returned for
    // destruction (the caller dropping the last reference with a live edge into the target's
    // rings is a use-after-free on the producer's next send) — and a retry after the producer
    // quiesces must complete.
    {
        class wedged_producer : public component {
        public:
            explicit wedged_producer(std::string_view id) : component(id) { add_port(m_out); }
            auto process() -> retval override {
                m_entered.store(true, std::memory_order_release);
                while (!m_release.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // ignores park requests
                }
                return retval::NOOP;
            }
            output_port<immutable_buffer<float>> m_out{"out"};
            std::atomic<bool> m_entered{false};
            std::atomic<bool> m_release{false};
            component::auto_stop m_auto_stop{*this};
        };

        application app{"remove_wedged"};
        auto prod = std::make_shared<wedged_producer>("wp");
        auto cons = std::make_shared<consumer_comp>("victim");
        app.add_component(prod);
        app.add_component(cons);
        check(prod->connect("out", cons, "in"), "unremovable: connected wp->victim");
        prod->start();
        check(wait_until([&] { return prod->m_entered.load(std::memory_order_acquire); }, std::chrono::seconds(2)),
              "unremovable: producer is wedged inside process()");

        bool threw = false;
        try {
            auto removed = app.remove_component("victim"); // park times out (~5s) -> must fail atomically
            (void)removed;
        } catch (const std::exception&) {
            threw = true;
        }
        check(threw, "unremovable: removal with a wedged producer FAILS loudly instead of returning the target");
        check(app.components().size() == 2, "unremovable: the target is still REGISTERED (not lost, retryable)");

        // Producer quiesces; the retry must now complete and actually remove.
        prod->m_release.store(true, std::memory_order_release);
        prod->stop();
        auto removed = app.remove_component("victim");
        check(removed != nullptr, "unremovable: retry after the producer quiesced removed the component");
        check(app.components().size() == 1, "unremovable: registry reflects the completed removal");
    }

    if (g_fails != 0) {
        std::fprintf(stderr, "%d containment check(s) FAILED\n", g_fails);
        return 1;
    }
    std::puts("FAILURE-CONTAINMENT TESTS PASSED");
    return 0;
}
