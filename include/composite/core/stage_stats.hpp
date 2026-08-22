/*
 * Copyright (C) 2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "composite/metrics/metrics.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace composite {

/**
 * @brief Per-stage throughput, occupancy, and latency metrics
 *
 * Companion to port_stats. port_stats answers "what crossed this port";
 * stage_stats answers "what did the component do with it" — frames and samples
 * in versus out, how long process() was busy, where work was shed, and the
 * latency distribution.
 *
 * Metrics are named "composite.stage.<name>" and carry a component_id label:
 *
 * | Metric                           | Type      | Extra labels |
 * | -------------------------------- | --------- | ------------ |
 * | composite.stage.frames_in        | counter   |              |
 * | composite.stage.frames_out       | counter   |              |
 * | composite.stage.samples_in       | counter   |              |
 * | composite.stage.samples_out      | counter   |              |
 * | composite.stage.frames_dropped   | counter   | reason       |
 * | composite.stage.busy             | counter   |              |
 * | composite.stage.kernel           | counter   |              |
 * | composite.stage.process_duration | histogram |              |
 * | composite.stage.queue_depth      | gauge     |              |
 *
 * "Frame" means one process() unit of work, whatever the stage's payload
 * happens to be — a sample block upstream of the framer, an NrFrame
 * downstream. Keeping one name lets a single dashboard panel plot the rate
 * across every stage in a chain, so a bottleneck reads as a step down.
 *
 * The `reason` label on frames_dropped exists so one metric family covers
 * every way a stage can shed work (no upstream sync, empty buffer, short
 * frame, no worker available, ...) instead of each component inventing its own
 * counter name. Reasons are registered up front so recording one never
 * allocates.
 *
 * Usage:
 * @code
 * // In the constructor or initialize():
 * static constexpr std::string_view reasons[] = {"no_sync", "empty"};
 * m_stats.register_metrics(id(), reasons);
 *
 * // In process():
 * const auto t0 = std::chrono::steady_clock::now();
 * ...
 * m_stats.record(samples_in, samples_out, std::chrono::steady_clock::now() - t0);
 * @endcode
 */
struct stage_stats {
    /**
     * @brief Default bucket boundaries for process() duration, in seconds
     *
     * Spans 100 us to 1 s, which covers the observed per-frame range of an RF
     * chain (a mixer at ~1 ms through a detector at ~13 ms) with enough
     * resolution either side to make a p99 meaningful.
     */
    [[nodiscard]]
    static auto default_duration_boundaries() -> std::vector<double> {
        return {
            0.0001, 0.00025, 0.0005,
            0.001,  0.0025,  0.005,
            0.01,   0.025,   0.05,
            0.1,    0.25,    0.5,
            1.0,    2.5,     5.0,
            10.0
        };
    }

    /**
     * @brief Register this stage's metrics
     *
     * Idempotent — the registry returns existing instruments for a repeated
     * name plus label set, so a re-run of initialize() is safe.
     *
     * @param component_id ID of the owning component
     * @param drop_reasons Reason labels to pre-register for record_drop()
     * @param duration_boundaries Histogram bucket bounds in seconds
     */
    auto register_metrics(
        std::string_view component_id,
        std::span<const std::string_view> drop_reasons = {},
        std::vector<double> duration_boundaries = default_duration_boundaries()
    ) -> void {
        auto& registry = metrics::registry::instance();

        metrics::labels_t labels = {{"component_id", std::string{component_id}}};

        m_frames_in = &registry.get_or_create_counter(
            "composite.stage.frames_in",
            "Units of work consumed by this stage",
            "1",
            labels
        );

        m_frames_out = &registry.get_or_create_counter(
            "composite.stage.frames_out",
            "Units of work emitted by this stage",
            "1",
            labels
        );

        m_samples_in = &registry.get_or_create_counter(
            "composite.stage.samples_in",
            "Samples consumed by this stage",
            "1",
            labels
        );

        m_samples_out = &registry.get_or_create_counter(
            "composite.stage.samples_out",
            "Samples emitted by this stage",
            "1",
            labels
        );

        // Nanoseconds rather than seconds because counters are integral;
        // dashboards divide by 1e9 to get a duty cycle.
        m_busy = &registry.get_or_create_counter(
            "composite.stage.busy",
            "Cumulative time spent inside process()",
            "ns",
            labels
        );

        // Kernel time is the DSP work proper, excluding buffer allocation,
        // metadata handling, and port traffic. busy minus kernel is framework
        // overhead, which is what separates "compute-bound" from "we are
        // spending the frame budget on allocation". Stays zero for stages that
        // do not distinguish the two.
        m_kernel = &registry.get_or_create_counter(
            "composite.stage.kernel",
            "Cumulative time spent in the stage's compute kernel",
            "ns",
            labels
        );

        m_queue_depth = &registry.get_or_create_gauge(
            "composite.stage.queue_depth",
            "Work items awaiting processing by this stage",
            "1",
            labels
        );

        m_duration = &registry.get_or_create_histogram(
            "composite.stage.process_duration",
            "Distribution of process() durations",
            "s",
            std::move(duration_boundaries),
            labels
        );

        m_drops.clear();
        m_drops.reserve(drop_reasons.size());
        for (const auto reason : drop_reasons) {
            auto drop_labels = labels;
            drop_labels.emplace_back("reason", std::string{reason});
            m_drops.emplace_back(
                std::string{reason},
                &registry.get_or_create_counter(
                    "composite.stage.frames_dropped",
                    "Units of work consumed but not emitted, by reason",
                    "1",
                    std::move(drop_labels)
                )
            );
        }
    }

