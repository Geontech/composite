// Standalone data-path benchmark (Perf-0) — the baseline that drives the performance leg.
// Measures the costs the redesign targets:
//   (1) per-packet buffer allocation (make_mutable heap path) and pooled acquire/release
//   (2) cross-thread 1:1 hand-off throughput — in TWO variants: with per-packet allocation
//       (end-to-end cost) and with a shared seed buffer (isolates the ring itself)
//   (3) scalar vs direct-batch publish, and 1:N fan-out
//   (4) first-packet wake latency across N component hops (doorbell latency-floor removal)
//   (5) steady-state throughput through N component hops (worker-loop per-hop cost),
//       measured to the LAST PACKET'S ARRIVAL — drain is included but bounded by observation,
//       not by a fixed settle sleep
//   (6) pipeline_component slot-ring throughput at 1/2/4 workers — the machinery FR-1/FR-4
//       change; without this case the 0.6 pipeline work has no before/after evidence
//
// Every case runs a warmup pass and R timed repetitions (--reps, default 3); the human table
// prints medians, and --json writes the versioned artifact (schema in bench_support.hpp) with
// raw samples, spread, and environment metadata for the committed baselines under
// benchmarks/baselines/.
//
// Build: the CMake `bench_datapath` target (links composite::composite). Run:
//   ./bench_datapath [seconds-per-throughput-case] [--reps N] [--json out.json] [--commit sha]

#include "bench_support.hpp"

#include "composite/buffers/buffer.hpp"
#include "composite/buffers/slab_pool.hpp"
#include "composite/core/component.hpp"
#include "composite/core/pipeline_component.hpp"
#include "composite/ports/input_port.hpp"
#include "composite/ports/output_port.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace composite;
using clk = std::chrono::steady_clock;

namespace {

double g_seconds = 1.0;
int g_reps = 3;
bench::reporter g_report;

// Keep the optimizer honest (DoNotOptimize-style barrier; no -Wvolatile).
template <typename T>
void sink_value(const T& v) {
    asm volatile("" : : "r,m"(v) : "memory");
}

/// One warmup invocation (discarded), then R timed repetitions of @p fn (which returns the
/// sample value in the case's unit).
template <typename F>
auto run_samples(F&& fn) -> std::vector<double> {
    (void)fn(); // warmup: touch the paths, fault the pages, spin the threads up once
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(g_reps));
    for (int r = 0; r < g_reps; ++r) {
        samples.push_back(fn());
    }
    return samples;
}

auto median_of(std::vector<double> v) -> double { return bench::compute_stats(std::move(v)).median; }

// ---- (1) allocation cost ------------------------------------------------
void bench_alloc() {
    std::printf("\n== allocation: make_mutable<float>(N) heap path ==\n");
    std::printf("%-12s %14s\n", "elems", "ns/alloc");
    for (std::size_t n : {64u, 256u, 1024u, 4096u, 16384u}) {
        auto samples = run_samples([n] {
            const std::size_t iters = 200000;
            const auto t0 = clk::now();
            for (std::size_t i = 0; i < iters; ++i) {
                auto b = make_mutable<float>(n);
                b[0] = static_cast<float>(i);
                sink_value(b[0]);
            }
            return std::chrono::duration<double>(clk::now() - t0).count() / iters * 1e9;
        });
        std::printf("%-12zu %14.1f\n", n, median_of(samples));
        g_report.add({"alloc.heap", {{"elems", std::to_string(n)}}, "ns/alloc", std::move(samples), {}});
    }
}

