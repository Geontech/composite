// pipeline_component: out-of-order parallel work() must be re-serialised to exact
// submission order at finalize(). A "doubler" runs on a pool of workers with a
// per-packet variable delay (so later packets often finish first); finalize()
// asserts it observes packets in strict submission order with correct values.
// Verifies ordered retirement, parallelism (>1 worker used), no loss, and clean
// start/stop. Under TSan and ASan/UBSan.
#include "composite/buffers/buffer.hpp"
#include "composite/core/pipeline_component.hpp"
#include "composite/ports/output_port.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

#include <spdlog/spdlog.h>

using namespace composite;

namespace {
constexpr int W = 4;
constexpr std::int64_t N = 50000;

class doubler : public pipeline_component<mutable_buffer<std::int64_t>, mutable_buffer<std::int64_t>> {
public:
    doubler() : pipeline_component("doubler", "in", "out", W) {}

    std::int64_t m_expect{0}; // finalize() runs single-threaded -> plain int
    std::atomic<std::int64_t> m_finalized{0};
    std::atomic_bool m_bad{false};
    std::array<std::atomic<int>, 64> m_hits{};

protected:
    auto work(in_t in, timestamp /*ts*/, const composite::metadata& /*md*/) -> out_t override {
        const int wi = worker_index();
        if (wi >= 0 && wi < 64) {
            m_hits[static_cast<std::size_t>(wi)].fetch_add(1, std::memory_order_relaxed);
        }
        auto si = in.as_span();
        const std::int64_t seq = si.empty() ? 0 : si[0];
        std::this_thread::sleep_for(std::chrono::microseconds(seq % 7)); // jitter -> reorder
        auto out = make_mutable<std::int64_t>(si.size());
        auto so = out.as_span();
        for (std::size_t i = 0; i < si.size(); ++i) {
            so[i] = si[i] * 2;
        }
        return out;
    }

    auto finalize(out_t& out, timestamp /*ts*/, const composite::metadata& /*md*/) -> bool override {
        auto so = out.as_span();
        const std::int64_t seq = so.empty() ? -1 : so[0] / 2;
        if (seq != m_expect) {
            m_bad.store(true, std::memory_order_relaxed);
        } // SUBMISSION ORDER
        for (auto v : so) {
            if (v != seq * 2) {
                m_bad.store(true, std::memory_order_relaxed);
            }
        } // value/integrity
        ++m_expect;
        m_finalized.fetch_add(1, std::memory_order_relaxed);
        return false; // verified here; nothing downstream to send
    }

public:
    component::auto_stop m_auto_stop{*this}; // MUST be last
};
} // namespace

