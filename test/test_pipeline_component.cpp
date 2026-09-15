// pipeline_component: out-of-order parallel work() must be re-serialised to exact
// submission order at finalize(). A "doubler" runs on a pool of workers with a
// per-packet variable delay (so later packets often finish first); finalize()
// asserts it observes packets in strict submission order with correct values.
// Verifies ordered retirement, parallelism (>1 worker used), no loss, and clean
// start/stop. Under TSan and ASan/UBSan.
#include "composite/buffers/buffer.hpp"
#include "composite/core/pipeline_component.hpp"
#include "composite/ports/output_port.hpp"
#include "composite/properties/snapshot.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

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

    // ---- (E) FR-1 ingest context: EXACT per-packet identity under runtime churn ----
    // Acceptance: a subclass overriding ingest_context() + the 4-arg work() observes, for
    // every packet, the exact context instance captured at that packet's ingest — under
    // concurrent RUNTIME property writes (which publish new snapshots) AND live pool resizes.
    {
        class ctx_pipe : public pipeline_component<mutable_buffer<std::int64_t>, mutable_buffer<std::int64_t>> {
        public:
            explicit ctx_pipe(std::int64_t total)
                : pipeline_component("ctxpipe", "in", "out", 4), m_got(static_cast<std::size_t>(total), nullptr) {
                m_snap.publish(std::make_shared<const int>(0));
                add_property("gen", m_gen, properties::config_type::RUNTIME)
                    .on_change([this](const properties::json&) { m_snap.publish(std::make_shared<const int>(m_gen)); });
            }
            std::vector<const void*> m_expected; ///< arrival order, main worker only; read after stop()
            std::vector<const void*> m_got;      ///< work() writes index [seq] only — disjoint slots
            std::atomic<std::int64_t> m_finalized{0};
            auto current_ctx() -> std::shared_ptr<const int> { return m_snap.load(); }

        protected:
            auto ingest_context() -> std::shared_ptr<const void> override {
                auto c = std::static_pointer_cast<const void>(m_snap.load());
                m_expected.push_back(c.get()); // what THIS packet must see in work()
                return c;
            }
            auto work(in_t in, timestamp /*ts*/, const composite::metadata& /*md*/,
                      const std::shared_ptr<const void>& ctx) -> out_t override {
                const std::int64_t seq = in.as_span()[0];
                if (seq >= 0 && static_cast<std::size_t>(seq) < m_got.size()) {
                    m_got[static_cast<std::size_t>(seq)] = ctx.get();
                }
                std::this_thread::sleep_for(std::chrono::microseconds(seq % 5)); // reorder pressure
                return in;
            }
            auto finalize(out_t& /*out*/, timestamp /*ts*/, const composite::metadata& /*md*/) -> bool override {
                m_finalized.fetch_add(1, std::memory_order_relaxed);
                return false;
            }

        private:
            composite::snapshot<int> m_snap;
            int m_gen{0};

        public:
            component::auto_stop m_auto_stop{*this}; // MUST be last
        };

        constexpr std::int64_t E = 40000;
        auto p5 = std::make_shared<ctx_pipe>(E);
        output_port<mutable_buffer<std::int64_t>> feed5{"feed"};
        auto* in5 = p5->get_port<input_port_base>("in");
        if (in5 == nullptr || !feed5.connect(in5)) {
            std::puts("FAIL: connect (E)");
            return 1;
        }
        p5->start();

        // Churn: RUNTIME generation writes (each publishes a NEW snapshot instance) and live
        // pool resizes, concurrent with the feed.
        std::thread churner([&] {
            int gen = 1;
            for (int round = 0; round < 40; ++round) {
                p5->set_properties(properties::json{{"gen", gen++}}, properties::config_type::RUNTIME);
                if (round % 10 == 4) {
                    const int nw = 2 + (round % 7);
                    p5->set_properties(properties::json{{"num_workers", nw}}, properties::config_type::RUNTIME);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
        for (std::int64_t s = 0; s < E; ++s) {
            while (in5->is_full()) {
                std::this_thread::yield();
            }
            auto b = make_mutable<std::int64_t>(1);
            b.as_span()[0] = s;
            feed5.send_data(std::move(b), timestamp{});
        }
        churner.join();
        for (int i = 0; i < 5000 && p5->m_finalized.load() < E; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        p5->stop(); // join = happens-before for reading m_expected/m_got below

        if (p5->m_finalized.load() != E) {
            std::printf("FAIL: ctx pipe finalized %lld of %lld (E)\n", (long long)p5->m_finalized.load(),
                        (long long)E);
            return 1;
        }
        if (p5->m_expected.size() != static_cast<std::size_t>(E)) {
            std::printf("FAIL: ingest_context ran %zu times for %lld packets (E)\n", p5->m_expected.size(),
                        (long long)E);
            return 1;
        }
        // Single producer + FIFO ring + arrival-order ingest => arrival index == seq.
        std::size_t distinct = 1;
        for (std::int64_t s = 0; s < E; ++s) {
            const auto us = static_cast<std::size_t>(s);
            if (p5->m_got[us] != p5->m_expected[us]) {
                std::printf("FAIL: packet %lld saw ctx %p, ingest captured %p (E)\n", (long long)s, p5->m_got[us],
                            p5->m_expected[us]);
                return 1;
            }
            if (s > 0 && p5->m_expected[us] != p5->m_expected[us - 1]) {
                ++distinct;
            }
        }
        if (distinct < 10) {
            std::printf("FAIL: only %zu distinct context generations observed — churn was vacuous (E)\n", distinct);
            return 1;
        }
        std::printf("PIPELINE ingest-context OK: %lld packets, exact per-packet identity across %zu generations\n",
                    (long long)E, distinct);

        // ---- (F) the slot releases its context at retire ----
        // Publish a fresh generation, run ONE packet through, then publish another so the
        // subclass no longer holds the old instance: the only remaining owner would be the
        // slot, and retire must have released it.
        p5->start();
        p5->set_properties(properties::json{{"gen", 777}}, properties::config_type::RUNTIME);
        std::weak_ptr<const int> token = p5->current_ctx();
        {
            auto b = make_mutable<std::int64_t>(1);
            b.as_span()[0] = 0; // reuse slot 0 of m_got; identity already validated above
            feed5.send_data(std::move(b), timestamp{});
        }
        for (int i = 0; i < 2000 && p5->m_finalized.load() < E + 1; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (p5->m_finalized.load() != E + 1) {
            std::puts("FAIL: retire-release packet stalled (F)");
            return 1;
        }
        p5->set_properties(properties::json{{"gen", 778}}, properties::config_type::RUNTIME);
        bool released = false;
        for (int i = 0; i < 2000 && !released; ++i) {
            released = token.expired();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        p5->stop();
        if (!released) {
            std::puts("FAIL: slot retained the ingest context after retire (F)");
            return 1;
        }
        std::puts("PIPELINE ingest-context release OK: slot drops its context at retire");
    }
    // ---- (G) FR-4: reject an excessive request before committing or resizing ----
    {
        class limited_pipe : public pipeline_component<mutable_buffer<int>, mutable_buffer<int>> {
        public:
            limited_pipe(int initial = 1, int ceiling = 2)
                : pipeline_component("limited", "in", "out", initial, ceiling) {}
            std::atomic<int> workers{0};
            std::atomic<int> resizes{0};

        protected:
            auto work(in_t in, timestamp, const composite::metadata&) -> out_t override { return in; }
            auto on_workers_resized(int n) -> void override {
                workers.store(n);
                resizes.fetch_add(1);
            }
        };

        for (auto [initial, ceiling] : {std::pair{0, 2}, {3, 2}, {1, 0}, {1, 1025}}) {
            bool rejected = false;
            try {
                auto invalid = make_component<limited_pipe>(initial, ceiling);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            if (!rejected) {
                std::puts("FAIL: invalid initial worker count or ceiling accepted (G)");
                return 1;
            }
        }

        auto limited = make_component<limited_pipe>();
        auto rejects = [&](int n, properties::config_type phase) {
            try {
                limited->set_properties(properties::json{{"num_workers", n}}, phase);
            } catch (const properties::validation_error&) {
                return true;
            }
            return false;
        };
        if (!rejects(3, properties::config_type::INITIALIZE) || limited->get_property<int>("num_workers") != 1) {
            std::puts("FAIL: worker ceiling not enforced during initialization (G)");
            return 1;
        }
        limited->start();
        if (!rejects(3, properties::config_type::RUNTIME) || !rejects(0, properties::config_type::RUNTIME) ||
            limited->get_property<int>("num_workers") != 1 || limited->resizes.load() != 1) {
            std::puts("FAIL: rejected worker write committed or resized the pool (G)");
            return 1;
        }
        limited->set_properties(properties::json{{"num_workers", 2}}, properties::config_type::RUNTIME);
        for (int i = 0; i < 2000 && limited->workers.load() != 2; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        limited->stop();
        if (limited->workers.load() != 2 || limited->resizes.load() != 2) {
            std::puts("FAIL: valid boundary worker count did not resize the pool (G)");
            return 1;
        }
        // Omitting the new argument preserves the former 1..1024 property range.
        auto legacy = make_component<doubler>();
        legacy->set_properties(properties::json{{"num_workers", 1024}}, properties::config_type::INITIALIZE);
        if (legacy->get_property<int>("num_workers") != 1024) {
            std::puts("FAIL: default worker ceiling changed (G)");
            return 1;
        }
        std::puts("PIPELINE worker ceiling OK: constructor/init/runtime validation and valid resize");
    }

    // ---- finish_at_end=false: a pipeline_component must NOT self-FINISH at EOS when opted out ----
    {
        class idler : public pipeline_component<mutable_buffer<std::int64_t>, mutable_buffer<std::int64_t>> {
        public:
            idler() : pipeline_component("idler", "in", "out", 1) {}
            std::atomic<int> m_finalized{0};

        protected:
            auto work(in_t in, timestamp /*ts*/, const composite::metadata& /*md*/) -> out_t override { return in; }
            auto finalize(out_t& /*out*/, timestamp /*ts*/, const composite::metadata& /*md*/) -> bool override {
                m_finalized.fetch_add(1, std::memory_order_relaxed);
                return false; // nothing downstream
            }

        public:
            component::auto_stop m_auto_stop{*this}; // MUST be last
        };

        auto p5 = std::make_shared<idler>();
        p5->set_properties(properties::json{{"finish_at_end", false}}); // RUNTIME property, set before start
        output_port<mutable_buffer<std::int64_t>> feed5{"feed"};
        auto* in5 = p5->get_port<input_port_base>("in");
        if (in5 == nullptr || !feed5.connect(in5)) {
            std::puts("FAIL: connect (H)");
            return 1;
        }
        p5->start();
        constexpr int K = 5;
        for (int s = 0; s < K; ++s) {
            feed5.send_data(make_mutable<std::int64_t>(4), timestamp{});
        }
        feed5.send_eos();
        for (int i = 0; i < 2000 && p5->m_finalized.load() < K; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (p5->m_finalized.load() != K) {
            std::printf("FAIL: idler finalized %d of %d before EOS drained (H)\n", p5->m_finalized.load(), K);
            p5->stop();
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300)); // generous wait past EOS
        if (!p5->is_running() || p5->is_finished()) {
            std::puts("FAIL: finish_at_end=false pipeline_component self-finished at EOS (H)");
            p5->stop();
            return 1;
        }
        p5->stop();
        std::puts("PIPELINE finish_at_end=false OK: stays running past EOS");
    }

    // ---- on_end_of_stream(): fires exactly once, may emit a tail packet, and that packet reaches a
    //      downstream sink BEFORE the sink observes end-of-stream ----
    {
        constexpr std::int64_t kTailMark = -1;

        class tail_pipe : public pipeline_component<mutable_buffer<std::int64_t>, mutable_buffer<std::int64_t>> {
        public:
            tail_pipe() : pipeline_component("tailpipe", "in", "out", 1) {}
            std::atomic<int> m_eos_calls{0};

        protected:
            auto work(in_t in, timestamp /*ts*/, const composite::metadata& /*md*/) -> out_t override { return in; }
            auto on_end_of_stream() -> void override {
                m_eos_calls.fetch_add(1, std::memory_order_relaxed);
                auto tail = make_mutable<std::int64_t>(1);
                tail.as_span()[0] = kTailMark;
                out_port().send_data(std::move(tail), timestamp{});
            }

        public:
            component::auto_stop m_auto_stop{*this}; // MUST be last
        };

        class tail_sink : public component {
        public:
            explicit tail_sink(std::string_view id) : component(id) { add_port(&m_in); }
            auto process() -> retval override {
                const int call = m_calls.fetch_add(1, std::memory_order_relaxed);
                if (auto pkt = m_in.try_get()) {
                    auto& [buf, ts, md] = *pkt;
                    m_received.fetch_add(1, std::memory_order_relaxed);
                    auto sp = buf.as_span();
                    if (!sp.empty() && sp[0] == kTailMark) {
                        m_tail_call.store(call, std::memory_order_relaxed);
                    }
                    return retval::NORMAL;
                }
                if (inputs_at_end()) {
                    m_eos_call.store(call, std::memory_order_relaxed);
                    return retval::FINISH;
                }
                return retval::NOOP;
            }
            input_port<mutable_buffer<std::int64_t>> m_in{"in"};
            std::atomic<int> m_calls{0};
            std::atomic<int> m_received{0};
            std::atomic<int> m_tail_call{-1};
            std::atomic<int> m_eos_call{-1};
            component::auto_stop m_auto_stop{*this}; // MUST be last
        };

        auto p6 = std::make_shared<tail_pipe>();
        auto sink6 = std::make_shared<tail_sink>("tailsink");
        output_port<mutable_buffer<std::int64_t>> feed6{"feed"};
        auto* in6 = p6->get_port<input_port_base>("in");
        if (in6 == nullptr || !feed6.connect(in6) || !p6->connect("out", sink6, "in")) {
            std::puts("FAIL: connect (I)");
            return 1;
        }
        p6->start();
        sink6->start();
        constexpr int K6 = 5;
        for (int s = 0; s < K6; ++s) {
            auto b = make_mutable<std::int64_t>(1);
            b.as_span()[0] = s;
            feed6.send_data(std::move(b), timestamp{});
        }
        feed6.send_eos();
        if (!sink6->wait_until_finished(std::chrono::seconds(5))) {
            std::puts("FAIL: tail sink never finished (I)");
            return 1;
        }
        if (!p6->wait_until_finished(std::chrono::seconds(5))) {
            std::puts("FAIL: tail pipe never finished (I)");
            return 1;
        }
        if (p6->m_eos_calls.load() != 1) {
            std::printf("FAIL: on_end_of_stream() called %d times (want 1) (I)\n", p6->m_eos_calls.load());
            return 1;
        }
        if (p6->finished_reason() != finish_reason::completed) {
            std::puts("FAIL: tail pipe finish_reason != completed (I)");
            return 1;
        }
        if (sink6->m_received.load() != K6 + 1) {
            std::printf("FAIL: sink received %d packets (want %d live + 1 tail) (I)\n", sink6->m_received.load(),
                        K6 + 1);
            return 1;
        }
        if (sink6->m_tail_call.load() < 0 || sink6->m_eos_call.load() < 0 ||
            sink6->m_tail_call.load() >= sink6->m_eos_call.load()) {
            std::printf("FAIL: tail packet (call %d) did not precede EOS observation (call %d) (I)\n",
                        sink6->m_tail_call.load(), sink6->m_eos_call.load());
            return 1;
        }
        std::puts("PIPELINE on_end_of_stream OK: fired once, tail packet arrived before EOS");
    }
    return 0;
}