// ---- (1b) pooled acquire/release vs heap alloc --------------------------
void bench_pool() {
    std::printf("\n== buffer_pool acquire+release (lock-free Treiber) vs heap make_mutable ==\n");
    std::printf("%-12s %16s %16s\n", "elems", "pool ns/op", "heap ns/op");
    for (std::size_t n : {64u, 256u, 1024u, 4096u}) {
        auto pool = slab_pool<float>::create(n, 64);
        auto pool_samples = run_samples([&pool] {
            const std::size_t iters = 200000;
            const auto t0 = clk::now();
            for (std::size_t i = 0; i < iters; ++i) {
                auto b = pool->acquire(); // lock-free pop; released at scope exit (push)
                if (b) {
                    (*b)[0] = static_cast<float>(i);
                    sink_value((*b)[0]);
                }
            }
            return std::chrono::duration<double>(clk::now() - t0).count() / iters * 1e9;
        });
        auto heap_samples = run_samples([n] {
            const std::size_t iters = 200000;
            const auto t0 = clk::now();
            for (std::size_t i = 0; i < iters; ++i) {
                auto b = make_mutable<float>(n);
                b[0] = static_cast<float>(i);
                sink_value(b[0]);
            }
            return std::chrono::duration<double>(clk::now() - t0).count() / iters * 1e9;
        });
        std::printf("%-12zu %16.1f %16.1f\n", n, median_of(pool_samples), median_of(heap_samples));
        g_report.add({"pool.acquire_release", {{"elems", std::to_string(n)}}, "ns/op", std::move(pool_samples), {}});
    }
}

// ---- (2) 1:1 cross-thread hand-off throughput ---------------------------
// Two variants. "alloc": a fresh mutable buffer per packet — the end-to-end producer cost a
// naive component pays. "shared": one immutable seed share()d per packet (a refcount bump) —
// isolates the ring/handoff itself from the allocator, which the alloc variant conflates.
auto measure_handoff_alloc(std::size_t n) -> double {
    output_port<mutable_buffer<float>> out{"out"};
    input_port<mutable_buffer<float>> in{"in"};
    in.depth(1024);
    out.connect(&in);

    std::atomic<bool> go{false};
    const std::uint64_t count = 2'000'000;
    std::thread cons([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        std::uint64_t got = 0;
        while (got < count) {
            auto [buf, ts, md] = in.get_data();
            if (buf.size() != 0) {
                ++got;
            }
        }
    });
    go.store(true, std::memory_order_release);
    const auto t0 = clk::now();
    for (std::uint64_t i = 0; i < count; ++i) {
        while (in.is_full()) {
            std::this_thread::yield();
        }
        out.send_data(make_mutable<float>(n), timestamp{});
    }
    cons.join();
    return count / std::chrono::duration<double>(clk::now() - t0).count() / 1e6;
}

auto measure_handoff_shared(std::size_t n) -> double {
    output_port<immutable_buffer<float>> out{"out"};
    input_port<immutable_buffer<float>> in{"in"};
    in.depth(1024);
    out.connect(&in);

    std::atomic<bool> go{false};
    const std::uint64_t count = 2'000'000;
    std::thread cons([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        std::uint64_t got = 0;
        while (got < count) {
            auto [buf, ts, md] = in.get_data();
            if (buf.size() != 0) {
                ++got;
            }
        }
    });
    auto seed = make_immutable<float>(n);
    go.store(true, std::memory_order_release);
    const auto t0 = clk::now();
    for (std::uint64_t i = 0; i < count; ++i) {
        while (in.is_full()) {
            std::this_thread::yield();
        }
        out.send_data(seed.share(), timestamp{});
    }
    cons.join();
    return count / std::chrono::duration<double>(clk::now() - t0).count() / 1e6;
}

void bench_handoff_1to1() {
    std::printf("\n== 1:1 hand-off throughput (lock-free SPSC ring, depth=1024) ==\n");
    std::printf("%-12s %18s %18s\n", "elems", "alloc Mpkt/s", "shared Mpkt/s");
    for (std::size_t n : {16u, 64u, 256u, 1024u, 4096u}) {
        auto alloc_samples = run_samples([n] { return measure_handoff_alloc(n); });
        auto shared_samples = run_samples([n] { return measure_handoff_shared(n); });
        std::printf("%-12zu %18.2f %18.2f\n", n, median_of(alloc_samples), median_of(shared_samples));
        g_report.add({"spsc.handoff.alloc",
                      {{"elems", std::to_string(n)}},
                      "Mpkt/s",
                      std::move(alloc_samples),
                      "per-packet make_mutable inside the timed loop (end-to-end producer cost)"});
        g_report.add({"spsc.handoff.shared",
                      {{"elems", std::to_string(n)}},
                      "Mpkt/s",
                      std::move(shared_samples),
                      "share() of one seed buffer per packet (isolates the ring from the allocator)"});
    }
}

