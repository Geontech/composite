// Standalone data-path benchmark (Perf-0) — the baseline that drives the
// performance leg (ASSESSMENT §4.C / §6). Measures the costs the redesign targets:
//   (1) per-packet buffer allocation (make_mutable / make_immutable heap path)
//   (2) cross-thread 1:1 and 1:N port hand-off throughput (the mutex+CV+deque
//       queue the lock-free SPSC ring is meant to replace)
//   (4) first-packet wake latency across N component hops — demonstrates the
//       doorbell latency-floor removal (~us/hop vs the N*m_delay NOOP floor)
//   (5) steady-state throughput through N component hops (worker-loop per-hop cost)
//
// Build (no Catch2; -O3 so we measure the real cost). Cases (4)/(5) use components, so
// link the one non-header-only TU (the metrics registry):
//   g++ -std=c++20 -O3 -DNDEBUG -I include -I <spdlog/include> -I <nlohmann-json/include>
//       test/bench_datapath.cpp src/registry.cpp -pthread -o bench
// (the CMake `bench_datapath` target links composite::composite, which already supplies it.)
// Run: ./bench   (optional arg: seconds per throughput case, default 1)
//
// Numbers are intentionally printed as a table so a before/after diff is obvious.

#include "composite/buffers/buffer.hpp"
#include "composite/buffers/slab_pool.hpp"
#include "composite/core/component.hpp"
#include "composite/ports/input_port.hpp"
#include "composite/ports/output_port.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace composite;
using clk = std::chrono::steady_clock;