int main() {
    spdlog::set_level(spdlog::level::off);

    auto pipe = std::make_shared<doubler>();
    output_port<mutable_buffer<std::int64_t>> feeder{"feed"};
    auto* in = pipe->get_port<input_port_base>("in");
    if (in == nullptr || !feeder.connect(in)) {
        std::puts("FAIL: connect");
        return 1;
    }

    pipe->start();
    for (std::int64_t s = 0; s < N; ++s) {
        while (in->is_full()) {
            std::this_thread::yield();
        } // throttle -> no input drops
        auto b = make_mutable<std::int64_t>(4);
        for (auto& v : b.as_span()) {
            v = s;
        }
        feeder.send_data(std::move(b), timestamp{});
    }
    for (int i = 0; i < 4000 && pipe->m_finalized.load() < N; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    pipe->stop();

    if (pipe->m_bad.load()) {
        std::puts("FAIL: out-of-order retirement or wrong value");
        return 1;
    }
    if (pipe->m_finalized.load() != N) {
        std::printf("FAIL: finalized %lld of %lld\n", (long long)pipe->m_finalized.load(), (long long)N);
        return 1;
    }
    int used = 0;
    for (auto& h : pipe->m_hits) {
        if (h.load() > 0) {
            ++used;
        }
    }
    if (used < 2) {
        std::printf("FAIL: only %d worker(s) used (no parallelism)\n", used);
        return 1;
    }

    std::printf("PIPELINE OK: %lld packets, exact submission order, %d/%d workers used\n", (long long)N, used, W);

    // ---- stop with work still in flight: must not hang/crash/race ----
    {
        auto p2 = std::make_shared<doubler>();
        output_port<mutable_buffer<std::int64_t>> feed2{"feed"};
        auto* in2 = p2->get_port<input_port_base>("in");
        if (in2 == nullptr || !feed2.connect(in2)) {
            std::puts("FAIL: connect (B)");
            return 1;
        }
        p2->start();
        for (std::int64_t s = 0; s < 20000; ++s) {
            while (in2->is_full()) {
                std::this_thread::yield();
            }
            auto b = make_mutable<std::int64_t>(4);
            for (auto& v : b.as_span()) {
                v = s;
            }
            feed2.send_data(std::move(b), timestamp{});
        }
        p2->stop(); // immediately — workers are still busy, slots in flight
        if (p2->m_bad.load()) {
            std::puts("FAIL: out-of-order under early stop (B)");
            return 1;
        }
        std::printf("PIPELINE stop-in-flight OK: finalized %lld before stop, no hang/race\n",
                    (long long)p2->m_finalized.load());
    }

    // ---- runtime num_workers resize: order + zero loss across live resizes ----
    {
        auto p3 = std::make_shared<doubler>();
        output_port<mutable_buffer<std::int64_t>> feed3{"feed"};
        auto* in3 = p3->get_port<input_port_base>("in");
        if (in3 == nullptr || !feed3.connect(in3)) {
            std::puts("FAIL: connect (C)");
            return 1;
        }
        p3->start();
        constexpr std::int64_t M = 60000;
        std::thread resizer([&] {
            for (int nw : {8, 2, 6, 3, 5}) {
                std::this_thread::sleep_for(std::chrono::milliseconds(8));
                p3->set_properties(properties::json{{"num_workers", nw}}, properties::config_type::RUNTIME);
            }
        });
        for (std::int64_t s = 0; s < M; ++s) {
            while (in3->is_full()) {
                std::this_thread::yield();
            }
            auto b = make_mutable<std::int64_t>(4);
            for (auto& v : b.as_span()) {
                v = s;
            }
            feed3.send_data(std::move(b), timestamp{});
        }
        resizer.join();
        for (int i = 0; i < 5000 && p3->m_finalized.load() < M; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        p3->stop();
        if (p3->m_bad.load()) {
            std::puts("FAIL: out-of-order across live resize (C)");
            return 1;
        }
        if (p3->m_finalized.load() != M) {
            std::printf("FAIL: resize lost packets, finalized %lld of %lld (C)\n", (long long)p3->m_finalized.load(),
                        (long long)M);
            return 1;
        }
        std::printf("PIPELINE runtime-resize OK: %lld packets in exact order across live num_workers changes\n",
                    (long long)M);
    }

    // ---- prepared-metadata cache: prepare() runs per metadata CHANGE, not per packet ----
    {
        class stamper : public pipeline_component<mutable_buffer<std::int64_t>, mutable_buffer<std::int64_t>> {
        public:
            stamper() : pipeline_component("stamper", "in", "out", 1) {}
            std::atomic<int> m_prepares{0};
            std::atomic<std::int64_t> m_finalized{0};
            auto poke_invalidate() -> void { invalidate_prepared_metadata(); }

        protected:
            auto prepare(composite::metadata& md) -> void override {
                m_prepares.fetch_add(1, std::memory_order_relaxed);
                md.annotations["stamp"] = true;
            }
            auto work(in_t in, timestamp /*ts*/, const composite::metadata& /*md*/) -> out_t override { return in; }
            auto finalize(out_t& /*out*/, timestamp /*ts*/, const composite::metadata& md) -> bool override {
                if (!md.annotations.contains("stamp")) {
                    m_bad.store(true, std::memory_order_relaxed);
                }
                m_finalized.fetch_add(1, std::memory_order_relaxed);
                return false; // nothing downstream
            }

        public:
            std::atomic_bool m_bad{false};
            component::auto_stop m_auto_stop{*this}; // MUST be last
        };

        auto p4 = std::make_shared<stamper>();
        output_port<mutable_buffer<std::int64_t>> feed4{"feed"};
        auto* in4 = p4->get_port<input_port_base>("in");
        if (in4 == nullptr || !feed4.connect(in4)) {
            std::puts("FAIL: connect (D)");
            return 1;
        }
        p4->start();

        composite::metadata md;
        md.sample_rate = 1e6;
        const auto shared_md = make_metadata(std::move(md));
        auto send_and_wait = [&](composite::metadata_ptr m, std::int64_t upto) -> bool {
            feed4.send_data(make_mutable<std::int64_t>(4), timestamp{}, std::move(m));
            for (int i = 0; i < 2000 && p4->m_finalized.load() < upto; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return p4->m_finalized.load() == upto;
        };

        // Three packets, one shared instance: prepare() must run exactly once.
        for (std::int64_t s = 1; s <= 3; ++s) {
            if (!send_and_wait(shared_md, s)) {
                std::puts("FAIL: stamper stalled (D)");
                return 1;
            }
        }
        if (p4->m_prepares.load() != 1) {
            std::printf("FAIL: prepare() ran %d times for one metadata instance (want 1)\n", p4->m_prepares.load());
            return 1;
        }
        // Invalidation (a config change prepare() depends on) forces one rebuild.
        p4->poke_invalidate();
        if (!send_and_wait(shared_md, 4)) {
            std::puts("FAIL: stamper stalled post-invalidate (D)");
            return 1;
        }
        if (p4->m_prepares.load() != 2) {
            std::printf("FAIL: prepare() ran %d times after invalidate (want 2)\n", p4->m_prepares.load());
            return 1;
        }
        // A different incoming instance forces one rebuild.
        composite::metadata md2;
        md2.sample_rate = 2e6;
        if (!send_and_wait(make_metadata(std::move(md2)), 5)) {
            std::puts("FAIL: stamper stalled on new md (D)");
            return 1;
        }
        if (p4->m_prepares.load() != 3) {
            std::printf("FAIL: prepare() ran %d times after metadata change (want 3)\n", p4->m_prepares.load());
            return 1;
        }
        p4->stop();
        if (p4->m_bad.load()) {
            std::puts("FAIL: unstamped metadata reached finalize (D)");
            return 1;
        }
        std::puts("PIPELINE prepared-metadata cache OK: 1 prepare per change, shared across packets");
    }
    return 0;
}