// ---- (2b) scalar vs direct batch hand-off --------------------------------
auto measure_batch_handoff(bool batched, std::size_t batch_size, const std::string& metric_id) -> double {
    constexpr std::size_t count = 3'000'000;
    output_port<immutable_buffer<std::uint8_t>> out{"out"};
    input_port<immutable_buffer<std::uint8_t>> in{"in", 4096};
    out.connect(&in);
    out.register_port_metrics(metric_id);
    in.register_port_metrics(metric_id);

    std::atomic<bool> go{false};
    std::thread consumer([&] {
        std::array<decltype(in)::queue_type, 256> received;
        std::size_t n = 0;
        while (!go.load(std::memory_order_acquire)) {
        }
        while (n < count) {
            n += in.get_batch(std::span{received});
        }
    });

    auto seed = make_immutable<std::uint8_t>(64);
    std::vector<immutable_buffer<std::uint8_t>> buffers;
    buffers.reserve(batch_size);
    go.store(true, std::memory_order_release);
    const auto start = clk::now();
    for (std::size_t sent = 0; sent < count;) {
        const auto n = std::min(batch_size, count - sent);
        while (in.available_capacity() < n) {
            std::this_thread::yield();
        }
        buffers.clear();
        for (std::size_t i = 0; i < n; ++i) {
            buffers.emplace_back(seed.share());
        }
        if (batched) {
            out.send_batch(std::span{buffers}, timestamp{});
        } else {
            for (auto& buffer : buffers) {
                out.send_data(std::move(buffer), timestamp{});
            }
        }
        sent += n;
    }
    consumer.join();
    return static_cast<double>(count) / std::chrono::duration<double>(clk::now() - start).count() / 1e6;
}

void bench_batch_handoff() {
    std::printf("\n== immutable 1:1 scalar vs direct batch (registered metrics, batched drain) ==\n");
    std::printf("%-10s %8s %16s\n", "method", "batch", "Mpkt/s");
    int unique = 0;
    for (const auto batch_size : {std::size_t{32}, std::size_t{128}}) {
        for (const bool batched : {false, true}) {
            auto samples = run_samples([&] {
                // Fresh metric id per measurement: register_port_metrics label sets collide
                // across repeated constructions of same-id ports otherwise.
                const auto id = std::string{"bench_"} + (batched ? "batch" : "scalar") + std::to_string(unique++);
                return measure_batch_handoff(batched, batch_size, id);
            });
            std::printf("%-10s %8zu %16.2f\n", batched ? "batch" : "scalar", batch_size, median_of(samples));
            g_report.add({batched ? "spsc.batch.direct" : "spsc.batch.scalar",
                          {{"batch", std::to_string(batch_size)}},
                          "Mpkt/s",
                          std::move(samples),
                          {}});
        }
    }
}

// ---- (3) 1:N fan-out hand-off throughput --------------------------------
auto measure_fanout(std::size_t n, int consumers) -> double {
    output_port<immutable_buffer<float>> out{"out"};
    std::vector<std::unique_ptr<input_port<immutable_buffer<float>>>> ins;
    for (int k = 0; k < consumers; ++k) {
        ins.push_back(std::make_unique<input_port<immutable_buffer<float>>>("in"));
        ins.back()->depth(1024);
        out.connect(ins.back().get());
    }
    const std::uint64_t count = 1'000'000;
    std::atomic<bool> go{false};
    std::vector<std::thread> cons;
    cons.reserve(static_cast<std::size_t>(consumers));
    for (int k = 0; k < consumers; ++k) {
        cons.emplace_back([&, k] {
            while (!go.load(std::memory_order_acquire)) {
            }
            std::uint64_t got = 0;
            while (got < count) {
                auto [buf, ts, md] = ins[static_cast<std::size_t>(k)]->get_data();
                if (buf.size() != 0) {
                    ++got;
                }
            }
        });
    }
    auto seed = make_immutable<float>(n);
    go.store(true, std::memory_order_release);
    const auto t0 = clk::now();
    for (std::uint64_t i = 0; i < count; ++i) {
        // Throttle while ANY consumer's ring is full so the delivery is lossless.
        bool any_full = true;
        while (any_full) {
            any_full = false;
            for (auto& in : ins) {
                if (in->is_full()) {
                    any_full = true;
                    break;
                }
            }
            if (any_full) {
                std::this_thread::yield();
            }
        }
        out.send_data(seed.share(), timestamp{});
    }
    for (auto& c : cons) {
        c.join();
    }
    return count / std::chrono::duration<double>(clk::now() - t0).count() / 1e6;
}