namespace {

double g_seconds = 1.0;

// Keep the optimizer honest (DoNotOptimize-style barrier; no -Wvolatile).
template <typename T> void sink_value(const T& v) {
    asm volatile("" : : "r,m"(v) : "memory");
}

// ---- (1) allocation cost ------------------------------------------------
void bench_alloc() {
    std::printf("\n== allocation: make_mutable<float>(N) heap path ==\n");
    std::printf("%-12s %14s %16s\n", "elems", "ns/alloc", "Malloc/s");
    for (std::size_t n : {64u, 256u, 1024u, 4096u, 16384u}) {
        const std::size_t iters = 200000;
        auto t0 = clk::now();
        for (std::size_t i = 0; i < iters; ++i) {
            auto b = make_mutable<float>(n);
            b[0] = static_cast<float>(i);
            sink_value(b[0]);
        }
        auto dt = std::chrono::duration<double>(clk::now() - t0).count();
        double ns = dt / iters * 1e9;
        std::printf("%-12zu %14.1f %16.2f\n", n, ns, iters / dt / 1e6);
    }
}

// ---- (1b) pooled acquire/release vs heap alloc --------------------------
void bench_pool() {
    std::printf("\n== buffer_pool acquire+release (lock-free Treiber) vs heap make_mutable ==\n");
    std::printf("%-12s %16s %16s\n", "elems", "pool ns/op", "heap ns/op");
    for (std::size_t n : {64u, 256u, 1024u, 4096u}) {
        auto pool = slab_pool<float>::create(n, 64);
        const std::size_t iters = 200000;
        auto t0 = clk::now();
        for (std::size_t i = 0; i < iters; ++i) {
            auto b = pool->acquire();             // lock-free pop
            if (b) { (*b)[0] = static_cast<float>(i); sink_value((*b)[0]); }
        }                                          // released here (lock-free push)
        auto pool_ns = std::chrono::duration<double>(clk::now() - t0).count() / iters * 1e9;
        t0 = clk::now();
        for (std::size_t i = 0; i < iters; ++i) {
            auto b = make_mutable<float>(n);
            b[0] = static_cast<float>(i);
            sink_value(b[0]);
        }
        auto heap_ns = std::chrono::duration<double>(clk::now() - t0).count() / iters * 1e9;
        std::printf("%-12zu %16.1f %16.1f\n", n, pool_ns, heap_ns);
    }
}

// ---- (2) 1:1 cross-thread hand-off throughput ---------------------------
// Producer pushes `count` packets (spinning while the bounded queue is full,
// single producer => no drops); consumer blocks-receives until it has them all.
void bench_handoff_1to1() {
    std::printf("\n== 1:1 hand-off throughput (lock-free SPSC ring, depth=1024) ==\n");
    std::printf("%-12s %16s %16s\n", "elems", "Mpkt/s", "ns/pkt");
    for (std::size_t n : {16u, 64u, 256u, 1024u, 4096u}) {
        output_port<mutable_buffer<float>> out{"out"};
        input_port<mutable_buffer<float>> in{"in"};
        in.depth(1024);
        out.connect(&in);

        std::atomic<bool> go{false};
        std::atomic<std::uint64_t> received{0};
        // Target a fixed wall-clock budget: estimate count from a quick warmup.
        const std::uint64_t count = 2'000'000;

        std::thread cons([&] {
            while (!go.load(std::memory_order_acquire)) { /* spin */ }
            std::uint64_t got = 0;
            while (got < count) {
                auto [buf, ts, md] = in.get_data();
                if (buf.size() != 0) { ++got; }
            }
            received.store(got, std::memory_order_release);
        });

        go.store(true, std::memory_order_release);
        auto t0 = clk::now();
        for (std::uint64_t i = 0; i < count; ++i) {
            while (in.is_full()) { std::this_thread::yield(); }
            auto b = make_mutable<float>(n);
            out.send_data(std::move(b), timestamp{});
        }
        cons.join();
        auto dt = std::chrono::duration<double>(clk::now() - t0).count();
        std::printf("%-12zu %16.2f %16.1f\n", n, count / dt / 1e6, dt / count * 1e9);
        (void)received;
    }
}

// ---- (3) 1:N fan-out hand-off throughput --------------------------------
void bench_handoff_1toN() {
    constexpr int N = 4;
    std::printf("\n== 1:%d fan-out hand-off throughput (immutable share, depth=1024) ==\n", N);
    std::printf("%-12s %16s\n", "elems", "Mpkt/s(in)");
    for (std::size_t n : {64u, 256u, 1024u}) {
        output_port<immutable_buffer<float>> out{"out"};
        std::vector<std::unique_ptr<input_port<immutable_buffer<float>>>> ins;
        for (int k = 0; k < N; ++k) {
            ins.push_back(std::make_unique<input_port<immutable_buffer<float>>>("in"));
            ins.back()->depth(1024);
            out.connect(ins.back().get());
        }
        const std::uint64_t count = 1'000'000;
        std::atomic<bool> go{false};
        std::vector<std::thread> cons;
        for (int k = 0; k < N; ++k) {
            cons.emplace_back([&, k] {
                while (!go.load(std::memory_order_acquire)) {}
                std::uint64_t got = 0;
                while (got < count) {
                    auto [buf, ts, md] = ins[k]->get_data();
                    if (buf.size() != 0) { ++got; }
                }
            });
        }
        go.store(true, std::memory_order_release);
        auto t0 = clk::now();
        for (std::uint64_t i = 0; i < count; ++i) {
            // Throttle while ANY consumer's ring is full, so no packet is dropped
            // and every consumer receives all `count` packets (else it never
            // reaches its target and its thread spins forever).
            bool any_full = true;
            while (any_full) {
                any_full = false;
                for (auto& in : ins) { if (in->is_full()) { any_full = true; break; } }
                if (any_full) { std::this_thread::yield(); }
            }
            auto b = make_immutable<float>(n);
            out.send_data(std::move(b), timestamp{});
        }
        for (auto& c : cons) { c.join(); }
        auto dt = std::chrono::duration<double>(clk::now() - t0).count();
        std::printf("%-12zu %16.2f\n", n, count / dt / 1e6);
    }
}

// ---- component-level fixtures for the hop benchmarks --------------------
// Minimal pass-through component: pop one packet, forward it, else NOOP (so it idles
// on the doorbell). Used to build a chain of started-worker hops.
class bench_passthrough : public component {
public:
    input_port<immutable_buffer<float>> in{"in"};
    output_port<immutable_buffer<float>> out{"out"};
    explicit bench_passthrough(std::string_view id) : component(id) { add_port(in); add_port(out); }
    auto process() -> retval override {
        // Pace via can_send() (reverse doorbell): if the downstream ring is full, leave our
        // input queued (lossless backpressure) and idle on AWAIT_OUTPUT until a slot frees.
        if (!out.can_send()) { return retval::AWAIT_OUTPUT; }
        auto [buf, ts, md] = in.get_data();
        if (buf.size() == 0) { return retval::NOOP; }
        out.send_data(std::move(buf), ts, md);
        return retval::NORMAL;
    }
    component::auto_stop m_auto_stop{*this};
};

// Sink: count received packets (the bench polls this to detect arrival).
class bench_sink : public component {
public:
    input_port<immutable_buffer<float>> in{"in"};
    std::atomic<std::uint64_t> received{0};
    explicit bench_sink(std::string_view id) : component(id) { add_port(in); }
    auto process() -> retval override {
        auto [buf, ts, md] = in.get_data();
        (void)ts; (void)md;
        if (buf.size() == 0) { return retval::NOOP; }
        received.fetch_add(1, std::memory_order_release);
        return retval::NORMAL;
    }
    component::auto_stop m_auto_stop{*this};
};

// ---- (4) first-packet wake latency across H component hops (doorbell) ---
// Build inject -> c0 -> c1 -> ... -> sink, all started workers idling on the doorbell with
// a deliberately LARGE NOOP backoff (m_delay = 50 ms). Inject one packet and time its
// arrival at the sink. The doorbell wakes each idle hop the instant data arrives, so the
// measured latency is ~us per hop; WITHOUT the doorbell each hop would wait out its NOOP
// backoff, so the floor would be ~hops * 50 ms. The contrast is the doorbell latency-floor removal.
void bench_latency_hops() {
    std::printf("\n== first-packet latency across N component hops (doorbell wake; m_delay=50ms) ==\n");
    std::printf("doorbell wakes each idle hop on arrival; without it the floor would be ~N*50ms.\n");
    std::printf("%-8s %14s %14s %14s %20s\n", "hops(N)", "median us", "p99 us", "max us", "no-doorbell floor");
    const long m_delay_ns = 50'000'000;  // 50 ms NOOP backoff
    for (int N : {1, 2, 4, 8}) {
        // N components: [0..N-2] pass-through, [N-1] sink. inject feeds component 0.
        std::vector<std::shared_ptr<bench_passthrough>> pts;
        for (int i = 0; i < N - 1; ++i) {
            auto p = std::make_shared<bench_passthrough>("pt" + std::to_string(i));
            p->set_properties(properties::json{{"noop_thread_delay", m_delay_ns}}, properties::config_type::INITIALIZE);
            pts.push_back(p);
        }
        auto sink = std::make_shared<bench_sink>("sink");
        sink->set_properties(properties::json{{"noop_thread_delay", m_delay_ns}}, properties::config_type::INITIALIZE);
        output_port<immutable_buffer<float>> inject{"inject"};
        if (pts.empty()) {
            inject.connect(&sink->in);
        } else {
            inject.connect(&pts.front()->in);
            for (std::size_t i = 0; i + 1 < pts.size(); ++i) { pts[i]->connect("out", pts[i + 1], "in"); }
            pts.back()->connect("out", sink, "in");
        }
        for (auto& p : pts) { p->start(); }
        sink->start();

        const int trials = 100;
        std::vector<double> lat_us;
        lat_us.reserve(trials);
        std::uint64_t expect = 0;
        for (int t = 0; t < trials; ++t) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));  // let the chain go idle (armed + sleeping)
            ++expect;
            auto t0 = clk::now();
            inject.send_data(make_immutable<float>({1.0F}), timestamp{});
            const auto deadline = clk::now() + std::chrono::seconds(2);
            while (sink->received.load(std::memory_order_acquire) < expect && clk::now() < deadline) { }
            lat_us.push_back(std::chrono::duration<double>(clk::now() - t0).count() * 1e6);
        }
        std::sort(lat_us.begin(), lat_us.end());
        std::printf("%-8d %14.1f %14.1f %14.1f %17dms\n", N,
                    lat_us[trials / 2], lat_us[(trials * 99) / 100], lat_us.back(), N * 50);
        for (auto& p : pts) { p->stop(); }
        sink->stop();
    }
}

