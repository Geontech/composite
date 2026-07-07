// Lifecycle — source_component archetype + application::drain_stop. A source that produces a
// finite range and returns done() drives the whole graph to completion via auto-EOS (produce done()
// -> FINISH/completed -> EOS on its output -> the sink reaches at_end() and finishes). And a source
// that produces forever is shut down gracefully by drain_stop (stop sources + EOS -> drain -> finish).
// Ties together all four lifecycle sub-features: completion, EOS, resilience-adjacent drain, source-mode.
#include "composite/buffers/buffer.hpp"
#include "composite/core/application.hpp"
#include "composite/core/component.hpp"
#include "composite/core/source_component.hpp"
#include "composite/ports/input_port.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>

using namespace composite;
using namespace std::chrono_literals;
using ibuf = immutable_buffer<int>;

namespace {
int g_failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

// Emits `total` single-element packets (value = index), then done() -> EOS.
class range_source : public source_component<ibuf> {
public:
    range_source(std::string_view id, int total) : source_component(id), m_total(total) {}

protected:
    auto produce() -> produce_result override {
        if (m_produced >= m_total) {
            return produce_result::done();
        }
        auto b = make_mutable<int>(1);
        b[0] = m_produced++;
        return produce_result::emit(std::move(b).to_immutable(), timestamp{});
    }

private:
    int m_total;
    int m_produced{0}; // worker-thread only
    component::auto_stop m_auto_stop{*this};
};

// Emits forever (until stopped). Used to exercise drain_stop.
class forever_source : public source_component<ibuf> {
public:
    explicit forever_source(std::string_view id) : source_component(id) {}

protected:
    auto produce() -> produce_result override {
        auto b = make_mutable<int>(1);
        b[0] = 1;
        return produce_result::emit(std::move(b).to_immutable(), timestamp{});
    }

private:
    component::auto_stop m_auto_stop{*this};
};

// Counts received elements; finishes on upstream EOS.
class counting_sink : public component {
public:
    explicit counting_sink(std::string_view id) : component(id) { add_port(&m_in); }
    auto process() -> retval override {
        auto [buf, ts, md] = m_in.get_data();
        if (buf.empty()) {
            return inputs_at_end() ? retval::FINISH : retval::NOOP;
        }
        m_count.fetch_add(static_cast<int>(buf.size()), std::memory_order_relaxed);
        return retval::NORMAL;
    }
    input_port<ibuf> m_in{"in"};
    std::atomic<int> m_count{0};
    component::auto_stop m_auto_stop{*this};
};

// Consumes SLOWLY (models a downstream that can't keep up), so a fast source overruns a small ring.
class slow_sink : public component {
public:
    explicit slow_sink(std::string_view id) : component(id) { add_port(&m_in); }
    auto process() -> retval override {
        auto [buf, ts, md] = m_in.get_data();
        if (buf.empty()) {
            return inputs_at_end() ? retval::FINISH : retval::NOOP;
        }
        std::this_thread::sleep_for(1ms); // deliberately slower than the source produces
        m_count.fetch_add(static_cast<int>(buf.size()), std::memory_order_relaxed);
        return retval::NORMAL;
    }
    input_port<ibuf> m_in{"in"};
    std::atomic<int> m_count{0};
    component::auto_stop m_auto_stop{*this};
};

// The DOCUMENTED-correct pattern: a source that stops its worker in the LEAF via its own auto_stop
// (last member), so it is safe to DESTROY while running. source_component deliberately provides no
// base auto_stop — it cannot, since produce() is pure, so a base-member auto_stop would run only
// after the leaf vtable degrades (the worker would then hit the now-pure produce()). The stop MUST
// live here in the leaf. See source_component's "Destruction" note.
class dtor_safe_source : public source_component<ibuf> {
public:
    explicit dtor_safe_source(std::string_view id) : source_component(id) {}

protected:
    auto produce() -> produce_result override {
        auto b = make_mutable<int>(1);
        b[0] = 7;
        return produce_result::emit(std::move(b).to_immutable(), timestamp{});
    }

private:
    component::auto_stop m_auto_stop{*this}; // MUST be last — stops the worker while THIS vtable is live
};
} // namespace