void bench_handoff_1toN() {
    constexpr int N = 4;
    std::printf("\n== 1:%d fan-out hand-off throughput (immutable share, depth=1024) ==\n", N);
    std::printf("%-12s %16s\n", "elems", "Mpkt/s(in)");
    for (std::size_t n : {64u, 256u, 1024u}) {
        auto samples = run_samples([n] { return measure_fanout(n, N); });
        std::printf("%-12zu %16.2f\n", n, median_of(samples));
        g_report.add(
            {"fanout.shared", {{"elems", std::to_string(n)}, {"consumers", std::to_string(N)}}, "Mpkt/s",
             std::move(samples), {}});
    }
}

// ---- component-level fixtures for the hop benchmarks --------------------
// Minimal pass-through component: pop one packet, forward it, else NOOP (so it idles
// on the doorbell). Used to build a chain of started-worker hops.
class bench_passthrough : public component {
public:
    input_port<immutable_buffer<float>> in{"in"};
    output_port<immutable_buffer<float>> out{"out"};
    explicit bench_passthrough(std::string_view id) : component(id) {
        add_port(in);
        add_port(out);
    }
    auto process() -> retval override {
        // Pace via can_send() (reverse doorbell): if the downstream ring is full, leave our
        // input queued (lossless backpressure) and idle on AWAIT_OUTPUT until a slot frees.
        if (!out.can_send()) {
            return retval::AWAIT_OUTPUT;
        }
        auto [buf, ts, md] = in.get_data();
        if (buf.size() == 0) {
            return retval::NOOP;
        }
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
        (void)ts;
        (void)md;
        if (buf.size() == 0) {
            return retval::NOOP;
        }
        received.fetch_add(1, std::memory_order_release);
        return retval::NORMAL;
    }
    component::auto_stop m_auto_stop{*this};
};

// Pass-through pipeline_component: the slot ring, pool claim/publish, and in-order retire are
// the whole measured cost (work() is a move). This is the machinery FR-1 (per-slot context)
// and FR-4 (worker ceiling) modify — the 0.6 before/after evidence for the pipeline leg.
class bench_pipeline : public pipeline_component<immutable_buffer<float>, immutable_buffer<float>> {
public:
    explicit bench_pipeline(std::string_view id, int workers)
        : pipeline_component(id, "in", "out", workers) {}
    auto input() -> input_port<immutable_buffer<float>>& { return in_port(); }

protected:
    auto work(immutable_buffer<float> in, timestamp /*ts*/, const composite::metadata& /*md*/)
        -> immutable_buffer<float> override {
        return in;
    }

private:
    component::auto_stop m_auto_stop{*this}; // MUST be last
};

