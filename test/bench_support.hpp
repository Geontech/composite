/*
 * Copyright (C) 2026 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

// Shared support for the benchmark binaries (bench_datapath, bench_registry): sample
// statistics, environment capture, and the versioned JSON result artifact. Test-tree only —
// deliberately NOT installed; the JSON schema (schema_version below) is the stable contract,
// not this header.

#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <sched.h>
#include <string>
#include <string_view>
#include <sys/utsname.h>
#include <unistd.h>
#include <vector>

namespace bench {

/// Schema of the JSON artifact. Bump on any breaking change to the layout below; baseline
/// files record the value so a comparison across harness revisions is refused, not garbled.
inline constexpr int schema_version = 1;

/// Revision of the MEASUREMENT DEFINITIONS (what the cases time and how). Bump whenever a
/// change would make old samples incomparable even under the same schema — this is the
/// harness half of provenance: the artifact is committed alongside the harness, so it cannot
/// record its own future commit hash.
inline constexpr int harness_revision = 2; // 1 = the pre-Baseline-0 table-only harness

// ------------------------------------------------------------------------ failure handling

/// Set when a measurement INVARIANT breaks (packet loss on a lossless chain, a timed-out
/// latency trial, a wrong-sized snapshot). A failed run must produce NO artifact and exit
/// nonzero — a warning printed next to a number is a number somebody will baseline against.
inline bool g_failed = false;

inline auto fail(const std::string& what) -> void {
    std::fprintf(stderr, "BENCH INVARIANT FAILED: %s\n", what.c_str());
    g_failed = true;
}

// ---------------------------------------------------------------------------- statistics

/// Nearest-rank percentile over a SORTED sample vector: the smallest value such that at
/// least ceil(p/100 * N) samples are <= it. For N=100, p99 selects index 98 — the previous
/// hand-rolled `[(N*99)/100]` selected index 99, i.e. the maximum.
inline auto percentile_sorted(const std::vector<double>& sorted, double p) -> double {
    if (sorted.empty()) {
        return 0.0;
    }
    auto rank = static_cast<std::size_t>(std::ceil(p / 100.0 * static_cast<double>(sorted.size())));
    rank = std::clamp<std::size_t>(rank, 1, sorted.size());
    return sorted[rank - 1];
}

struct stats {
    double median{};
    double p95{};
    double p99{};
    double min{};
    double max{};
    double mean{};
    double stddev{};
    double cv{}; ///< stddev / mean — the dimensionless spread tolerance bands derive from
};

inline auto compute_stats(std::vector<double> samples) -> stats {
    stats s{};
    if (samples.empty()) {
        return s;
    }
    std::sort(samples.begin(), samples.end());
    const auto n = static_cast<double>(samples.size());
    s.min = samples.front();
    s.max = samples.back();
    s.median = percentile_sorted(samples, 50.0);
    s.p95 = percentile_sorted(samples, 95.0);
    s.p99 = percentile_sorted(samples, 99.0);
    for (const double v : samples) {
        s.mean += v;
    }
    s.mean /= n;
    double acc = 0.0;
    for (const double v : samples) {
        acc += (v - s.mean) * (v - s.mean);
    }
    s.stddev = samples.size() > 1 ? std::sqrt(acc / (n - 1.0)) : 0.0;
    s.cv = s.mean != 0.0 ? s.stddev / s.mean : 0.0;
    return s;
}

// ------------------------------------------------------------------- environment capture

inline auto read_first_line(const char* path) -> std::string {
    std::ifstream f{path};
    std::string line;
    if (f && std::getline(f, line)) {
        return line;
    }
    return {};
}

inline auto cpu_model() -> std::string {
    std::ifstream f{"/proc/cpuinfo"};
    std::string line;
    while (f && std::getline(f, line)) {
        if (line.starts_with("model name")) {
            const auto colon = line.find(':');
            if (colon != std::string::npos) {
                const auto start = line.find_first_not_of(" \t", colon + 1);
                return start != std::string::npos ? line.substr(start) : std::string{};
            }
        }
    }
    return {};
}

inline auto affinity_cpu_count() -> int {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof(set), &set) == 0) {
        return CPU_COUNT(&set);
    }
    return -1;
}

/// Everything the roadmap's Baseline-0 scope asks a result artifact to identify. Provenance is
/// TWO commits: `framework_commit` names the production code being measured (--framework-commit;
/// for a release baseline, the release tag's commit), and `harness_commit` names the harness
/// revision that measured it (--harness-commit; in CI both are $CI_COMMIT_SHA, while a baseline
/// committed together with its harness passes "committed-with-artifact" — the artifact cannot
/// know the hash of the commit it will land in, which is what `harness_revision` covers).
/// Commits come from the caller: a build-time define goes stale across rebuilds.
inline auto capture_environment(std::string framework_commit, std::string harness_commit) -> nlohmann::json {
    utsname un{};
    const bool have_uname = ::uname(&un) == 0;
    char host[256] = {};
    ::gethostname(host, sizeof(host) - 1);

    char stamp[32] = {};
    const auto now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));

    return {
        {"framework_commit", std::move(framework_commit)},
        {"harness_commit", harness_commit.empty() ? "committed-with-artifact" : std::move(harness_commit)},
        {"harness_revision", harness_revision},
        {"compiler", __VERSION__},
        // The two build facts that make numbers comparable, recorded SEPARATELY (a build can
        // be optimized with assertions live — the exact miscapture this field exposed once).
#ifdef NDEBUG
        {"assertions", "disabled"},
#else
        {"assertions", "live"},
#endif
#ifdef __OPTIMIZE__
        {"optimized", true},
#else
        {"optimized", false},
#endif
#ifdef BENCH_CMAKE_CONFIG
        {"cmake_config", BENCH_CMAKE_CONFIG},
#endif
        {"kernel", have_uname ? std::string{un.release} : std::string{}},
        {"arch", have_uname ? std::string{un.machine} : std::string{}},
        {"cpu", cpu_model()},
        {"affinity_cpus", affinity_cpu_count()},
        {"governor", read_first_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")},
        {"host", host},
        {"timestamp_utc", stamp},
    };
}

// ------------------------------------------------------------------------------ reporting

struct case_result {
    std::string name;                                ///< stable identifier, e.g. "spsc.handoff.shared"
    std::map<std::string, std::string> params;       ///< e.g. {"elems": "256"}
    std::string unit;                                ///< unit of every sample, e.g. "Mpkt/s"
    std::vector<double> samples;                     ///< one value per repetition (or per trial)
    std::string notes;                               ///< measurement definition caveats, if any
};

class reporter {
public:
    auto add(case_result r) -> void { m_cases.push_back(std::move(r)); }

    /// Write the artifact. Returns false (and prints why) if the file cannot be written.
    auto write(const std::string& path, const nlohmann::json& meta) -> bool {
        nlohmann::json doc{
            {"schema_version", schema_version},
            {"meta", meta},
            {"cases", nlohmann::json::array()},
        };
        for (const auto& c : m_cases) {
            const auto s = compute_stats(c.samples);
            nlohmann::json jc{
                {"name", c.name},          {"params", c.params}, {"unit", c.unit},
                {"samples_n", c.samples.size()},
                {"samples", c.samples},    {"median", s.median}, {"mean", s.mean},
                {"min", s.min},            {"max", s.max},       {"stddev", s.stddev},
                {"cv", s.cv},
            };
            // Tail percentiles only where the sample count can support them: with a handful
            // of repetitions p99 IS the max, which is exactly the misreading this schema is
            // meant to prevent.
            if (c.samples.size() >= 20) {
                jc["p95"] = s.p95;
            }
            if (c.samples.size() >= 100) {
                jc["p99"] = s.p99;
            }
            if (!c.notes.empty()) {
                jc["notes"] = c.notes;
            }
            doc["cases"].push_back(std::move(jc));
        }
        std::ofstream f{path};
        if (!f) {
            std::fprintf(stderr, "bench: cannot write %s\n", path.c_str());
            return false;
        }
        f << doc.dump(2) << '\n';
        return static_cast<bool>(f);
    }

private:
    std::vector<case_result> m_cases;
};

} // namespace bench
