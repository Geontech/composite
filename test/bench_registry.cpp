/*
 * Copyright (C) 2026 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

// Metrics-registry profile (Baseline-0) — the evidence base for PERF-5 (registry indexing /
// snapshot filtering) and for sizing the Prometheus exposition path. Measures, at 100 / 1,000
// / 10,000 registered metrics with a realistic shape (a handful of metric names fanned out
// across component_id label sets, the way ports and lifecycle metrics actually register):
//
//   - registration cost (ns/metric)          — the O(N^2)-scan suspicion at startup
//   - snapshot_all latency + payload size    — the GET /app/metrics and SSE tick cost
//   - snapshot_by_prefix / by_label latency  — the filtered forms (currently filter-after-copy)
//   - teardown by component label (ns/metric)— component destruction cost
//   - retained heap of one live snapshot_all result (glibc mallinfo2 in-use delta)
//
// Uses registry::clear() (COMPOSITE_TESTING, defined for the whole test tree) to reset the
// singleton between counts. Run:
//   ./bench_registry [--reps N] [--json out.json] [--commit sha]

#include "bench_support.hpp"

#include "composite/metrics/registry.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <malloc.h>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using composite::metrics::registry;
using clk = std::chrono::steady_clock;

namespace {

int g_reps = 3;
bench::reporter g_report;

constexpr int k_metrics_per_component = 10; ///< the mixed set below: what a real component registers

/// Register a REPRESENTATIVE mixture per component — the shape lifecycle + port metrics
/// actually take (few global names, many component_id label sets, and histograms whose
/// boundary/bucket vectors dominate snapshot payload): 4 counters, 1 updown, 2 gauges,
/// 3 histograms (two pow2, one custom-boundary). An all-counter population undersold both
/// the snapshot copy cost and the Prometheus exposition sizing.
auto register_population(int count) -> void {
    auto& reg = registry::instance();
    const int components = count / k_metrics_per_component;
    const std::vector<double> latency_bounds{0.0001, 0.00025, 0.0005, 0.001, 0.0025, 0.005, 0.01, 0.025,
                                             0.05,   0.1,     0.25,  0.5,   1.0,    2.5,   5.0,  10.0};
    for (int c = 0; c < components; ++c) {
        const composite::metrics::labels_t labels{{"component_id", "comp" + std::to_string(c)}};
        reg.create_counter("composite.bench.process_calls", "process() invocations", "1", labels);
        reg.create_counter("composite.bench.packets_in", "packets consumed", "1", labels);
        reg.create_counter("composite.bench.bytes_in", "bytes consumed", "By", labels);
        reg.create_counter("composite.bench.drops", "packets shed", "1", labels);
        reg.create_updown_counter("composite.bench.inflight", "work in flight", "1", labels);
        reg.create_gauge("composite.bench.state", "lifecycle state", "1", labels);
        reg.create_gauge("composite.bench.queue_depth", "ring occupancy", "1", labels);
        reg.create_histogram_pow2("composite.bench.process_time", "process() duration", "us", 20, labels);
        reg.create_histogram_pow2("composite.bench.work_size", "packet size", "By", 12, labels);
        reg.create_histogram("composite.bench.latency", "end-to-end latency", "s", latency_bounds, labels);
    }
}

auto snapshot_payload_bytes(const std::vector<composite::metrics::metric_snapshot>& snaps) -> std::size_t {
    std::size_t bytes = 0;
    for (const auto& s : snaps) {
        bytes += s.name.size() + s.description.size() + s.unit.size();
        for (const auto& [k, v] : s.labels) {
            bytes += k.size() + v.size();
        }
        if (const auto* h = std::get_if<composite::metrics::histogram_snapshot>(&s.value)) {
            bytes += (h->boundaries.size() + h->bucket_counts.size()) * 8;
        } else {
            bytes += 16; // scalar value order-of-magnitude
        }
    }
    return bytes;
}

auto profile_count(int count) -> void {
    auto& reg = registry::instance();
    const auto params = std::map<std::string, std::string>{{"metrics", std::to_string(count)}};
    const int components = count / k_metrics_per_component;

    // Registration + teardown are destructive, so they sample via full cycles.
    std::vector<double> reg_ns;
    std::vector<double> teardown_ns;
    for (int r = 0; r < g_reps + 1; ++r) { // +1: first cycle is the warmup
        reg.clear();
        auto t0 = clk::now();
        register_population(count);
        const auto reg_sample = std::chrono::duration<double>(clk::now() - t0).count() / count * 1e9;
        t0 = clk::now();
        for (int c = 0; c < components; ++c) {
            reg.remove_by_label("component_id", "comp" + std::to_string(c));
        }
        const auto td_sample = std::chrono::duration<double>(clk::now() - t0).count() / count * 1e9;
        if (r > 0) {
            reg_ns.push_back(reg_sample);
            teardown_ns.push_back(td_sample);
        }
    }

    // Read-side cases run against one standing population.
    reg.clear();
    register_population(count);

    std::vector<double> snap_us;
    std::vector<double> prefix_us;
    std::vector<double> label_us;
    std::size_t payload = 0;
    long alloc_delta = -1;
    (void)reg.snapshot_all(); // warmup
    for (int r = 0; r < g_reps; ++r) {
        auto t0 = clk::now();
        auto all = reg.snapshot_all();
        snap_us.push_back(std::chrono::duration<double>(clk::now() - t0).count() * 1e6);
        payload = snapshot_payload_bytes(all);

        t0 = clk::now();
        auto pre = reg.snapshot_by_prefix("composite.bench.packets_in");
        prefix_us.push_back(std::chrono::duration<double>(clk::now() - t0).count() * 1e6);
        if (pre.size() != static_cast<std::size_t>(components)) {
            bench::fail("registry prefix snapshot returned " + std::to_string(pre.size()) + " series, expected " +
                        std::to_string(components));
        }

        t0 = clk::now();
        auto lab = reg.snapshot_by_label("component_id", "comp0");
        label_us.push_back(std::chrono::duration<double>(clk::now() - t0).count() * 1e6);
        if (lab.size() != k_metrics_per_component) {
            bench::fail("registry label snapshot returned " + std::to_string(lab.size()) + " series, expected " +
                        std::to_string(k_metrics_per_component));
        }
    }
    if (const auto total = reg.metric_count(); total != static_cast<std::size_t>(count)) {
        bench::fail("registry population is " + std::to_string(total) + " series, expected " + std::to_string(count));
    }

#if defined(__GLIBC__)
    {
        // RETAINED heap of one live snapshot_all result (glibc arena in-use delta while the
        // result is alive) — NOT allocation volume or churn, which would need allocator
        // instrumentation: freed-and-reused blocks never show here. Named accordingly in the
        // artifact. Still sizes the interning win PERF-5 proposes (the retained copies are
        // exactly what interning removes).
        const auto before = mallinfo2().uordblks;
        auto all = reg.snapshot_all();
        const auto after = mallinfo2().uordblks;
        alloc_delta = static_cast<long>(after) - static_cast<long>(before);
    }
#endif

    std::printf("%-10d %12.1f %12.1f %14.1f %14.1f %12.1f %12zu %12ld\n", count,
                bench::compute_stats(reg_ns).median, bench::compute_stats(teardown_ns).median,
                bench::compute_stats(snap_us).median, bench::compute_stats(prefix_us).median,
                bench::compute_stats(label_us).median, payload, alloc_delta);

    g_report.add({"registry.register", params, "ns/metric", std::move(reg_ns), {}});
    g_report.add({"registry.teardown_by_label", params, "ns/metric", std::move(teardown_ns), {}});
    auto snap_params = params;
    snap_params["payload_bytes"] = std::to_string(payload);
    if (alloc_delta >= 0) {
        snap_params["retained_heap_bytes_mallinfo2"] = std::to_string(alloc_delta);
    }
    g_report.add({"registry.snapshot_all", snap_params, "us/call", std::move(snap_us), {}});
    g_report.add({"registry.snapshot_by_prefix", params, "us/call", std::move(prefix_us),
                  "prefix matches 1/" + std::to_string(k_metrics_per_component) + " of the population"});
    g_report.add({"registry.snapshot_by_label", params, "us/call", std::move(label_us),
                  "label matches one component (" + std::to_string(k_metrics_per_component) + " series)"});

    reg.clear();
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
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
        }
    }

    auto& reg = registry::instance();
    const auto saved_max = reg.max_metrics();
    reg.set_max_metrics(0); // the 10k case sits exactly at the default cap

    std::printf("composite metrics-registry profile  (reps=%d, %d metrics/component)\n", g_reps,
                k_metrics_per_component);
    std::printf("%-10s %12s %12s %14s %14s %12s %12s %12s\n", "metrics", "reg ns/m", "rm ns/m", "snap us",
                "prefix us", "label us", "bytes", "alloc B");
    for (const int count : {100, 1000, 10000}) {
        profile_count(count);
    }
    reg.set_max_metrics(saved_max);

    if (bench::g_failed) {
        std::fprintf(stderr, "\nFAILED: measurement invariants broke; no artifact written.\n");
        return 1;
    }
    if (!json_path.empty()) {
        auto meta = bench::capture_environment(std::move(framework_commit), std::move(harness_commit));
        meta["harness"] = "bench_registry";
        meta["repetitions"] = g_reps;
        meta["metrics_per_component"] = k_metrics_per_component;
        if (!g_report.write(json_path, meta)) {
            return 1;
        }
        std::printf("\nwrote %s\n", json_path.c_str());
    }
    std::printf("\ndone.\n");
    return 0;
}