// ---- (4) first-packet wake latency across H component hops (doorbell) ---
// All hops idle on the doorbell with a deliberately LARGE NOOP backoff (50 ms). Inject one
// packet and time its arrival at the sink; the doorbell wake makes this ~us per hop, where
// the no-doorbell floor would be ~hops * 50 ms. Samples are per-trial (100 after 5 warmups),
// so p95/p99 are meaningful for this case.
void bench_latency_hops() {
    std::printf("\n== first-packet latency across N component hops (doorbell wake; m_delay=50ms) ==\n");
    std::printf("%-8s %14s %14s %14s %20s\n", "hops(N)", "median us", "p95 us", "p99 us", "no-doorbell floor");
    const long m_delay_ns = 50'000'000; // 50 ms NOOP backoff
    for (int N : {1, 2, 4, 8}) {
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
            for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
                pts[i]->connect("out", pts[i + 1], "in");
            }
            pts.back()->connect("out", sink, "in");
        }
        for (auto& p : pts) {
            p->start();
        }
        sink->start();

        constexpr int warmups = 5;
        constexpr int trials = 100;
        std::vector<double> lat_us;
        lat_us.reserve(trials);
        std::uint64_t expect = 0;
        for (int t = 0; t < warmups + trials; ++t) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2)); // let the chain go idle (armed + sleeping)
            ++expect;
            const auto t0 = clk::now();
            inject.send_data(make_immutable<float>({1.0F}), timestamp{});
            const auto deadline = clk::now() + std::chrono::seconds(2);
            while (sink->received.load(std::memory_order_acquire) < expect && clk::now() < deadline) {
            }
            if (sink->received.load(std::memory_order_acquire) < expect) {
                // A timed-out trial is a BROKEN measurement, not a 2-second sample.
                bench::fail("latency.hops(" + std::to_string(N) + "): trial " + std::to_string(t) +
                            " timed out (packet never arrived)");
                break;
            }
            if (t >= warmups) {
                lat_us.push_back(std::chrono::duration<double>(clk::now() - t0).count() * 1e6);
            }
        }
        const auto s = bench::compute_stats(lat_us);
        std::printf("%-8d %14.1f %14.1f %14.1f %17dms\n", N, s.median, s.p95, s.p99, N * 50);
        g_report.add({"latency.hops", {{"hops", std::to_string(N)}}, "us", std::move(lat_us),
                      "per-trial samples (100 trials after 5 warmups)"});
        for (auto& p : pts) {
            p->stop();
        }
        sink->stop();
    }
}

// ---- (5) steady-state throughput through an N-component chain -----------
// Shared-seed injection (no per-packet allocation), lossless AWAIT_OUTPUT pacing, and the
// elapsed window closes at the LAST PACKET'S OBSERVED ARRIVAL: drain is part of the measured
// pipeline cost, but bounded by observation instead of a fixed settle sleep that used to sit
// inside the elapsed time whether or not anything was still in flight.
auto measure_chain_throughput(int hops) -> double {
    std::vector<std::shared_ptr<bench_passthrough>> pts;
    for (int i = 0; i < hops - 1; ++i) {
        pts.push_back(std::make_shared<bench_passthrough>("pt" + std::to_string(i)));
    }
    auto sink = std::make_shared<bench_sink>("sink");
    output_port<immutable_buffer<float>> inject{"inject"};
    input_port<immutable_buffer<float>>* head = pts.empty() ? &sink->in : &pts.front()->in;
    if (pts.empty()) {
        inject.connect(&sink->in);
    } else {
        inject.connect(&pts.front()->in);
        for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
            pts[i]->connect("out", pts[i + 1], "in");
        }
        pts.back()->connect("out", sink, "in");
    }
    for (auto& p : pts) {
        p->start();
    }
    sink->start();

    auto seed = make_immutable<float>({1.0F});
    const auto t0 = clk::now();
    std::uint64_t sent = 0;
    const auto send_deadline = t0 + std::chrono::duration<double>(g_seconds);
    while (clk::now() < send_deadline) {
        for (int b = 0; b < 256; ++b) {
            while (head->is_full()) {
                std::this_thread::yield();
            } // first ring: single producer, no drops here
            inject.send_data(seed.share(), timestamp{});
            ++sent;
        }
    }
    // Drain to the last packet, observed: the chain is lossless, so received reaches sent.
    const auto drain_deadline = clk::now() + std::chrono::seconds(5);
    while (sink->received.load(std::memory_order_acquire) < sent && clk::now() < drain_deadline) {
    }
    const auto dt = std::chrono::duration<double>(clk::now() - t0).count();
    const auto recv = sink->received.load(std::memory_order_acquire);
    for (auto& p : pts) {
        p->stop();
    }
    sink->stop();
    if (recv != sent) {
        bench::fail("chain.throughput(" + std::to_string(hops) + " hops): lost " + std::to_string(sent - recv) +
                    " of " + std::to_string(sent) + " packets — lossless pacing broke");
    }
    return static_cast<double>(recv) / dt / 1e6;
}

