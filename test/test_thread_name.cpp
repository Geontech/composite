// Thread naming: pool workers of a pipeline_component, and the component's own worker, must show
// up under recognisable names in /proc/self/task/<tid>/comm. Also covers truncation, since Linux
// rejects names over 15 characters instead of shortening them.
#include "composite/buffers/buffer.hpp"
#include "composite/core/pipeline_component.hpp"
#include "composite/util/thread_name.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>

#include <spdlog/spdlog.h>

using namespace composite;

namespace {
constexpr int W = 3;

// 16 characters: one over the limit, so an untruncated name would be rejected outright.
constexpr const char* k_id = "channelizer_bank";

class namer : public pipeline_component<mutable_buffer<int>, mutable_buffer<int>> {
public:
    namer() : pipeline_component(k_id, "in", "out", W) {}

protected:
    auto work(in_t in, timestamp, const composite::metadata&) -> out_t override { return in; }
    auto finalize(out_t&, timestamp, const composite::metadata&) -> bool override { return false; }

public:
    component::auto_stop m_auto_stop{*this};
};

auto live_thread_names() -> std::set<std::string> {
    std::set<std::string> names;
    for (const auto& e : std::filesystem::directory_iterator("/proc/self/task")) {
        std::ifstream f(e.path() / "comm");
        std::string n;
        if (std::getline(f, n)) {
            names.insert(n);
        }
    }
    return names;
}

int failures = 0;

auto expect_eq(const std::string& got, const std::string& want, const char* what) -> void {
    if (got != want) {
        std::printf("FAIL: %s: got '%s', want '%s'\n", what, got.c_str(), want.c_str());
        ++failures;
    }
}
} // namespace

int main() {
    spdlog::set_level(spdlog::level::off);

    // ---- composition and truncation ----
    expect_eq(thread_name_with_suffix("short", "w1"), "short.w1", "short stem is left alone");
    expect_eq(thread_name_with_suffix(k_id, "w0"), "channelizer_.w0", "stem trimmed to fit");
    expect_eq(thread_name_with_suffix(k_id, "w11"), "channelizer.w11", "wider suffix takes stem room");
    if (thread_name_with_suffix(k_id, "w0").size() > k_max_thread_name) {
        std::puts("FAIL: composed name exceeds the platform limit");
        ++failures;
    }
    // A suffix that alone fills the budget: the distinguishing part still survives.
    expect_eq(thread_name_with_suffix("stem", "0123456789abcdef"), "0123456789abcde", "suffix-only fallback");

    // A name over the limit must still be applied (truncated), not silently dropped.
    if (!set_thread_name("this_name_is_far_too_long")) {
        std::puts("FAIL: set_thread_name rejected a long name instead of truncating");
        ++failures;
    }
    expect_eq(*live_thread_names().begin(), "this_name_is_fa", "calling thread renamed (truncated)");

    // ---- the pool actually gets named ----
    auto pipe = std::make_shared<namer>();
    pipe->start();

    std::set<std::string> seen;
    for (int i = 0; i < 500; ++i) {
        seen = live_thread_names();
        bool all = true;
        for (int w = 0; w < W; ++w) {
            if (!seen.count(thread_name_with_suffix(k_id, "w" + std::to_string(w)))) {
                all = false;
            }
        }
        if (all) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    for (int w = 0; w < W; ++w) {
        const auto want = thread_name_with_suffix(k_id, "w" + std::to_string(w));
        if (!seen.count(want)) {
            std::printf("FAIL: no pool thread named '%s'\n", want.c_str());
            ++failures;
        }
    }
    // The component's own worker thread is named after the (truncated) id.
    if (!seen.count(std::string{k_id}.substr(0, k_max_thread_name))) {
        std::printf("FAIL: no worker thread named '%.15s'\n", k_id);
        ++failures;
    }

    pipe->stop();

    if (failures != 0) {
        std::printf("THREAD NAME: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("THREAD NAME OK: %d pool workers + worker named, truncation correct\n", W);
    return 0;
}