// ---- (5) steady-state throughput through an N-component chain -----------
// Saturated end-to-end Mpkt/s through N started-worker hops — exercises the worker loop
// (park_point + loop-top reaction drain + process + batched yield) the hot-loop tax-cuts target,
// on top of the lock-free ring measured by the 1:1 hand-off case above.
void bench_throughput_hops() {
    std::printf("\n== steady-state throughput through N component hops (achieved end-to-end) ==\n");
    std::printf("the pass-through hops pace via can_send()/AWAIT_OUTPUT (reverse doorbell), so\n");
    std::printf("the chain is LOSSLESS — every hop backpressures upstream and drop%% stays ~0 even\n");
    std::printf("as N grows; ns/pkt is the per-hop worker-loop + handoff cost.\n");
    std::printf("%-8s %14s %14s %10s\n", "hops(N)", "Mpkt/s(recv)", "ns/pkt(recv)", "drop%");
    for (int N : {1, 2, 4}) {
        std::vector<std::shared_ptr<bench_passthrough>> pts;
        for (int i = 0; i < N - 1; ++i) { pts.push_back(std::make_shared<bench_passthrough>("pt" + std::to_string(i))); }
        auto sink = std::make_shared<bench_sink>("sink");
        output_port<immutable_buffer<float>> inject{"inject"};
        input_port<immutable_buffer<float>>* head = pts.empty() ? &sink->in : &pts.front()->in;
        if (pts.empty()) {
            inject.connect(&sink->in);
        } else {
            inject.connect(&pts.front()->in);
            for (std::size_t i = 0; i + 1 < pts.size(); ++i) { pts[i]->connect("out", pts[i + 1], "in"); }
            pts.back()->connect("out", sink, "in");
        }
        for (auto& p : pts) { p->start(); }
        sink->start();

        auto t0 = clk::now();
        std::uint64_t sent = 0;
        const auto deadline = t0 + std::chrono::duration<double>(g_seconds);
        while (clk::now() < deadline) {
            for (int b = 0; b < 256; ++b) {
                while (head->is_full()) { std::this_thread::yield(); }  // first ring: single producer, no drops here
                inject.send_data(make_immutable<float>({static_cast<float>(sent)}), timestamp{});
                ++sent;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));  // brief settle for in-flight packets
        auto dt = std::chrono::duration<double>(clk::now() - t0).count();
        const double recv = static_cast<double>(sink->received.load(std::memory_order_acquire));
        const double drop_pct = sent > 0 ? 100.0 * (static_cast<double>(sent) - recv) / static_cast<double>(sent) : 0.0;
        std::printf("%-8d %14.2f %14.1f %9.1f%%\n", N, recv / dt / 1e6, recv > 0 ? dt / recv * 1e9 : 0.0, drop_pct);
        for (auto& p : pts) { p->stop(); }
        sink->stop();
    }
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);  // line-buffer so progress streams
    if (argc > 1) { g_seconds = std::atof(argv[1]); }
    std::printf("composite data-path baseline  (sizeof mutable_buffer<float>=%zu)\n",
                sizeof(mutable_buffer<float>));
    bench_alloc();
    bench_pool();
    bench_handoff_1to1();
    bench_handoff_1toN();
    bench_latency_hops();
    bench_throughput_hops();
    std::printf("\ndone.\n");
    return 0;
}
