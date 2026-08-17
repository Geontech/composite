/*
 * Copyright (C) 2024-2026 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

// AF2 failure-containment contract (v0.5 freeze): a user callback must never unwind across
// a worker, destructor, or graph-teardown boundary, and a committed property batch stays
// committed when its reaction fails. Before this suite, a throwing config<T> on_apply
// escaped the worker loop-top (std::terminate), escaped ~component / ~auto_stop
// (std::terminate from a destructor), and a failed stop() inside
// application::remove_component() skipped the disconnect loops that make the removal safe.
//
// Also covers AF1: input_port::depth() must not replace the physical ring while a producer
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

    // ---- (7) AF1: depth() must not replace the ring while a producer is connected ----
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

    // ---- (8) AF1: power-of-two rounding saturates instead of spinning forever ----
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

    // ---- (9) AF7: has_property() is the globals filter's predicate ----
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

    // ---- (10) AF2-D: finish_error() carries the detail behind finish_reason::error ----
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

    // ---- (13) AF3-I: partial admission must not over-count transferred packets ----
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

    if (g_fails != 0) {
        std::fprintf(stderr, "%d containment check(s) FAILED\n", g_fails);
        return 1;
    }
    std::puts("AF1/AF2 FAILURE-CONTAINMENT TESTS PASSED");
    return 0;
}
