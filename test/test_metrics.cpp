/*
 * Copyright (C) 2025 Geon Technologies, LLC
 *
 * This file is part of composite.
 *
 * composite is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * composite is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see http://www.gnu.org/licenses/.
 */

#define CATCH_CONFIG_MAIN
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

#include "composite/metrics/metrics.hpp"

#include <thread>
#include <vector>

using namespace composite::metrics;
using Catch::Matchers::WithinAbs;

// ============================================================================
// Counter Tests
// ============================================================================

TEST_CASE("Counter basic operations", "[metrics][counter]") {
    counter<uint64_t> c;

    SECTION("Initial value is zero") {
        REQUIRE(c.value() == 0);
    }

    SECTION("inc() increments by 1") {
        c.inc();
        REQUIRE(c.value() == 1);
        c.inc();
        REQUIRE(c.value() == 2);
    }

    SECTION("add() increments by delta") {
        c.add(10);
        REQUIRE(c.value() == 10);
        c.add(5);
        REQUIRE(c.value() == 15);
    }

    SECTION("reset() sets value to zero (deprecated — removed in 0.6)") {
        // counter::reset() is deprecated: counters export as monotonic sums, and a reset reads
        // as a counter-reset to OTLP collectors (fabricated rates). The behavior is still
        // verified until the 0.6 removal; suppress only the deprecation diagnostic here.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
        c.add(100);
        c.reset();
        REQUIRE(c.value() == 0);
#pragma GCC diagnostic pop
    }
}

TEST_CASE("Counter arithmetic operators", "[metrics][counter]") {
    counter<uint64_t> c;

    SECTION("Prefix increment (++c)") {
        ++c;
        REQUIRE(c.value() == 1);
        auto& ref = ++c;
        REQUIRE(c.value() == 2);
        REQUIRE(&ref == &c);
    }

    SECTION("Postfix increment (c++)") {
        auto prev = c++;
        REQUIRE(prev == 0);
        REQUIRE(c.value() == 1);
    }

    SECTION("Add-assign (c += delta)") {
        c += 10;
        REQUIRE(c.value() == 10);
        auto& ref = (c += 5);
        REQUIRE(c.value() == 15);
        REQUIRE(&ref == &c);
    }
}

// ============================================================================
// UpDownCounter Tests
// ============================================================================

TEST_CASE("UpDownCounter basic operations", "[metrics][updown_counter]") {
    updown_counter<int64_t> c;

    SECTION("Initial value is zero") {
        REQUIRE(c.value() == 0);
    }

    SECTION("inc() increments by 1") {
        c.inc();
        REQUIRE(c.value() == 1);
    }

    SECTION("dec() decrements by 1") {
        c.inc();
        c.inc();
        c.dec();
        REQUIRE(c.value() == 1);
    }

    SECTION("add() with positive and negative values") {
        c.add(10);
        REQUIRE(c.value() == 10);
        c.add(-3);
        REQUIRE(c.value() == 7);
    }

    SECTION("Value can go negative") {
        c.add(-5);
        REQUIRE(c.value() == -5);
    }

    SECTION("reset() sets value to zero") {
        c.add(100);
        c.reset();
        REQUIRE(c.value() == 0);
    }
}

TEST_CASE("UpDownCounter arithmetic operators", "[metrics][updown_counter]") {
    updown_counter<int64_t> c;

    SECTION("Prefix increment (++c)") {
        ++c;
        REQUIRE(c.value() == 1);
    }

    SECTION("Postfix increment (c++)") {
        auto prev = c++;
        REQUIRE(prev == 0);
        REQUIRE(c.value() == 1);
    }

    SECTION("Prefix decrement (--c)") {
        --c;
        REQUIRE(c.value() == -1);
    }

    SECTION("Postfix decrement (c--)") {
        c.inc();
        c.inc();
        auto prev = c--;
        REQUIRE(prev == 2);
        REQUIRE(c.value() == 1);
    }

    SECTION("Add-assign (c += delta)") {
        c += 10;
        REQUIRE(c.value() == 10);
    }

    SECTION("Subtract-assign (c -= delta)") {
        c += 10;
        c -= 3;
        REQUIRE(c.value() == 7);
    }
}

// ============================================================================
// Gauge Tests
// ============================================================================

TEST_CASE("Gauge basic operations", "[metrics][gauge]") {
    gauge<double> g;

    SECTION("Initial value is zero") {
        REQUIRE_THAT(g.value(), WithinAbs(0.0, 0.001));
    }

    SECTION("set() changes value") {
        g.set(42.5);
        REQUIRE_THAT(g.value(), WithinAbs(42.5, 0.001));
    }

    SECTION("set() overwrites previous value") {
        g.set(10.0);
        g.set(20.0);
        REQUIRE_THAT(g.value(), WithinAbs(20.0, 0.001));
    }
}

TEST_CASE("Gauge assignment operator", "[metrics][gauge]") {
    gauge<double> g;

    SECTION("Assignment operator (g = value)") {
        g = 3.14159;
        REQUIRE_THAT(g.value(), WithinAbs(3.14159, 0.00001));
    }
}

// ============================================================================
// Histogram Tests
// ============================================================================

