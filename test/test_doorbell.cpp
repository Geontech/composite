// Doorbell: a producer's add_data() wakes an idle consumer worker the instant data
// arrives, instead of the worker waiting out its NOOP backoff. We set the NOOP fallback
// to ~4 s (near the uint32 ns max), let the sink go idle (process()->NOOP -> arm -> sleep),
// then send one packet and assert it is processed promptly (well under the fallback) —
// which can only happen via the doorbell signal, not the timeout. A second case fires
// many drain->idle->send cycles to flush out any lost-wakeup race on the empty->non-empty
// edge. Own main(); explicit checks. Linked against composite::composite.
#include <composite/buffers/buffer.hpp>
#include <composite/core/component.hpp>

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace composite;
using composite::properties::config_type;
using json = composite::properties::json;

class sink_comp : public component {
public:
    input_port<immutable_buffer<float>> in{"in", 16};
    std::atomic<long> processed{0};
    explicit sink_comp(std::string_view id) : component(id) { add_port(in); }
    auto process() -> retval override {
        auto [buf, ts, md] = in.get_data();
        (void)ts;
        (void)md;
        if (buf.size() == 0) { return retval::NOOP; }  // empty ring -> idle
        processed.fetch_add(1, std::memory_order_release);
        return retval::NORMAL;
    }
    component::auto_stop m_auto_stop{*this};
};

static int g_fails = 0;
static void check(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++g_fails; }
}
template <typename Pred>
static bool wait_until(Pred pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) { return true; }
        std::this_thread::yield();
    }
    return pred();
}

int main() {
    spdlog::set_level(spdlog::level::off);
    using namespace std::chrono_literals;

    // NOOP fallback ~4 s (near uint32 ns max). Without the doorbell, a packet arriving
    // just after the worker sleeps would wait out this whole interval before processing.
    constexpr long k_fallback_ns = 4'000'000'000L;

    // ---- (1) the doorbell wakes a single idle worker promptly ----
    {
        sink_comp c{"sink"};
        auto* inp = c.get_port<input_port<immutable_buffer<float>>>("in");
        check(inp != nullptr, "input port present");

        output_port<immutable_buffer<float>> src{"src"};
        check(src.connect(inp), "connect src -> sink.in");

        c.set_properties(json{{"noop_thread_delay", k_fallback_ns}}, config_type::INITIALIZE);
        c.start();
        std::this_thread::sleep_for(150ms);  // let the worker reach armed + sleeping
        check(c.processed.load() == 0, "nothing processed before any data");

        const auto t0 = std::chrono::steady_clock::now();
        src.send_data(make_immutable<float>({1.0F, 2.0F, 3.0F}), timestamp{});
        const bool got = wait_until([&] { return c.processed.load() == 1; }, 1s);
        const auto dt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0).count();
        check(got, "packet processed via doorbell (not the ~4s NOOP fallback)");
        check(dt_ms < 500, "doorbell wake latency well under the NOOP fallback");
        std::printf("doorbell single-hop wake latency: %lld ms\n", (long long)dt_ms);
    }

    // ---- (2) repeated drain->idle->send cycles each wake the worker (no lost wakeup) ----
    {
        sink_comp c{"sink2"};
        auto* inp = c.get_port<input_port<immutable_buffer<float>>>("in");
        output_port<immutable_buffer<float>> src{"src"};
        check(src.connect(inp), "connect src2 -> sink2.in");
        c.set_properties(json{{"noop_thread_delay", k_fallback_ns}}, config_type::INITIALIZE);
        c.start();

        constexpr long N = 200;
        for (long i = 0; i < N; ++i) {
            // Wait for the ring to drain, then give the worker a moment to arm + sleep, so
            // the next send hits the empty->non-empty edge against a (likely) sleeping worker.
            check(wait_until([&] { return inp->pending() == 0; }, 1s), "ring drained between bursts");
            std::this_thread::sleep_for(1ms);
            src.send_data(make_immutable<float>({static_cast<float>(i)}), timestamp{});
            if (!wait_until([&] { return c.processed.load() == i + 1; }, 1s)) {
                check(false, "burst processed via doorbell (per-iteration < 1s << 4s fallback)");
                break;
            }
        }
        check(c.processed.load() == N, "all bursts processed (no lost wakeup over many cycles)");
        std::printf("doorbell burst cycles processed: %ld/%ld\n", c.processed.load(), N);
    }

    // ---- (3) stop() of an idle (armed + sleeping) worker is immediate, not ~m_delay ----
    {
        sink_comp c{"sink3"};
        c.set_properties(json{{"noop_thread_delay", k_fallback_ns}}, config_type::INITIALIZE);
        c.start();
        check(wait_until([&] { return c.is_running(); }, 1s), "sink3 running");
        std::this_thread::sleep_for(150ms);  // worker reaches armed + sleeping on the ~4s fallback
        const auto t0 = std::chrono::steady_clock::now();
        c.stop();
        const auto dt_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0).count();
        check(!c.is_running(), "sink3 stopped");
        check(dt_ms < 500, "stop() wakes the idle worker immediately (not the ~4s NOOP fallback)");
        std::printf("idle-worker stop latency: %lld ms\n", (long long)dt_ms);
    }

    if (g_fails == 0) { std::puts("DOORBELL OK"); }
    return g_fails == 0 ? 0 : 1;
}
