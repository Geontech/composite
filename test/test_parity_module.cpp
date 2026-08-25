/*
 * Copyright (C) 2026 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

// Minimal loadable component module for the POST /app/components parity regressions — the only
// dlopen'd component in the framework suite. It makes the behaviors the v0.5.0 known issue says
// the REST path skipped observable as properties:
//   - "initialized":    flipped by initialize(), so a missed initialize() reads back false;
//   - "trace_enabled":  whether the component's logger had the process-wide level applied
//                       before initialize() ran (the test sets the global level to trace);
//   - "observed_cpus":  the affinity mask the WORKER thread actually runs under, self-reported
//                       from process() — proving cpu_affinity was applied, not merely accepted;
//   - "init_delay_ms":  makes initialize() slow on demand, so the suite can prove that one
//                       stuck initializer does not head-of-line block unrelated creations.

#include <composite/core/component.hpp>
#include <composite/core/logger.hpp>
#include <composite/core/register.hpp>

#include <chrono>
#include <pthread.h>
#include <sched.h>
#include <string>
#include <thread>

class parity_probe : public composite::component {
public:
    explicit parity_probe(std::string_view id) : composite::component(id) {
        add_property("initialized", m_initialized);
        add_property("trace_enabled", m_trace_enabled);
        add_property("init_delay_ms", m_init_delay_ms);
        add_property("observed_cpus", m_observed_cpus, composite::properties::config_type::RUNTIME);
    }

    auto initialize() -> void override {
        m_initialized = true;
        m_trace_enabled = logger()->should_log(composite::log_level::trace);
        if (m_init_delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(m_init_delay_ms));
        }
    }

    auto process() -> composite::retval override {
        if (!m_reported) {
            m_reported = true; // worker-local: only this thread touches it
            cpu_set_t mask;
            CPU_ZERO(&mask);
            if (pthread_getaffinity_np(pthread_self(), sizeof(mask), &mask) == 0) {
                std::string csv;
                for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
                    if (CPU_ISSET(cpu, &mask)) {
                        if (!csv.empty()) {
                            csv += ",";
                        }
                        csv += std::to_string(cpu);
                    }
                }
                // Publish through the property system (worker self-write) so a concurrent REST
                // GET reads a committed value instead of racing a plain member store.
                set_properties({{"observed_cpus", csv}}, composite::properties::config_type::RUNTIME);
            }
        }
        return composite::retval::NOOP;
    }

private:
    bool m_initialized{false};
    bool m_trace_enabled{false};
    std::int32_t m_init_delay_ms{0};
    std::string m_observed_cpus{};
    bool m_reported{false};

    // MUST be last (stops the worker while the leaf is intact).
    composite::component::auto_stop m_auto_stop{*this};
};

COMPOSITE_REGISTER_SIMPLE(parity_probe)