TEST_CASE("Histogram basic operations", "[metrics][histogram]") {
    histogram h({10.0, 50.0, 100.0, 500.0, 1000.0});

    SECTION("Initial state") {
        REQUIRE(h.count() == 0);
        REQUIRE_THAT(h.sum(), WithinAbs(0.0, 0.001));
        auto counts = h.bucket_counts();
        REQUIRE(counts.size() == 6); // 5 boundaries + 1 overflow
        for (auto count : counts) {
            REQUIRE(count == 0);
        }
    }

    SECTION("Recording values") {
        h.record(5.0);    // Bucket 0: [0, 10)
        h.record(25.0);   // Bucket 1: [10, 50)
        h.record(75.0);   // Bucket 2: [50, 100)
        h.record(200.0);  // Bucket 3: [100, 500)
        h.record(750.0);  // Bucket 4: [500, 1000)
        h.record(2000.0); // Bucket 5: [1000, +inf)

        REQUIRE(h.count() == 6);
        REQUIRE_THAT(h.sum(), WithinAbs(3055.0, 0.001));

        auto counts = h.bucket_counts();
        REQUIRE(counts[0] == 1);
        REQUIRE(counts[1] == 1);
        REQUIRE(counts[2] == 1);
        REQUIRE(counts[3] == 1);
        REQUIRE(counts[4] == 1);
        REQUIRE(counts[5] == 1);
    }

    SECTION("reset() clears histogram") {
        h.record(50.0);
        h.record(100.0);
        h.reset();

        REQUIRE(h.count() == 0);
        REQUIRE_THAT(h.sum(), WithinAbs(0.0, 0.001));
    }
}

TEST_CASE("Histogram power-of-2 boundaries", "[metrics][histogram]") {
    auto bounds = histogram::power_of_2_boundaries(10);

    SECTION("Generates correct boundaries") {
        REQUIRE(bounds.size() == 9); // num_buckets - 1
        REQUIRE_THAT(bounds[0], WithinAbs(1.0, 0.001));
        REQUIRE_THAT(bounds[1], WithinAbs(2.0, 0.001));
        REQUIRE_THAT(bounds[2], WithinAbs(4.0, 0.001));
        REQUIRE_THAT(bounds[3], WithinAbs(8.0, 0.001));
        REQUIRE_THAT(bounds[4], WithinAbs(16.0, 0.001));
    }
}

// v0.5 API guarantee: observations must be finite and non-negative. This is what makes sum()
// monotonic, which is what lets the OTLP bridge export `_sum` as a Counter rather than an
// UpDownCounter — a decreasing counter is read downstream as a reset and fabricates a huge rate.
// A single NaN is worse than wrong: NaN + x is NaN forever, so it would poison the sum permanently.
TEST_CASE("Histogram rejects negative and non-finite observations", "[metrics][histogram]") {
    const std::vector<double> bounds{1.0, 10.0, 100.0};
    histogram h{bounds};

    h.record(5.0);
    const auto baseline = h.snapshot();
    REQUIRE(baseline.count == 1);
    REQUIRE(baseline.sum == 5.0);
    REQUIRE(h.rejected_observations() == 0);

    SECTION("zero is VALID and is recorded") {
        h.record(0.0);
        const auto snap = h.snapshot();
        REQUIRE(snap.count == 2);
        REQUIRE(snap.sum == 5.0);
        REQUIRE(h.rejected_observations() == 0);
        REQUIRE(snap.bucket_counts[0] == 1); // 0.0 lands in the first bucket (<= 1.0)
    }

    SECTION("negative is rejected, touching no field") {
        h.record(-1.0);
        const auto snap = h.snapshot();
        REQUIRE(snap.count == baseline.count); // not counted
        REQUIRE(snap.sum == baseline.sum);     // not summed
        REQUIRE(snap.bucket_counts == baseline.bucket_counts);
        REQUIRE(h.rejected_observations() == 1);
    }

    SECTION("NaN is rejected and does not poison the sum") {
        h.record(std::numeric_limits<double>::quiet_NaN());
        const auto snap = h.snapshot();
        REQUIRE(snap.count == baseline.count);
        REQUIRE(snap.sum == 5.0); // the decisive one: a NaN here would make this NaN forever
        REQUIRE(!std::isnan(snap.sum));
        REQUIRE(h.rejected_observations() == 1);
        h.record(3.0); // and the histogram still works afterwards
        REQUIRE(h.snapshot().sum == 8.0);
    }

    SECTION("+infinity is rejected") {
        h.record(std::numeric_limits<double>::infinity());
        const auto snap = h.snapshot();
        REQUIRE(snap.count == baseline.count);
        REQUIRE(snap.sum == 5.0);
        REQUIRE(std::isfinite(snap.sum));
        REQUIRE(h.rejected_observations() == 1);
    }

    SECTION("-infinity is rejected") {
        h.record(-std::numeric_limits<double>::infinity());
        const auto snap = h.snapshot();
        REQUIRE(snap.count == baseline.count);
        REQUIRE(snap.sum == 5.0);
        REQUIRE(h.rejected_observations() == 1);
    }

    SECTION("rejections are visible in the snapshot and cleared by reset()") {
        h.record(-1.0);
        h.record(std::numeric_limits<double>::quiet_NaN());
        REQUIRE(h.snapshot().rejected == 2); // carried in the snapshot, not just the accessor
        REQUIRE(h.rejected_observations() == 2);
        h.reset();
        REQUIRE(h.rejected_observations() == 0); // "initial state" means ALL of it
        REQUIRE(h.snapshot().rejected == 0);
        REQUIRE(h.snapshot().count == 0);
        REQUIRE(h.snapshot().sum == 0.0);
    }

    SECTION("sum is monotonic across a mixed stream") {
        double previous = h.snapshot().sum;
        const std::vector<double> stream{
            2.0, -5.0, 0.0, std::numeric_limits<double>::quiet_NaN(), 50.0, -std::numeric_limits<double>::infinity(),
            1.0};
        for (const auto v : stream) {
            h.record(v);
            const auto now = h.snapshot().sum;
            REQUIRE(now >= previous); // never decreases — the property `_sum`-as-Counter rests on
            previous = now;
        }
        REQUIRE(h.rejected_observations() == 3);
        REQUIRE(h.snapshot().count == 5); // 5.0 + the four valid stream values
    }
}

