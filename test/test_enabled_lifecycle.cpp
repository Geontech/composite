// `enabled` as a framework spec/status virtual. Proves: a RUNTIME write
// IS the start/stop action; the read reports desired (spec) AND running (observed) with
// no stale mirror (desync unrepresentable); re-enabling after a DIRECT stop restarts
// even though the desired value did not change (the re-enable no-op trap is gone); the
// legacy two-step set_properties(INITIALIZE)+apply_lifecycle_changes() reconcile still
// works; and get_property<bool>("enabled") shims to the desired state. Own main();
// explicit checks. Linked against composite::composite.
#include <composite/buffers/buffer.hpp>
#include <composite/core/application.hpp>
#include <composite/core/component.hpp>
#include <composite/ports/input_port.hpp>
#include <composite/ports/output_port.hpp>

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
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

// Consumer with an input at a known nonzero depth; records the first sequence value it sees.
// For the initially-disabled gating regressions: its input must be at depth 0 from the first
// disabled reconcile, and must never retain pre-enable packets.
class gated_sink : public component {
public:
    explicit gated_sink(std::string_view id) : component(id) { add_port(&m_in); }
    auto process() -> retval override {
        auto pkt = m_in.try_get();
        if (!pkt) {
            return retval::NOOP;
        }
        auto& [buf, ts, md] = *pkt;
        if (!buf.as_span().empty()) {
            std::int64_t expected = -1;
            m_first_seq.compare_exchange_strong(expected, buf.as_span()[0]);
        }
        m_received.fetch_add(1, std::memory_order_relaxed);
        return retval::NORMAL;
    }
    static constexpr std::size_t k_depth = 64;
    input_port<immutable_buffer<std::int64_t>> m_in{"in", k_depth};
    std::atomic<std::int64_t> m_first_seq{-1};
    std::atomic<long> m_received{0};
    component::auto_stop m_auto_stop{*this};
};

// Producer that sends BLINDLY (no can_send pacing): against a depth-0 consumer every send must
// be REJECTED at admission, not retained — which is exactly what the gating fix establishes.
// on_worker_start() sends a burst SYNCHRONOUSLY during this component's own reconcile: in a
// single-pass application::start() that burst lands strictly before a later-registered disabled
// consumer is reconciled, making the startup-ordering window DETERMINISTIC instead of a
// scheduling race. (Single-producer rule holds: on_worker_start() is sequenced before the
// worker thread spawns, so the two never send concurrently.)
class blind_pusher : public component {
public:
    explicit blind_pusher(std::string_view id) : component(id) { add_port(&m_out); }
    auto on_worker_start() -> void override {
        for (int i = 0; i < 32; ++i) {
            send_one();
        }
    }
    auto process() -> retval override {
        send_one();
        return retval::NORMAL;
    }
    output_port<immutable_buffer<std::int64_t>> m_out{"out"};
    std::atomic<std::int64_t> m_sent{0};

private:
    auto send_one() -> void {
        auto b = make_immutable<std::int64_t>({m_sent.load(std::memory_order_relaxed)});
        m_out.send_data(std::move(b), timestamp{});
        m_sent.fetch_add(1, std::memory_order_release);
    }

public:
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

    // ---- (A) 0.5.2 regression: initially disabled component pauses WITHOUT ever starting ----
    // An initially disabled component matched neither reconcile branch (want=false,
    // has_handle=false), so its inputs stayed open at their configured depth and retained
    // whatever an enabled upstream sent. Disabled must mean depth 0 from the first reconcile.
    {
        gated_sink c{"gate_a"};
        check(c.m_in.depth() == gated_sink::k_depth, "A: constructed at the configured depth");
        c.set_properties(json{{"enabled", false}}, config_type::INITIALIZE);
        c.apply_lifecycle_changes();
        check(!c.is_running(), "A: no worker after disabled reconcile");
        check(!c.is_enabled(), "A: desired enabled == false");
        check(c.m_in.depth() == 0, "A: never-started disabled component has input depth 0");
        c.set_properties(json{{"enabled", true}}, config_type::RUNTIME);
        check(wait_until([&] { return c.is_running(); }, 2s), "A: later enable starts the worker");
        check(c.m_in.depth() == gated_sink::k_depth, "A: enable restored the ORIGINAL depth");
        c.stop();
    }

    // ---- (C) repeated disabled reconciliation preserves the saved depth ----
    // Pins the no-overwrite rule: reconciling a never-started disabled component again must not
    // clobber the saved original depth with the already-paused 0.
    {
        gated_sink c{"gate_c"};
        c.set_properties(json{{"enabled", false}}, config_type::INITIALIZE);
        for (int i = 0; i < 3; ++i) {
            c.apply_lifecycle_changes();
            check(c.m_in.depth() == 0, "C: depth stays 0 across repeated disabled reconciles");
        }
        c.set_properties(json{{"enabled", true}}, config_type::RUNTIME);
        check(wait_until([&] { return c.is_running(); }, 2s), "C: enable starts after repeated reconciles");
        check(c.m_in.depth() == gated_sink::k_depth, "C: original depth restored (not the paused 0)");
        c.stop();
    }

    // ---- (B) application startup gates disabled consumers BEFORE producers ----
    // Deliberately hostile order: the enabled producer is registered (and reconciles) before the
    // disabled consumer, so a single-pass start would let it send into a still-open input.
    // application::start() now gates every desired-disabled component in a first pass.
    {
        application app{"gating"};
        auto producer = std::make_shared<blind_pusher>("a_pusher"); // sorts/inserts before the sink
        auto consumer = std::make_shared<gated_sink>("z_sink");
        check(app.add_component(producer), "B: producer registered");
        check(app.add_component(consumer), "B: consumer registered");
        check(producer->connect("out", consumer, "in"), "B: producer -> consumer connected");
        consumer->set_properties(json{{"enabled", false}}, config_type::INITIALIZE);

        app.start();
        check(wait_until([&] { return producer->m_sent.load(std::memory_order_acquire) > 200; }, 5s),
              "B: producer is live and sending");
        check(!consumer->is_running(), "B: disabled consumer never started");
        check(consumer->m_in.depth() == 0, "B: disabled consumer input depth is 0 through startup");
        check(consumer->m_in.pending() == 0, "B: disabled consumer retained NOTHING");
        check(consumer->m_in.stats().packets_dropped() > 0, "B: blind sends were rejected, not retained");

        // Re-enable: original depth restored, and ONLY post-enable data is consumed. The seq
        // captured BEFORE the enable bounds it: everything sent earlier was rejected at depth 0.
        const std::int64_t seq_at_enable = producer->m_sent.load(std::memory_order_acquire);
        consumer->set_properties(json{{"enabled", true}}, config_type::RUNTIME);
        check(wait_until([&] { return consumer->is_running(); }, 2s), "B: consumer started on enable");
        check(consumer->m_in.depth() == gated_sink::k_depth, "B: enable restored the original depth");
        check(wait_until([&] { return consumer->m_received.load(std::memory_order_acquire) > 0; }, 5s),
              "B: consumer receives after enable");
        const auto first = consumer->m_first_seq.load(std::memory_order_acquire);
        check(first >= seq_at_enable, "B: first consumed packet was sent AFTER the enable (no stale backlog)");
        app.stop();
    }

    if (g_fails != 0) {
        std::fprintf(stderr, "%d enabled-lifecycle check(s) FAILED\n", g_fails);
        return 1;
    }
    std::puts("ENABLED SPEC/STATUS LIFECYCLE TESTS PASSED");
    return 0;
}