    /**
     * @brief Check whether register_metrics() has run
     */
    [[nodiscard]]
    auto is_registered() const -> bool {
        return m_frames_in != nullptr;
    }

    /**
     * @brief Record one completed unit of work
     *
     * @param samples_in Samples consumed
     * @param samples_out Samples emitted (equal to samples_in for 1:1 stages)
     * @param busy Time spent in process() for this unit
     * @param kernel Time spent in the compute kernel, if the stage measures it
     */
    auto record(
        std::size_t samples_in,
        std::size_t samples_out,
        std::chrono::nanoseconds busy,
        std::chrono::nanoseconds kernel = std::chrono::nanoseconds::zero()
    ) -> void {
        if (m_frames_in == nullptr) {
            return;
        }
        m_frames_in->add(1);
        m_frames_out->add(1);
        m_samples_in->add(samples_in);
        m_samples_out->add(samples_out);

        const auto busy_ns = busy.count();
        if (busy_ns > 0) {
            m_busy->add(static_cast<uint64_t>(busy_ns));
            m_duration->record(static_cast<double>(busy_ns) / 1e9);
        }
        const auto kernel_ns = kernel.count();
        if (kernel_ns > 0) {
            m_kernel->add(static_cast<uint64_t>(kernel_ns));
        }
    }

    /**
     * @brief Record time spent, without recording a unit of work
     *
     * For a stage whose process() emits a variable number of frames per call,
     * passing the call duration to record() would multiply busy and the latency
     * histogram by the frames-per-call factor. Such a stage calls record() per
     * frame with a zero duration to keep the counts honest, and calls this once
     * per process() with the wall time.
     */
    auto record_busy(
        std::chrono::nanoseconds busy,
        std::chrono::nanoseconds kernel = std::chrono::nanoseconds::zero()
    ) -> void {
        if (m_busy == nullptr) {
            return;
        }
        const auto busy_ns = busy.count();
        if (busy_ns > 0) {
            m_busy->add(static_cast<uint64_t>(busy_ns));
            m_duration->record(static_cast<double>(busy_ns) / 1e9);
        }
        const auto kernel_ns = kernel.count();
        if (kernel_ns > 0) {
            m_kernel->add(static_cast<uint64_t>(kernel_ns));
        }
    }

    /**
     * @brief Record a unit consumed but not emitted
     *
     * No-op for a reason that was not passed to register_metrics(), so a typo
     * loses a data point rather than growing the registry at runtime.
     */
    auto record_drop(std::string_view reason) -> void {
        if (m_frames_in == nullptr) {
            return;
        }
        m_frames_in->add(1);
        // Linear scan over a handful of reasons; drops are off the fast path.
        for (const auto& [name, drop_counter] : m_drops) {
            if (name == reason) {
                drop_counter->add(1);
                return;
            }
        }
    }

    /**
     * @brief Set the number of work items waiting on this stage
     */
    auto set_queue_depth(double depth) -> void {
        if (m_queue_depth != nullptr) {
            m_queue_depth->set(depth);
        }
    }

private:
    metrics::counter<uint64_t>* m_frames_in{nullptr};
    metrics::counter<uint64_t>* m_frames_out{nullptr};
    metrics::counter<uint64_t>* m_samples_in{nullptr};
    metrics::counter<uint64_t>* m_samples_out{nullptr};
    metrics::counter<uint64_t>* m_busy{nullptr};
    metrics::counter<uint64_t>* m_kernel{nullptr};
    metrics::gauge<double>* m_queue_depth{nullptr};
    metrics::histogram* m_duration{nullptr};
    std::vector<std::pair<std::string, metrics::counter<uint64_t>*>> m_drops;

}; // struct stage_stats

} // namespace composite