// v0.5 freeze: boundaries must be finite, non-negative and strictly increasing. A NaN boundary is
// the sharp case — std::lower_bound requires a partitioned range, so bucketing becomes undefined
// rather than merely odd — while duplicates and out-of-order bounds produce exported `le` labels
// implying an ordering the bucket counts do not honour.
TEST_CASE("Histogram validates bucket boundaries", "[metrics][histogram]") {
    // Boundaries passed as named vectors: a braced list inside REQUIRE_* splits on its commas and
    // the macro sees three arguments.
    const std::vector<double> ok{1.0, 5.0, 10.0};
    const std::vector<double> zero_ok{0.0, 1.0};
    const std::vector<double> empty_ok{};
    REQUIRE_NOTHROW(histogram{ok});
    REQUIRE_NOTHROW(histogram{zero_ok});  // zero is a legitimate boundary
    REQUIRE_NOTHROW(histogram{empty_ok}); // no boundaries: one overflow bucket

    const std::vector<double> negative{-1.0, 1.0};
    const std::vector<double> nan_bound{1.0, std::numeric_limits<double>::quiet_NaN()};
    const std::vector<double> inf_bound{1.0, std::numeric_limits<double>::infinity()};
    const std::vector<double> duplicate{5.0, 5.0};
    const std::vector<double> unsorted{10.0, 5.0};
    REQUIRE_THROWS_AS(histogram{negative}, std::invalid_argument);
    REQUIRE_THROWS_AS(histogram{nan_bound}, std::invalid_argument);
    REQUIRE_THROWS_AS(histogram{inf_bound}, std::invalid_argument);
    REQUIRE_THROWS_AS(histogram{duplicate}, std::invalid_argument);
    REQUIRE_THROWS_AS(histogram{unsorted}, std::invalid_argument);
}

// The bounds are INCLUSIVE (`le`), matching Prometheus and the OTLP bridge. The constructor comment
// used to claim exclusive bounds ([0,10)), which contradicted the lower_bound implementation; this
// pins the actual, intended behaviour so the two cannot drift apart again.
TEST_CASE("Histogram bucket bounds are inclusive", "[metrics][histogram]") {
    const std::vector<double> bounds{10.0, 20.0};
    histogram h{bounds};
    h.record(10.0); // exactly ON the first boundary -> first bucket, not the second
    h.record(20.0); // exactly ON the second boundary -> second bucket
    h.record(20.5); // past the last boundary -> overflow
    const auto snap = h.snapshot();
    REQUIRE(snap.bucket_counts[0] == 1);
    REQUIRE(snap.bucket_counts[1] == 1);
    REQUIRE(snap.bucket_counts[2] == 1);
    REQUIRE(snap.count == 3);
}

TEST_CASE("Histogram snapshot provides consistent data", "[metrics][histogram]") {
    histogram h({10.0, 50.0, 100.0});

    SECTION("Snapshot matches individual accessors when no concurrent writes") {
        h.record(5.0);
        h.record(25.0);
        h.record(75.0);

        auto snap = h.snapshot();

        REQUIRE(snap.count == h.count());
        REQUIRE_THAT(snap.sum, WithinAbs(h.sum(), 0.001));
        REQUIRE(snap.bucket_counts == h.bucket_counts());
    }

    SECTION("Snapshot bucket counts sum equals count") {
        h.record(5.0);
        h.record(15.0);
        h.record(55.0);
        h.record(200.0);

        auto snap = h.snapshot();

        uint64_t bucket_sum = 0;
        for (auto c : snap.bucket_counts) {
            bucket_sum += c;
        }
        REQUIRE(bucket_sum == snap.count);
    }
}

// ============================================================================
// Registry Tests
// ============================================================================

TEST_CASE("Registry metric creation", "[metrics][registry]") {
    // Note: Registry is a singleton, so we need to clear it first
    registry::instance().clear();

    SECTION("Create counter") {
        auto& c = registry::instance().create_counter("test.counter", "A test counter", "1", {{"component", "test"}});
        c.inc();
        REQUIRE(c.value() == 1);
        REQUIRE(registry::instance().metric_count() == 1);
    }

    SECTION("Create updown_counter") {
        auto& c = registry::instance().create_updown_counter("test.updown", "A test updown counter");
        c.add(5);
        c.dec();
        REQUIRE(c.value() == 4);
    }

    SECTION("Create gauge") {
        auto& g = registry::instance().create_gauge("test.gauge", "A test gauge");
        g.set(3.14);
        REQUIRE_THAT(g.value(), WithinAbs(3.14, 0.001));
    }

    SECTION("Create histogram") {
        auto& h = registry::instance().create_histogram("test.histogram", "A test histogram", "ms",
                                                        {1.0, 5.0, 10.0, 50.0, 100.0});
        h.record(7.5);
        REQUIRE(h.count() == 1);
    }

    SECTION("Create histogram with power-of-2 boundaries") {
        auto& h =
            registry::instance().create_histogram_pow2("test.histogram_pow2", "A test power-of-2 histogram", "ns", 16);
        h.record(100);
        REQUIRE(h.count() == 1);
    }
}