int main() {
    // ---- a finite source drives the graph to completion via auto-EOS ----
    {
        application app{"finite"};
        auto s = std::make_shared<range_source>("s", 15);
        auto k = std::make_shared<counting_sink>("k");
        app.add_component(s);
        app.add_component(k);
        check(s->connect("out", k, "in"), "finite: connect s->k");
        app.start();
        check(app.wait_until_finished(3s), "finite: all finished");
        check(s->finished_reason() == finish_reason::completed, "finite: source completed (produce done())");
        check(k->finished_reason() == finish_reason::completed, "finite: sink completed (via EOS)");
        check(k->m_count.load() == 15, "finite: sink received all 15 (EOS after data)");
    }

    // ---- a forever source is shut down gracefully by drain_stop ----
    {
        application app{"drain"};
        auto s = std::make_shared<forever_source>("s");
        auto k = std::make_shared<counting_sink>("k");
        app.add_component(s);
        app.add_component(k);
        s->connect("out", k, "in");
        app.start();
        // Let it run so data is flowing.
        for (int i = 0; i < 200 && k->m_count.load() == 0; ++i) {
            std::this_thread::sleep_for(1ms);
        }
        check(s->is_running(), "drain: source running before drain_stop");
        check(k->m_count.load() > 0, "drain: data was flowing");

        app.drain_stop(2s);
        check(!s->is_running(), "drain: source stopped");
        check(!k->is_running(), "drain: sink stopped");
        // The sink saw EOS (source stopped + send_eos) and drained to completion within the timeout.
        check(k->finished_reason() == finish_reason::completed, "drain: sink completed via EOS drain");
    }

    // ---- §2.4: a source PACES against a full downstream ring (AWAIT_OUTPUT) instead of dropping ----
    {
        application app{"backpressure"};
        constexpr int N = 40;
        auto s = std::make_shared<range_source>("s", N);
        auto k = std::make_shared<slow_sink>("k");
        app.add_component(s);
        app.add_component(k);
        k->m_in.depth(8); // tiny ring: a non-pacing source (send+NORMAL) would overrun it and drop
        check(s->connect("out", k, "in"), "backpressure: connect s->k");
        app.start();
        check(app.wait_until_finished(10s), "backpressure: graph finished");
        // With AWAIT_OUTPUT pacing the source never overruns the ring, so EVERY item is delivered.
        // Before the fix (produce() then unconditional send_data + NORMAL) the fast source dropped
        // into the full ring and the slow sink saw fewer than N.
        check(k->m_count.load() == N, "§2.4: sink received ALL items — source paced, nothing dropped");
        check(s->finished_reason() == finish_reason::completed, "backpressure: source completed");
    }

    // ---- §2.3: a correctly-written source (leaf auto_stop) is safe to DESTROY while running ----
    {
        auto s = std::make_shared<dtor_safe_source>("dtor"); // unconnected -> produces (drops) + runs
        s->start();
        for (int i = 0; i < 200 && !s->is_running(); ++i) {
            std::this_thread::sleep_for(1ms);
        }
        check(s->is_running(), "§2.3: source running before destruction");
        // Destroy while the worker is actively in process()/produce(). The leaf's own auto_stop (last
        // member) stops the worker while this vtable is still intact, so produce() is never called on
        // a torn-down vtable and m_out is never used after free. Exercised under CPU contention in CI.
        s.reset();
        check(true, "§2.3: destroyed a running source (leaf auto_stop) — no pure-virtual-call / no UAF");
    }

    // ---- §2.4 (round 3): done()/EOS is reachable even when the output is UN-SENDABLE ----
    {
        application app{"done-under-backpressure"};
        auto s = std::make_shared<range_source>("s", 0); // produces NO data — first produce() is done()
        auto k = std::make_shared<counting_sink>("k");
        app.add_component(s);
        app.add_component(k);
        check(s->connect("out", k, "in"), "done-bp: connect s->k");
        k->m_in.depth(0); // consumer permanently un-sendable (models a paused/disabled downstream)
        app.start();
        // With the round-2 gate-BEFORE-produce(), can_send()==false -> AWAIT_OUTPUT forever, so
        // produce() (hence done()) is never called and the source hangs. With produce-then-hold, only
        // the `data` case is backpressure-gated: done() is reached and FINISH fires regardless of
        // output room (EOS is out-of-band). A finite source must never be starved of completion.
        check(s->wait_until_finished(3s),
              "§2.4-r3: source with un-sendable output still self-finishes (done not gated)");
        check(s->finished_reason() == finish_reason::completed, "§2.4-r3: source completed via done()");
    }

    if (g_failures) {
        std::printf("\n%d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::puts("SOURCE COMPONENT OK: produce()/emit/idle/done, auto-EOS-to-completion on done(), "
              "and application::drain_stop graceful shutdown of a forever source");
    return 0;
}
