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

#include <cstdio>
#include <string>

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
    check(telemetry::manager::instance().initialize(cfg), "telemetry initialized");

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

    telemetry::manager::instance().shutdown();

    if (g_fails != 0) {
        std::fprintf(stderr, "%d telemetry grouping check(s) FAILED\n", g_fails);
        return 1;
    }
    std::puts("TELEMETRY GROUPING TESTS PASSED");
    return 0;
}