TEST_CASE("Registry snapshot operations", "[metrics][registry]") {
    registry::instance().clear();

    // Create some metrics
    auto& counter1 = registry::instance().create_counter("app.packets.sent", "Packets sent", "1", {{"port", "eth0"}});
    auto& counter2 =
        registry::instance().create_counter("app.packets.received", "Packets received", "1", {{"port", "eth0"}});
    auto& gauge1 = registry::instance().create_gauge("system.cpu.usage", "CPU usage percentage", "%");

    counter1.add(100);
    counter2.add(200);
    gauge1.set(45.5);

    SECTION("snapshot_all() returns all metrics") {
        auto snapshots = registry::instance().snapshot_all();
        REQUIRE(snapshots.size() == 3);
    }

    SECTION("snapshot_by_prefix() filters by name prefix") {
        auto snapshots = registry::instance().snapshot_by_prefix("app.packets");
        REQUIRE(snapshots.size() == 2);

        snapshots = registry::instance().snapshot_by_prefix("system");
        REQUIRE(snapshots.size() == 1);
    }

    SECTION("snapshot_by_label() filters by label") {
        auto snapshots = registry::instance().snapshot_by_label("port", "eth0");
        REQUIRE(snapshots.size() == 2);
    }
}

TEST_CASE("Registry observer registration", "[metrics][registry]") {
    registry::instance().clear();

    std::vector<std::string> registered_names;

    SECTION("Observer is notified on metric creation") {
        auto observer_id = registry::instance().add_observer(
            [&registered_names](const metric_metadata& meta, void*) { registered_names.push_back(meta.name); },
            false // Don't notify for existing metrics
        );

        registry::instance().create_counter("observed.counter1");
        registry::instance().create_gauge("observed.gauge1");

        REQUIRE(registered_names.size() == 2);
        REQUIRE(registered_names[0] == "observed.counter1");
        REQUIRE(registered_names[1] == "observed.gauge1");

        registry::instance().remove_observer(observer_id);
    }

    SECTION("Observer is notified for existing metrics") {
        registry::instance().create_counter("existing.counter");
        registry::instance().create_gauge("existing.gauge");

        auto observer_id = registry::instance().add_observer(
            [&registered_names](const metric_metadata& meta, void*) { registered_names.push_back(meta.name); },
            true // Notify for existing metrics
        );

        REQUIRE(registered_names.size() == 2);

        registry::instance().remove_observer(observer_id);
    }
}

// ============================================================================
// Thread Safety Tests
// ============================================================================

