// Regression: do_resize() rebuilds a pipeline_component's pool as stop_pool() THEN start_pool().
// If start_pool() throws (its slot-ring allocation is the one array-new in the rebuild), the old
// pool is already gone -- leaving the component "running" with no workers would wedge it silently
// (the next packet sits in the slot ring forever; EOS never completes). The fix re-arms the resize
// and rethrows, so under error_restart_max>0 the base worker loop retries do_resize() (which rebuilds
// cleanly once the injected failure is spent), and with no restarts configured the component finishes
// with finish_reason::error instead of hanging.
//
// Global operator new[]/delete[] are replaced so exactly one array allocation can be made to throw
// std::bad_alloc on demand -- the only array-new sites reachable here are the slot ring's
// std::make_unique<slot[]> (pipeline_component::start_pool) and slab_pool's free-list bookkeeping
// (settled once by the WARMUP packet, before either scenario arms the failure), so the injected
// throw lands exactly on the rebuild it is meant to pin.
#include "composite/buffers/buffer.hpp"
#include "composite/core/pipeline_component.hpp"
#include "composite/ports/output_port.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <thread>

namespace {
std::atomic<bool> g_fail_next_array{false};
std::atomic<bool> g_array_alloc_failed{false};
} // namespace

void* operator new[](std::size_t n) {
    if (g_fail_next_array.exchange(false, std::memory_order_acq_rel)) {
        g_array_alloc_failed.store(true, std::memory_order_release);
        throw std::bad_alloc();
    }
    if (void* p = std::malloc(n)) {
        return p;
    }
    throw std::bad_alloc();
}
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace composite;
using namespace std::chrono_literals;
using json = composite::properties::json;
using composite::properties::config_type;

namespace {
int g_failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

// Single-worker pass-through pipeline; finalize() counts what reached it and drops (nothing
// downstream is needed to observe the resize behaviour).
class resize_probe : public pipeline_component<mutable_buffer<int>, mutable_buffer<int>> {
public:
    explicit resize_probe(std::string_view id) : pipeline_component(id, "in", "out", 1) {}
    auto work(in_t in, timestamp, const composite::metadata&) -> out_t override { return in; }
    auto finalize(out_t&, timestamp, const composite::metadata&) -> bool override {
        m_finalized.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    std::atomic<int> m_finalized{0};
    component::auto_stop m_auto_stop{*this};
};
} // namespace

int main() {
    // ---- error_restart_max > 0: a resize failure is retried, not fatal ----
    {
        auto c = std::make_shared<resize_probe>("resize-retry");
        output_port<mutable_buffer<int>> feeder{"feed"};
        auto* in = c->get_port<input_port_base>("in");
        check(in != nullptr && feeder.connect(in), "retry: connect feeder");

        c->set_properties(json{{"error_restart_max", 2}, {"error_restart_backoff_ms", 1}});
        c->start();
        feeder.send_data(make_mutable<int>(1), timestamp{});

        const auto warmup = std::chrono::steady_clock::now() + 5s;
        while (c->m_finalized.load(std::memory_order_relaxed) != 1 && std::chrono::steady_clock::now() < warmup) {
            std::this_thread::yield();
        }
        check(c->m_finalized.load(std::memory_order_relaxed) == 1, "retry: first packet finalized before arming");

        g_fail_next_array.store(true, std::memory_order_release);
        c->set_properties(json{{"num_workers", 4}}, config_type::RUNTIME);
        std::this_thread::sleep_for(100ms); // let the main worker notice, throw, back off, and retry

        feeder.send_data(make_mutable<int>(1), timestamp{});
        feeder.send_eos();

        check(c->wait_until_finished(5s), "retry: pipeline finished after the retried resize");
        check(g_array_alloc_failed.load(std::memory_order_acquire), "retry: the injected allocation actually failed");
        check(!c->is_running(), "retry: not running after finishing");
        check(c->m_finalized.load(std::memory_order_relaxed) == 2, "retry: both packets were finalized");

        c->stop();
        feeder.disconnect(in);
    }

    // ---- error_restart_max == 0: a resize failure finishes with error, not a hang ----
    {
        auto c = std::make_shared<resize_probe>("resize-giveup");
        output_port<mutable_buffer<int>> feeder{"feed"};
        auto* in = c->get_port<input_port_base>("in");
        check(in != nullptr && feeder.connect(in), "giveup: connect feeder");

        c->set_properties(json{{"error_restart_max", 0}, {"error_restart_backoff_ms", 1}});
        c->start();
        feeder.send_data(make_mutable<int>(1), timestamp{});

        const auto warmup = std::chrono::steady_clock::now() + 5s;
        while (c->m_finalized.load(std::memory_order_relaxed) != 1 && std::chrono::steady_clock::now() < warmup) {
            std::this_thread::yield();
        }
        check(c->m_finalized.load(std::memory_order_relaxed) == 1, "giveup: first packet finalized before arming");

        g_array_alloc_failed.store(false, std::memory_order_release);
        g_fail_next_array.store(true, std::memory_order_release);
        c->set_properties(json{{"num_workers", 4}}, config_type::RUNTIME);

        // Bounded: with the bug this would hang forever with no workers and a stuck slot ring.
        check(c->wait_until_finished(5s), "giveup: bounded wait succeeds (no hang) after the resize failure");
        check(g_array_alloc_failed.load(std::memory_order_acquire), "giveup: the injected allocation actually failed");
        check(!c->is_running(), "giveup: not running after finishing");
        check(c->finished_reason() == finish_reason::error, "giveup: finished with finish_reason::error");
        check(c->finish_error().find("bad_alloc") != std::string::npos, "giveup: finish_error() mentions bad_alloc");

        c->stop();
        feeder.disconnect(in);
    }

    if (g_failures) {
        std::printf("\n%d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::puts("PIPELINE RESIZE FAILURE OK: a do_resize() rebuild failure re-arms and retries under "
              "error_restart_max>0 (no data loss), and finishes with finish_reason::error -- not a "
              "hang -- with no restarts configured");
    return 0;
}
