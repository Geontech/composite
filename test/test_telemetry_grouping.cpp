/*
 * Copyright (C) 2024-2026 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

// OTLP export groups series by INSTRUMENT NAME.
//
// The OpenTelemetry SDK keys instruments by name. The exporter used to create one instrument per
// (name, label-set), so N components publishing the same metric name — the normal case, since
// identity is carried by the component_id LABEL, not the name — collapsed into a single series:
// whichever registered last. Every other component silently vanished from the collector while the
// exporter looked perfectly healthy. Histograms were worse: one "<name>_bucket" instrument per
// bucket, so all but one bucket were dropped.
//
// The contract this pins: one instrument per name, carrying one series per label set.
//
// Only built when COMPOSITE_USE_OPENTELEMETRY=ON. Needs no collector — the OTLP HTTP exporter is
// lazy, so an unreachable endpoint exercises registration without any network.
#include <composite/metrics/registry.hpp>
#include <composite/telemetry/config.hpp>
#include <composite/telemetry/manager.hpp>

// Private (src/, not installed) — see the header's comment on why this is testable at all.
#include "telemetry_boundary_format.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>

namespace {
int g_fails = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_fails;
    }
}
auto labels_for(const std::string& id) -> composite::metrics::labels_t {
    return {{"component_id", id}};
}
} // namespace

int main() {
    using namespace composite;

    telemetry::config cfg;
    cfg.enabled = true;
    cfg.service_name = "grouping_test";
    cfg.exporter.endpoint = "http://127.0.0.1:1"; // deliberately unreachable; exports just fail
    // Short enough that real COLLECTS happen while this test runs. At the 10s default nothing is
    // ever collected in a test that finishes in milliseconds, so none of the six observe_*
    // callbacks executed — and those callbacks are the entire static_cast<Native*>(series.metric)
    // surface. Collection happens before export, so the unreachable endpoint costs nothing here.
    cfg.export_interval = std::chrono::milliseconds{50};
    telemetry::manager::instance().initialize(cfg);
    // initialize() returns true for three different states (fresh success, already-initialized,
    // and disabled-by-config, which explicitly leaves it uninitialized). is_initialized() is the
    // predicate that actually distinguishes them.
    check(telemetry::manager::instance().is_initialized(), "telemetry initialized");

    auto& reg = metrics::registry::instance();
    auto& mgr = telemetry::manager::instance();

    // ---- (1) three components, ONE metric name, three label sets ----
    constexpr int k_components = 3;
    for (int i = 0; i < k_components; ++i) {
        reg.get_or_create_counter("test.packets", "packets", "1", labels_for("comp" + std::to_string(i)));
    }
    {
        const auto [instruments, series] = mgr.exported_series_counts();
        check(instruments == 1, "one INSTRUMENT for one metric name");
        check(series == k_components, "one SERIES per component — none collapsed away");
        std::printf("counter: instruments=%zu series=%zu (expected 1/%d)\n", instruments, series, k_components);
    }

    // ---- (2) a histogram exports count/sum/bucket, with every bucket present ----
    const std::vector<double> bounds{1.0, 5.0, 10.0};
    reg.get_or_create_histogram("test.latency", "latency", "ms", bounds, labels_for("histcomp"));
    {
        const auto [instruments, series] = mgr.exported_series_counts();
        // +3 instruments: _count, _sum, _bucket. +1 count, +1 sum, +(bounds+1) bucket series.
        check(instruments == 1 + 3, "histogram adds exactly three instruments (count/sum/bucket)");
        const std::size_t expected = k_components + 1 + 1 + (bounds.size() + 1);
        check(series == expected, "every histogram bucket is its own series, not just the last one");
        std::printf("with histogram: instruments=%zu series=%zu (expected 4/%zu)\n", instruments, series, expected);
    }

    // ---- (3) deregistering one component drops only ITS series ----
    reg.remove_by_label("component_id", "comp1");
    {
        const auto [instruments, series] = mgr.exported_series_counts();
        check(instruments == 1 + 3, "the shared instrument survives while other components still use it");
        const std::size_t expected = (k_components - 1) + 1 + 1 + (bounds.size() + 1);
        check(series == expected, "only the removed component's series went");
        std::printf("after removal: instruments=%zu series=%zu (expected 4/%zu)\n", instruments, series, expected);
    }

    // ---- (4) removing the last user of a name retires its instrument ----
    reg.remove_by_label("component_id", "comp0");
    reg.remove_by_label("component_id", "comp2");
    {
        const auto [instruments, series] = mgr.exported_series_counts();
        check(instruments == 3, "the counter instrument is retired once its last series goes");
        std::printf("counter retired: instruments=%zu series=%zu\n", instruments, series);
    }

    // ---- (5) a NAME COLLISION across metric types must be refused, not type-confused ----
    // The registry's duplicate check is per-collection, so the same name can exist as a counter
    // AND a gauge. Grouping on the exported name alone would append the gauge to the counter's
    // group, and the counter callback would static_cast a gauge<double>* to counter<uint64_t>* —
    // undefined behaviour on every export. The collision must be refused instead.
    {
        const auto [before_instruments, before_series] = mgr.exported_series_counts();
        reg.get_or_create_counter("collide.me", "counter", "1", labels_for("as_counter"));
        const auto [mid_instruments, mid_series] = mgr.exported_series_counts();
        check(mid_series == before_series + 1, "collision: the counter registered normally");

        // Same NAME, different TYPE. Must not join the counter's group.
        reg.get_or_create_gauge("collide.me", "gauge", "1", labels_for("as_gauge"));
        const auto [after_instruments, after_series] = mgr.exported_series_counts();
        check(after_series == mid_series, "collision: the gauge was REFUSED, not appended to the counter's instrument");
        check(after_instruments == mid_instruments, "collision: no extra instrument was created either");
        std::printf("collision: instruments %zu->%zu series %zu->%zu (gauge must add nothing)\n", mid_instruments,
                    after_instruments, mid_series, after_series);
    }

    // ---- (6) removing a REFUSED collision must not strip the accepted metric's series ----
    // Same name AND same labels this time. The gauge is refused by grouping, but its metadata is
    // indistinguishable from the counter's by (name, labels) — so a removal keyed on those alone
    // would drop the counter's series while the counter is still alive and exporting.
    {
        const auto shared = labels_for("same_identity");
        reg.get_or_create_counter("collide.identical", "counter", "1", shared);
        const auto [_, with_counter] = mgr.exported_series_counts();

        reg.get_or_create_gauge("collide.identical", "gauge", "1", shared); // refused by grouping
        reg.remove_gauge("collide.identical", shared);                      // must be a no-op for the counter

        const auto [__, after_removal] = mgr.exported_series_counts();
        check(after_removal == with_counter,
              "collision removal: removing the REFUSED gauge left the counter's series intact");
        std::printf("collision removal: series %zu -> %zu (must be unchanged)\n", with_counter, after_removal);

        // and the counter's own removal still works
        reg.remove_counter("collide.identical", shared);
        const auto [___, after_counter] = mgr.exported_series_counts();
        check(after_counter == with_counter - 1, "collision removal: the counter's own removal still drops its series");
    }

    // ---- (7) drive REAL collects through all six observe_* callbacks ----
    // Everything above asserts the manager's own bookkeeping. That catches the collapse and the
    // collision bugs, but not the other half of the fix: that each callback reads the right type
    // from the right pointer. Give the histogram real observations and let the reader collect a
    // few times so observe_counter/updown/gauge/hist_count/hist_sum/hist_bucket all execute
    // against live metrics. Under the sanitizer presets a type-confused cast or an out-of-range
    // bucket index is fatal here; without them this at least proves the callbacks run.
    {
        auto& hist = reg.get_or_create_histogram("collect.latency", "latency", "ms", bounds, labels_for("collect"));
        for (int i = 0; i < 200; ++i) {
            hist.record(static_cast<double>(i % 12));
        }
        auto& ctr = reg.get_or_create_counter("collect.packets", "packets", "1", labels_for("collect"));
        ctr.add(7);
        auto& gg = reg.get_or_create_gauge("collect.temp", "temp", "C", labels_for("collect"));
        gg.set(36.6);
        auto& ud = reg.get_or_create_updown_counter("collect.queue", "queue", "1", labels_for("collect"));
        ud.add(3);

        // Several export intervals' worth, so a collect is guaranteed to have run.
        std::this_thread::sleep_for(std::chrono::milliseconds{300});
        std::puts("collect: export intervals elapsed with all six observe_* callbacks live");
    }

    // ---- (8a) the boundary formatter itself: distinct doubles => distinct labels ----
    // Asserted directly, because a collision is invisible from the outside: both series register
    // successfully and one silently overwrites the other only when Observe() keys them by
    // attribute set. to_string() (fixed 6-decimal) fails the first pair; {:g} (6 significant
    // digits) fails the second.
    {
        const double one = 1.0;
        const double one_ulp = std::nextafter(one, 2.0);
        check(composite::telemetry::detail::format_boundary(1e-7) !=
                  composite::telemetry::detail::format_boundary(2e-7),
              "le labels: 1e-7 and 2e-7 render differently");
        check(composite::telemetry::detail::format_boundary(one) !=
                  composite::telemetry::detail::format_boundary(one_ulp),
              "le labels: 1.0 and the next representable double render differently");
        // ...while ordinary bounds stay readable rather than becoming 17-digit noise.
        check(composite::telemetry::detail::format_boundary(10.0) == "10" ||
                  composite::telemetry::detail::format_boundary(10.0) == "1E1",
              "le labels: an ordinary bound stays short");
        std::printf("le labels: 1e-7='%s' 2e-7='%s' 1.0='%s' 1.0+ulp='%s' 10='%s'\n",
                    composite::telemetry::detail::format_boundary(1e-7).c_str(),
                    composite::telemetry::detail::format_boundary(2e-7).c_str(),
                    composite::telemetry::detail::format_boundary(one).c_str(),
                    composite::telemetry::detail::format_boundary(one_ulp).c_str(),
                    composite::telemetry::detail::format_boundary(10.0).c_str());
    }

    // ---- (8) adjacent close boundaries must not collapse to one "le" label ----
    // The le attribute is formatted from the boundary. to_string() (fixed 6-decimal) and {:g}
    // (6 significant digits) both render distinct nearby doubles identically, and two series with
    // the same attribute set in one group silently overwrite each other — the exact collapse this
    // grouping work exists to prevent. Boundaries chosen to defeat both: 1e-7/2e-7 differ only
    // below to_string's resolution, and the 1.0 pair only in the 17th significant digit.
    {
        const double one = 1.0;
        const double one_ulp = std::nextafter(one, 2.0);
        const std::vector<double> tricky{1e-7, 2e-7, one, one_ulp};
        const auto [before_inst, before_series] = mgr.exported_series_counts();
        reg.get_or_create_histogram("test.tricky", "tricky bounds", "s", tricky, labels_for("tricky"));
        const auto [after_inst, after_series] = mgr.exported_series_counts();

        // +3 instruments (_count/_sum/_bucket); +1 +1 +(bounds+1) series. If two boundaries
        // rendered the same label the bucket series would still be REGISTERED here — the
        // collision only bites at Observe() time — so assert the labels directly too.
        check(after_inst == before_inst + 3, "close bounds: histogram still adds three instruments");
        check(after_series == before_series + 2 + (tricky.size() + 1), "close bounds: every bucket is its own series");
        std::printf("close bounds: series %zu -> %zu\n", before_series, after_series);
    }

    // ---- (8b) a histogram registers all three roles, or none ----
    // A histogram is exported as three instruments and group_for() refuses per role, so a
    // collision on ONE of them used to leave a malformed family (buckets and a sum with no count,
    // say) whose shape depended on registration order. No collector can reconstruct that.
    {
        // (i) an existing native counter occupies "hcol.a_count", so the histogram's _count role
        //     collides and the whole histogram must be refused.
        reg.get_or_create_counter("hcol.a_count", "squatter", "1", labels_for("squat"));
        const auto [inst_before, series_before] = mgr.exported_series_counts();
        reg.get_or_create_histogram("hcol.a", "blocked", "ms", bounds, labels_for("hist"));
        const auto [inst_after, series_after] = mgr.exported_series_counts();
        check(inst_after == inst_before && series_after == series_before,
              "histogram atomicity: a _count collision refuses the WHOLE histogram, leaving nothing behind");
        std::printf("histogram atomicity (count collision): instruments %zu->%zu series %zu->%zu\n", inst_before,
                    inst_after, series_before, series_after);
    }
    {
        // (ii) a second histogram of the same name with a DIFFERENT unit. _count and _bucket carry
        //      unit "1" and would match, while _sum carries the metric's unit and collides — the
        //      partial-family case exactly. All three must be refused together.
        reg.get_or_create_histogram("hcol.b", "first", "ms", bounds, labels_for("first"));
        const auto [inst_before, series_before] = mgr.exported_series_counts();
        reg.get_or_create_histogram("hcol.b", "second", "s", bounds, labels_for("second"));
        const auto [inst_after, series_after] = mgr.exported_series_counts();
        check(inst_after == inst_before && series_after == series_before,
              "histogram atomicity: a unit collision on _sum refuses _count and _bucket too");
        std::printf("histogram atomicity (unit collision): instruments %zu->%zu series %zu->%zu\n", inst_before,
                    inst_after, series_before, series_after);
    }

    // ---- (9) shutdown() concurrent with metric removal ----
    // The deregistration observer is what stops a series outliving its native metric, so it has to
    // be the LAST thing shutdown() removes. Removing it first left a window where a concurrent
    // remove_counter() destroyed its metric with nobody listening while an export callback still
    // held that raw pointer — including across the final ForceFlush(). Sanitizer test: a
    // regression is a heap-use-after-free inside an observe_* callback, not a wrong count.
    {
        constexpr int k_churn = 200;
        for (int i = 0; i < k_churn; ++i) {
            reg.get_or_create_counter("shutdown.churn", "churn", "1", labels_for("c" + std::to_string(i)));
        }
        std::atomic<bool> go{false};
        std::thread remover{[&] {
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < k_churn; ++i) {
                reg.remove_counter("shutdown.churn", {{"component_id", "c" + std::to_string(i)}});
            }
        }};
        go.store(true, std::memory_order_release);
        telemetry::manager::instance().shutdown(); // races the removals above
        remover.join();
        std::puts("shutdown race: shutdown() completed against concurrent metric removal");
    }

    telemetry::manager::instance().shutdown();

    if (g_fails != 0) {
        std::fprintf(stderr, "%d telemetry grouping check(s) FAILED\n", g_fails);
        return 1;
    }
    std::puts("TELEMETRY GROUPING TESTS PASSED");
    return 0;
}