TEST_CASE("Counter thread safety", "[metrics][threading]") {
    counter<uint64_t> c;
    const int num_threads = 4;
    const int increments_per_thread = 10000;

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&c, increments_per_thread]() {
            for (int j = 0; j < increments_per_thread; ++j) {
                c.inc();
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    REQUIRE(c.value() == num_threads * increments_per_thread);
}

// 0.5.1 regression: max_metrics() used to read a plain member the (locked) setter wrote — a
// data race on the public API. The pair is atomic now; this pins it for the TSan preset, where
// the pre-fix code reports a race (the assertion itself is trivially true in any build mode).
TEST_CASE("Registry max_metrics set/get is race-free", "[metrics][registry][threading]") {
    auto& reg = registry::instance();
    const auto saved = reg.max_metrics();
    constexpr int iterations = 50000;

    std::size_t observed_floor = std::numeric_limits<std::size_t>::max();
    std::thread writer([&reg]() {
        for (int i = 0; i < iterations; ++i) {
            reg.set_max_metrics(10000 + static_cast<std::size_t>(i & 1));
        }
    });
    std::thread reader([&reg, &observed_floor]() {
        for (int i = 0; i < iterations; ++i) {
            observed_floor = std::min(observed_floor, reg.max_metrics());
        }
    });
    writer.join();
    reader.join();
    reg.set_max_metrics(saved);

    // Every observed value is one the writer actually stored (or the pre-test value).
    REQUIRE(observed_floor >= 10000);
}

TEST_CASE("UpDownCounter thread safety", "[metrics][threading]") {
    updown_counter<int64_t> c;
    const int num_threads = 4;
    const int ops_per_thread = 10000;

    std::vector<std::thread> threads;
    // Half the threads increment, half decrement
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&c, ops_per_thread, i]() {
            for (int j = 0; j < ops_per_thread; ++j) {
                if (i % 2 == 0) {
                    c.inc();
                } else {
                    c.dec();
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // With equal inc/dec, should be 0
    REQUIRE(c.value() == 0);
}

TEST_CASE("Gauge thread safety", "[metrics][threading]") {
    gauge<double> g;
    const int num_threads = 4;
    const int sets_per_thread = 10000;

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&g, sets_per_thread, i]() {
            for (int j = 0; j < sets_per_thread; ++j) {
                g.set(static_cast<double>(i * 1000 + j));
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // Value should be one of the set values (not corrupted)
    // Max value is (num_threads - 1) * 1000 + (sets_per_thread - 1) = 12999
    auto val = g.value();
    REQUIRE(val >= 0.0);
    REQUIRE(val < static_cast<double>(num_threads * 1000 + sets_per_thread));
}

// ============================================================================
// Metric Name Validation Tests
// ============================================================================

TEST_CASE("Metric name validation", "[metrics][validation]") {
    SECTION("Valid metric names") {
        // Should not throw
        REQUIRE_NOTHROW(validate_metric_name("counter"));
        REQUIRE_NOTHROW(validate_metric_name("my_counter"));
        REQUIRE_NOTHROW(validate_metric_name("app.packets.sent"));
        REQUIRE_NOTHROW(validate_metric_name("Component1.metric"));
        REQUIRE_NOTHROW(validate_metric_name("a"));
        REQUIRE_NOTHROW(validate_metric_name("A1_B2.C3"));
    }

    SECTION("Empty name throws") {
        REQUIRE_THROWS_AS(validate_metric_name(""), invalid_metric_name_error);
    }

    SECTION("Name starting with number throws") {
        REQUIRE_THROWS_AS(validate_metric_name("1counter"), invalid_metric_name_error);
        REQUIRE_THROWS_AS(validate_metric_name("123"), invalid_metric_name_error);
    }

    SECTION("Name starting with underscore throws") {
        REQUIRE_THROWS_AS(validate_metric_name("_counter"), invalid_metric_name_error);
    }

    SECTION("Name starting with dot throws") {
        REQUIRE_THROWS_AS(validate_metric_name(".counter"), invalid_metric_name_error);
    }

    SECTION("Name ending with dot throws") {
        REQUIRE_THROWS_AS(validate_metric_name("counter."), invalid_metric_name_error);
    }

    SECTION("Consecutive dots throw") {
        REQUIRE_THROWS_AS(validate_metric_name("app..counter"), invalid_metric_name_error);
        REQUIRE_THROWS_AS(validate_metric_name("a...b"), invalid_metric_name_error);
    }

    SECTION("Invalid characters throw") {
        REQUIRE_THROWS_AS(validate_metric_name("counter-name"), invalid_metric_name_error);
        REQUIRE_THROWS_AS(validate_metric_name("counter name"), invalid_metric_name_error);
        REQUIRE_THROWS_AS(validate_metric_name("counter@name"), invalid_metric_name_error);
        REQUIRE_THROWS_AS(validate_metric_name("counter#name"), invalid_metric_name_error);
    }
}

TEST_CASE("Metric name sanitization", "[metrics][validation]") {
    SECTION("Valid names pass through unchanged (no hash suffix)") {
        REQUIRE(sanitize_for_metric_name("counter") == "counter");
        REQUIRE(sanitize_for_metric_name("my_counter") == "my_counter");
        REQUIRE(sanitize_for_metric_name("app.metrics") == "app.metrics");
    }

    SECTION("Modified names get hash suffix for collision prevention") {
        // When sanitization changes the name, a hash suffix is appended
        auto result = sanitize_for_metric_name("my-component");
        REQUIRE(result.starts_with("my_component_"));
        REQUIRE(result.size() == std::string("my_component_").size() + 16); // 16 hex chars
    }

    SECTION("Leading digits get 'c' prefix plus hash") {
        auto result = sanitize_for_metric_name("123");
        REQUIRE(result.starts_with("c123_"));
        REQUIRE(result.size() == std::string("c123_").size() + 16);
    }

    SECTION("Different inputs produce different hashes") {
        // These would collide without hash suffix
        auto result1 = sanitize_for_metric_name("my-component");
        auto result2 = sanitize_for_metric_name("my_component");
        REQUIRE(result1 != result2);
        REQUIRE(result1.starts_with("my_component_"));
        REQUIRE(result2 == "my_component"); // unchanged, no hash
    }

    SECTION("Empty string returns 'unnamed'") {
        REQUIRE(sanitize_for_metric_name("") == "unnamed");
    }

    SECTION("Deterministic output for same input") {
        auto result1 = sanitize_for_metric_name("test-id");
        auto result2 = sanitize_for_metric_name("test-id");
        REQUIRE(result1 == result2);
    }
}

TEST_CASE("Histogram power-of-2 boundary validation", "[metrics][histogram]") {
    SECTION("Valid bucket counts work") {
        REQUIRE_NOTHROW(histogram::power_of_2_boundaries(2));
        REQUIRE_NOTHROW(histogram::power_of_2_boundaries(20));
        REQUIRE_NOTHROW(histogram::power_of_2_boundaries(64));
    }

    SECTION("Zero buckets throws") {
        REQUIRE_THROWS_AS(histogram::power_of_2_boundaries(0), std::invalid_argument);
    }

    SECTION("One bucket throws") {
        REQUIRE_THROWS_AS(histogram::power_of_2_boundaries(1), std::invalid_argument);
    }

    SECTION("More than 64 buckets throws") {
        REQUIRE_THROWS_AS(histogram::power_of_2_boundaries(65), std::invalid_argument);
        REQUIRE_THROWS_AS(histogram::power_of_2_boundaries(100), std::invalid_argument);
    }
}

// ============================================================================
// Duplicate Metric Detection Tests
// ============================================================================

TEST_CASE("Duplicate metric detection", "[metrics][registry][validation]") {
    registry::instance().clear();

    SECTION("Duplicate counter with same name and labels throws") {
        registry::instance().create_counter("duplicate.test.counter", "First counter", "1", {{"component", "test"}});

        REQUIRE_THROWS_AS(registry::instance().create_counter("duplicate.test.counter", "Second counter", "1",
                                                              {{"component", "test"}}),
                          duplicate_metric_error);
    }

    SECTION("Counter with same name but different labels is allowed") {
        registry::instance().create_counter("duplicate.label.counter", "Counter for eth0", "1", {{"port", "eth0"}});

        REQUIRE_NOTHROW(registry::instance().create_counter("duplicate.label.counter", "Counter for eth1", "1",
                                                            {{"port", "eth1"}}));

        REQUIRE(registry::instance().metric_count() == 2);
    }

    SECTION("Duplicate updown_counter throws") {
        registry::instance().create_updown_counter("duplicate.updown");

        REQUIRE_THROWS_AS(registry::instance().create_updown_counter("duplicate.updown"), duplicate_metric_error);
    }

    SECTION("Duplicate gauge throws") {
        registry::instance().create_gauge("duplicate.gauge");

        REQUIRE_THROWS_AS(registry::instance().create_gauge("duplicate.gauge"), duplicate_metric_error);
    }

    SECTION("Duplicate histogram throws") {
        registry::instance().create_histogram("duplicate.histogram", "First histogram", "ms", {1.0, 10.0, 100.0});

        REQUIRE_THROWS_AS(
            registry::instance().create_histogram("duplicate.histogram", "Second histogram", "ms", {1.0, 10.0, 100.0}),
            duplicate_metric_error);
    }

    SECTION("Duplicate histogram_pow2 throws") {
        registry::instance().create_histogram_pow2("duplicate.histogram_pow2");

        REQUIRE_THROWS_AS(registry::instance().create_histogram_pow2("duplicate.histogram_pow2"),
                          duplicate_metric_error);
    }

    SECTION("Different metric types with same name are allowed") {
        // This is technically allowed - OTel spec says different instrument types
        // can share names but it's discouraged
        registry::instance().create_counter("same_name_different_type");
        registry::instance().create_gauge("same_name_different_type");

        REQUIRE(registry::instance().metric_count() == 2);
    }

    SECTION("Labels in different order are treated as same metric") {
        // First creation with labels in one order
        auto& c1 =
            registry::instance().create_counter("label_order.test", "Test counter", "1", {{"b", "2"}, {"a", "1"}});
        c1.add(10);

        // Attempt to create with same labels in different order should throw
        REQUIRE_THROWS_AS(registry::instance().create_counter("label_order.test", "Same counter different label order",
                                                              "1", {{"a", "1"}, {"b", "2"}}),
                          duplicate_metric_error);

        // get_or_create with different order should return the same counter
        auto& c2 = registry::instance().get_or_create_counter("label_order.test", "ignored", "ignored",
                                                              {{"a", "1"}, {"b", "2"}});
        REQUIRE(c2.value() == 10); // Same counter, should have our added value
        REQUIRE(&c1 == &c2);       // Same object
    }
}

TEST_CASE("Invalid metric name is caught by registry", "[metrics][registry][validation]") {
    registry::instance().clear();

    SECTION("Invalid counter name throws") {
        REQUIRE_THROWS_AS(registry::instance().create_counter("1invalid"), invalid_metric_name_error);
    }

    SECTION("Invalid gauge name throws") {
        REQUIRE_THROWS_AS(registry::instance().create_gauge("invalid..name"), invalid_metric_name_error);
    }

    SECTION("Invalid histogram name throws") {
        REQUIRE_THROWS_AS(registry::instance().create_histogram("invalid-name", "Description", "ms", {1.0, 10.0}),
                          invalid_metric_name_error);
    }
}

// ============================================================================
// Get-or-Create Tests
// ============================================================================

TEST_CASE("Get-or-create returns existing metric", "[metrics][registry][get_or_create]") {
    registry::instance().clear();

    SECTION("get_or_create_counter returns existing counter") {
        auto& c1 = registry::instance().get_or_create_counter("goc.counter", "desc", "1");
        c1.add(42);

        auto& c2 = registry::instance().get_or_create_counter("goc.counter", "ignored", "ignored");

        // Should be the same object
        REQUIRE(&c1 == &c2);
        REQUIRE(c2.value() == 42);
        REQUIRE(registry::instance().metric_count() == 1);
    }

    SECTION("get_or_create_updown_counter returns existing") {
        auto& c1 = registry::instance().get_or_create_updown_counter("goc.updown");
        c1.add(10);

        auto& c2 = registry::instance().get_or_create_updown_counter("goc.updown");

        REQUIRE(&c1 == &c2);
        REQUIRE(c2.value() == 10);
    }

    SECTION("get_or_create_gauge returns existing") {
        auto& g1 = registry::instance().get_or_create_gauge("goc.gauge");
        g1.set(3.14);

        auto& g2 = registry::instance().get_or_create_gauge("goc.gauge");

        REQUIRE(&g1 == &g2);
        REQUIRE_THAT(g2.value(), WithinAbs(3.14, 0.001));
    }

    SECTION("get_or_create_histogram returns existing") {
        auto& h1 = registry::instance().get_or_create_histogram("goc.histogram", "desc", "ms", {1.0, 10.0, 100.0});
        h1.record(50.0);

        auto& h2 = registry::instance().get_or_create_histogram("goc.histogram", "ignored", "ignored", {999.0});

        REQUIRE(&h1 == &h2);
        REQUIRE(h2.count() == 1);
    }

    SECTION("get_or_create_histogram_pow2 returns existing") {
        auto& h1 = registry::instance().get_or_create_histogram_pow2("goc.histogram_pow2");
        h1.record(100.0);

        auto& h2 = registry::instance().get_or_create_histogram_pow2("goc.histogram_pow2");

        REQUIRE(&h1 == &h2);
        REQUIRE(h2.count() == 1);
    }
}

TEST_CASE("Get-or-create creates new metric when not exists", "[metrics][registry][get_or_create]") {
    registry::instance().clear();

    SECTION("get_or_create_counter creates new") {
        REQUIRE(registry::instance().metric_count() == 0);

        auto& c = registry::instance().get_or_create_counter("new.counter");
        c.inc();

        REQUIRE(registry::instance().metric_count() == 1);
        REQUIRE(c.value() == 1);
    }

    SECTION("Different labels create different metrics") {
        auto& c1 = registry::instance().get_or_create_counter("labeled.counter", "desc", "1", {{"env", "prod"}});
        auto& c2 = registry::instance().get_or_create_counter("labeled.counter", "desc", "1", {{"env", "dev"}});

        REQUIRE(&c1 != &c2);
        REQUIRE(registry::instance().metric_count() == 2);
    }
}

TEST_CASE("Get-or-create validates names", "[metrics][registry][get_or_create][validation]") {
    registry::instance().clear();

    SECTION("Invalid name throws even for get_or_create") {
        REQUIRE_THROWS_AS(registry::instance().get_or_create_counter("1invalid"), invalid_metric_name_error);
    }
}

// ============================================================================
// Label Validation Tests
// ============================================================================

TEST_CASE("Label key validation", "[metrics][validation][labels]") {
    SECTION("Valid label keys") {
        REQUIRE_NOTHROW(validate_labels({{"component", "test"}}));
        REQUIRE_NOTHROW(validate_labels({{"Port1", "eth0"}}));
        REQUIRE_NOTHROW(validate_labels({{"my_label", "value"}}));
        REQUIRE_NOTHROW(validate_labels({{"a", "b"}}));
        REQUIRE_NOTHROW(validate_labels({{"ABC123_def", "value"}}));
    }

    SECTION("Empty labels are valid") {
        REQUIRE_NOTHROW(validate_labels({}));
    }

    SECTION("Empty label key throws") {
        REQUIRE_THROWS_AS(validate_labels({{"", "value"}}), invalid_label_error);
    }

    SECTION("Label key starting with number throws") {
        REQUIRE_THROWS_AS(validate_labels({{"1component", "test"}}), invalid_label_error);
    }

    SECTION("Label key starting with underscore throws") {
        REQUIRE_THROWS_AS(validate_labels({{"_component", "test"}}), invalid_label_error);
    }

    SECTION("Label key with invalid characters throws") {
        REQUIRE_THROWS_AS(validate_labels({{"component-name", "test"}}), invalid_label_error);
        REQUIRE_THROWS_AS(validate_labels({{"component.name", "test"}}), invalid_label_error);
        REQUIRE_THROWS_AS(validate_labels({{"component name", "test"}}), invalid_label_error);
    }

    SECTION("Empty label value is valid") {
        REQUIRE_NOTHROW(validate_labels({{"key", ""}}));
    }

    SECTION("Multiple labels are all validated") {
        REQUIRE_THROWS_AS(validate_labels({{"valid", "value"}, {"1invalid", "value"}}), invalid_label_error);
    }
}

TEST_CASE("Registry validates labels on metric creation", "[metrics][registry][validation][labels]") {
    registry::instance().clear();

    SECTION("Invalid label on counter throws") {
        REQUIRE_THROWS_AS(
            registry::instance().create_counter("label.test.counter", "Test counter", "1", {{"1invalid", "value"}}),
            invalid_label_error);
    }

    SECTION("Invalid label on gauge throws") {
        REQUIRE_THROWS_AS(
            registry::instance().create_gauge("label.test.gauge", "Test gauge", "1", {{"invalid-key", "value"}}),
            invalid_label_error);
    }

    SECTION("Invalid label on histogram throws") {
        REQUIRE_THROWS_AS(registry::instance().create_histogram("label.test.histogram", "Test histogram", "ms",
                                                                {1.0, 10.0}, {{"_invalid", "value"}}),
                          invalid_label_error);
    }

    SECTION("Invalid label on get_or_create also throws") {
        REQUIRE_THROWS_AS(
            registry::instance().get_or_create_counter("label.goc.counter", "Test", "1", {{"invalid.key", "value"}}),
            invalid_label_error);
    }
}

// ============================================================================
// Metric Limit Tests
// ============================================================================

TEST_CASE("Error handler for observer failures", "[metrics][registry][error_handling]") {
    registry::instance().clear();

    std::vector<std::string> error_names;
    std::vector<std::size_t> error_ids;

    SECTION("Error handler is called when observer throws") {
        // Set up error handler
        registry::instance().set_error_handler([&](std::size_t id, const std::string& name, std::exception_ptr) {
            error_ids.push_back(id);
            error_names.push_back(name);
        });

        // Add a throwing observer
        auto observer_id = registry::instance().add_observer(
            [](const metric_metadata&, void*) { throw std::runtime_error("Observer error"); }, false);

        // Create a metric - should not throw, but error handler should be called
        REQUIRE_NOTHROW(registry::instance().create_counter("error.test.counter"));

        REQUIRE(error_ids.size() == 1);
        REQUIRE(error_ids[0] == observer_id);
        REQUIRE(error_names[0] == "error.test.counter");

        // Clean up
        registry::instance().remove_observer(observer_id);
        registry::instance().set_error_handler(nullptr);
    }

    SECTION("No error handler means silent failure") {
        // Make sure no error handler is set
        registry::instance().set_error_handler(nullptr);

        // Add a throwing observer
        auto observer_id = registry::instance().add_observer(
            [](const metric_metadata&, void*) { throw std::runtime_error("Observer error"); }, false);

        // Create a metric - should not throw (error is silently ignored)
        REQUIRE_NOTHROW(registry::instance().create_counter("silent.error.counter"));

        // Clean up
        registry::instance().remove_observer(observer_id);
    }
}

TEST_CASE("Metric limit enforcement", "[metrics][registry][limits]") {
    registry::instance().clear();

    SECTION("Default limit is set") {
        REQUIRE(registry::instance().max_metrics() == registry::DEFAULT_MAX_METRICS);
    }

    SECTION("Set limit to small value and exceed it") {
        registry::instance().set_max_metrics(3);

        // Create 3 metrics - should succeed
        REQUIRE_NOTHROW(registry::instance().create_counter("limit.counter1"));
        REQUIRE_NOTHROW(registry::instance().create_gauge("limit.gauge1"));
        REQUIRE_NOTHROW(registry::instance().create_updown_counter("limit.updown1"));

        // Fourth metric should fail
        REQUIRE_THROWS_AS(registry::instance().create_counter("limit.counter2"), metric_limit_exceeded_error);

        // Reset limit to default for other tests
        registry::instance().set_max_metrics(registry::DEFAULT_MAX_METRICS);
    }

    SECTION("Limit of zero means unlimited") {
        registry::instance().set_max_metrics(0);

        // Should be able to create many metrics
        for (int i = 0; i < 100; ++i) {
            REQUIRE_NOTHROW(registry::instance().create_counter("unlimited.counter" + std::to_string(i)));
        }

        // Reset limit to default for other tests
        registry::instance().set_max_metrics(registry::DEFAULT_MAX_METRICS);
    }

    SECTION("get_or_create does not count against limit for existing metrics") {
        registry::instance().set_max_metrics(2);

        registry::instance().create_counter("limit.existing1");
        registry::instance().create_counter("limit.existing2");

        // get_or_create for existing metric should succeed even at limit
        REQUIRE_NOTHROW(registry::instance().get_or_create_counter("limit.existing1"));

        // But creating a new one should fail
        REQUIRE_THROWS_AS(registry::instance().get_or_create_counter("limit.new"), metric_limit_exceeded_error);

        registry::instance().set_max_metrics(registry::DEFAULT_MAX_METRICS);
    }
}

// ============================================================================
// Metric Removal Tests
// ============================================================================

TEST_CASE("Metric removal by name and labels", "[metrics][registry][removal]") {
    registry::instance().clear();

    SECTION("Remove counter returns true when found") {
        registry::instance().create_counter("remove.counter", "desc", "1", {{"env", "test"}});
        REQUIRE(registry::instance().metric_count() == 1);

        REQUIRE(registry::instance().remove_counter("remove.counter", {{"env", "test"}}));
        REQUIRE(registry::instance().metric_count() == 0);
    }

    SECTION("Remove counter returns false when not found") {
        registry::instance().create_counter("remove.counter");
        REQUIRE_FALSE(registry::instance().remove_counter("nonexistent.counter"));
        REQUIRE(registry::instance().metric_count() == 1);
    }

    SECTION("Remove gauge by name and labels") {
        registry::instance().create_gauge("remove.gauge", "desc", "1", {{"host", "localhost"}});
        REQUIRE(registry::instance().remove_gauge("remove.gauge", {{"host", "localhost"}}));
        REQUIRE(registry::instance().metric_count() == 0);
    }

    SECTION("Remove histogram by name and labels") {
        registry::instance().create_histogram("remove.histogram", "desc", "ms", {1.0, 10.0});
        REQUIRE(registry::instance().remove_histogram("remove.histogram"));
        REQUIRE(registry::instance().metric_count() == 0);
    }

    SECTION("Labels order doesn't matter for removal") {
        registry::instance().create_counter("remove.ordered", "desc", "1", {{"a", "1"}, {"b", "2"}});
        // Remove with labels in different order
        REQUIRE(registry::instance().remove_counter("remove.ordered", {{"b", "2"}, {"a", "1"}}));
        REQUIRE(registry::instance().metric_count() == 0);
    }
}

TEST_CASE("Bulk metric removal", "[metrics][registry][removal]") {
    registry::instance().clear();

    SECTION("Remove by prefix removes all matching metrics") {
        registry::instance().create_counter("mycomp.packets");
        registry::instance().create_counter("mycomp.bytes");
        registry::instance().create_gauge("mycomp.queue_depth");
        registry::instance().create_counter("other.counter");

        auto removed = registry::instance().remove_by_prefix("mycomp.");
        REQUIRE(removed == 3);
        REQUIRE(registry::instance().metric_count() == 1);
    }

    SECTION("Remove by prefix returns 0 when no matches") {
        registry::instance().create_counter("test.counter");
        auto removed = registry::instance().remove_by_prefix("nonexistent.");
        REQUIRE(removed == 0);
        REQUIRE(registry::instance().metric_count() == 1);
    }

    SECTION("Remove by label removes all metrics with matching label") {
        registry::instance().create_counter("comp1.packets", "desc", "1", {{"component", "comp1"}});
        registry::instance().create_gauge("comp1.depth", "desc", "1", {{"component", "comp1"}});
        registry::instance().create_counter("comp2.packets", "desc", "1", {{"component", "comp2"}});

        auto removed = registry::instance().remove_by_label("component", "comp1");
        REQUIRE(removed == 2);
        REQUIRE(registry::instance().metric_count() == 1);
    }
}

TEST_CASE("Deregistration observer notifications", "[metrics][registry][removal][observer]") {
    registry::instance().clear();

    SECTION("Observer is called when metric is removed") {
        std::vector<std::string> removed_names;
        auto observer_id = registry::instance().add_deregistration_observer(
            [&](const metric_metadata& meta, void*) { removed_names.push_back(meta.name); });

        registry::instance().create_counter("observer.test1");
        registry::instance().create_counter("observer.test2");

        registry::instance().remove_counter("observer.test1");
        REQUIRE(removed_names.size() == 1);
        REQUIRE(removed_names[0] == "observer.test1");

        registry::instance().remove_deregistration_observer(observer_id);
    }

    SECTION("Observer is called for bulk removal") {
        std::vector<std::string> removed_names;
        auto observer_id = registry::instance().add_deregistration_observer(
            [&](const metric_metadata& meta, void*) { removed_names.push_back(meta.name); });

        registry::instance().create_counter("bulk.a");
        registry::instance().create_counter("bulk.b");
        registry::instance().create_counter("other.c");

        registry::instance().remove_by_prefix("bulk.");
        REQUIRE(removed_names.size() == 2);

        registry::instance().remove_deregistration_observer(observer_id);
    }
}