void bench_throughput_hops() {
    std::printf("\n== steady-state throughput through N component hops (lossless, drain-to-last) ==\n");
    std::printf("%-8s %14s\n", "hops(N)", "Mpkt/s(recv)");
    for (int N : {1, 2, 4}) {
        auto samples = run_samples([N] { return measure_chain_throughput(N); });
        std::printf("%-8d %14.2f\n", N, median_of(samples));
        g_report.add({"chain.throughput", {{"hops", std::to_string(N)}}, "Mpkt/s", std::move(samples),
                      "shared-seed injection; elapsed ends at the last packet's observed arrival"});
    }
}

// ---- (6) pipeline_component slot-ring throughput -------------------------
auto measure_pipeline_throughput(int workers) -> double {
    auto pipe = std::make_shared<bench_pipeline>("pipe", workers);
    auto sink = std::make_shared<bench_sink>("sink");
    output_port<immutable_buffer<float>> inject{"inject"};
    inject.connect(&pipe->input());
    pipe->connect("out", sink, "in");
    pipe->start();
    sink->start();

    auto seed = make_immutable<float>({1.0F});
    const auto t0 = clk::now();
    std::uint64_t sent = 0;
    const auto send_deadline = t0 + std::chrono::duration<double>(g_seconds);
    while (clk::now() < send_deadline) {
        for (int b = 0; b < 256; ++b) {
            while (pipe->input().is_full()) {
                std::this_thread::yield();
            }
            inject.send_data(seed.share(), timestamp{});
            ++sent;
        }
    }
    const auto drain_deadline = clk::now() + std::chrono::seconds(5);
    while (sink->received.load(std::memory_order_acquire) < sent && clk::now() < drain_deadline) {
    }
    const auto dt = std::chrono::duration<double>(clk::now() - t0).count();
    const auto recv = sink->received.load(std::memory_order_acquire);
    pipe->stop();
    sink->stop();
    if (recv != sent) {
        bench::fail("pipeline.throughput(" + std::to_string(workers) + " workers): lost " +
                    std::to_string(sent - recv) + " of " + std::to_string(sent) + " packets");
    }
    return static_cast<double>(recv) / dt / 1e6;
}

void bench_pipeline_hops() {
    std::printf("\n== pipeline_component throughput (pass-through work(); slot ring + ordered retire) ==\n");
    std::printf("%-10s %14s\n", "workers", "Mpkt/s(recv)");
    for (int workers : {1, 2, 4}) {
        auto samples = run_samples([workers] { return measure_pipeline_throughput(workers); });
        std::printf("%-10d %14.2f\n", workers, median_of(samples));
        g_report.add({"pipeline.throughput", {{"workers", std::to_string(workers)}}, "Mpkt/s", std::move(samples),
                      "work() is a move: measures ingest/claim/retire machinery, not compute"});
    }
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0); // line-buffer so progress streams
    std::string json_path;
    std::string framework_commit;
    std::string harness_commit;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--json" && i + 1 < argc) {
            json_path = argv[++i];
        } else if ((arg == "--framework-commit" || arg == "--commit") && i + 1 < argc) {
            framework_commit = argv[++i];
        } else if (arg == "--harness-commit" && i + 1 < argc) {
            harness_commit = argv[++i];
        } else if (arg == "--reps" && i + 1 < argc) {
            g_reps = std::max(1, std::atoi(argv[++i]));
        } else if (!arg.starts_with("--")) {
            g_seconds = std::atof(argv[i]);
        }
    }

    std::printf("composite data-path baseline  (sizeof mutable_buffer<float>=%zu, reps=%d)\n",
                sizeof(mutable_buffer<float>), g_reps);
    bench_alloc();
    bench_pool();
    bench_handoff_1to1();
    bench_batch_handoff();
    bench_handoff_1toN();
    bench_latency_hops();
    bench_throughput_hops();
    bench_pipeline_hops();

    if (bench::g_failed) {
        std::fprintf(stderr, "\nFAILED: measurement invariants broke; no artifact written.\n");
        return 1;
    }
    if (!json_path.empty()) {
        auto meta = bench::capture_environment(std::move(framework_commit), std::move(harness_commit));
        meta["harness"] = "bench_datapath";
        meta["seconds_per_case"] = g_seconds;
        meta["repetitions"] = g_reps;
        if (!g_report.write(json_path, meta)) {
            return 1;
        }
        std::printf("\nwrote %s\n", json_path.c_str());
    }
    std::printf("\ndone.\n");
    return 0;
}
