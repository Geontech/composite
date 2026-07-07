// Lifecycle — EOS (end-of-stream) marker propagation. Verifies the out-of-band close mechanism:
//   - input_port::try_get() distinguishes an empty ring (nullopt) from a real zero-length packet;
//   - output_port::send_eos() sets producer_closed on every connected input (fan-out), and at_end()
//     is closed-AND-drained (data-before-EOS ordering by construction);
//   - release_producer() clears the closed flag (reconnect safety);
//   - a completed component AUTO-sends EOS on its outputs, so a 3-stage chain (source -> relay ->
//     sink) completes end to end: each stage returns FINISH on inputs_at_end() and propagates.
// Also covers EOS-by-default: the base auto-promotes a NOOP-at-end to FINISH, so a PLAIN
// consumer that never checks inputs_at_end() self-completes; on_end_of_stream() flushes held state
// before the close; and the finish_at_end=false opt-out keeps such a consumer running (no busy-spin).
#include "composite/buffers/buffer.hpp"
#include "composite/core/application.hpp"
#include "composite/core/component.hpp"
#include "composite/ports/input_port.hpp"
#include "composite/ports/output_port.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>

using namespace composite;
using namespace std::chrono_literals;
using ibuf = immutable_buffer<int>;

namespace {
int g_failures = 0;
void check(bool ok, const char* what) { if (!ok) { std::printf("FAIL: %s\n", what); ++g_failures; } }

// Source: emits `total` unit packets, then FINISHes (completed) -> framework auto-EOS's its output.
class src : public component {
public:
    src(std::string_view id, int total) : component(id), m_total(total) { add_port(&m_out); }
    auto process() -> retval override {
        const int n = m_sent.load(std::memory_order_relaxed);
        if (n >= m_total) { return retval::FINISH; }
        m_out.send_data(make_immutable<int>(1), timestamp{}, std::nullopt);
        m_sent.store(n + 1, std::memory_order_relaxed);
        return retval::NORMAL;
    }
    output_port<ibuf> m_out{"out"};
    int m_total;
    std::atomic<int> m_sent{0};  // reset cross-thread by the reopen regression test
    component::auto_stop m_auto_stop{*this};
};

// Relay: forwards each packet; FINISHes once its input is at end-of-stream and drained.
class relay : public component {
public:
    explicit relay(std::string_view id) : component(id) { add_port(&m_in); add_port(&m_out); }
    auto process() -> retval override {
        auto [buf, ts, md] = m_in.get_data();
        if (buf.empty()) { return inputs_at_end() ? retval::FINISH : retval::NOOP; }
        m_forwarded.fetch_add(1, std::memory_order_relaxed);
        m_out.send_data(std::move(buf), ts, md);
        return retval::NORMAL;
    }
    input_port<ibuf> m_in{"in"};
    output_port<ibuf> m_out{"out"};
    std::atomic<int> m_forwarded{0};
    component::auto_stop m_auto_stop{*this};
};

// Sink: counts packets; FINISHes on upstream EOS.
class sink : public component {
public:
    explicit sink(std::string_view id) : component(id) { add_port(&m_in); }
    auto process() -> retval override {
        auto [buf, ts, md] = m_in.get_data();
        if (buf.empty()) { return inputs_at_end() ? retval::FINISH : retval::NOOP; }
        m_received.fetch_add(1, std::memory_order_relaxed);
        return retval::NORMAL;
    }
    input_port<ibuf> m_in{"in"};
    std::atomic<int> m_received{0};
    component::auto_stop m_auto_stop{*this};
};

// A NON-EOS-aware consumer: NOOPs on empty and NEVER checks inputs_at_end() (like a naive
// pipeline_component). Used to prove EOS does not busy-spin such a consumer. m_calls counts process().
class noop_consumer : public component {
public:
    explicit noop_consumer(std::string_view id) : component(id) { add_port(&m_in); }
    auto process() -> retval override {
        m_calls.fetch_add(1, std::memory_order_relaxed);
        auto [buf, ts, md] = m_in.get_data();
        return buf.empty() ? retval::NOOP : retval::NORMAL;  // deliberately ignores EOS
    }
    input_port<ibuf> m_in{"in"};
    std::atomic<long> m_calls{0};
    component::auto_stop m_auto_stop{*this};
};

// EOS-by-default: a PLAIN consumer that forwards data and only returns NOOP on an empty read — it never checks
// inputs_at_end() itself. The base must auto-promote its NOOP-at-EOS to FINISH. It also holds a tail
// packet emitted ONLY from on_end_of_stream(), proving the flush hook runs (before EOS) and its
// output reaches downstream — the pattern real held-state components (exp_smooth/framer) use.
class delay_relay : public component {
public:
    explicit delay_relay(std::string_view id) : component(id) { add_port(&m_in); add_port(&m_out); }
    auto process() -> retval override {
        auto pkt = m_in.try_get();
        if (!pkt) { return retval::NOOP; }              // NO manual EOS handling — the base does it
        auto& [buf, ts, md] = *pkt;
        m_forwarded.fetch_add(1, std::memory_order_relaxed);
        m_out.send_data(std::move(buf), ts, md);
        return retval::NORMAL;
    }
    auto on_end_of_stream() -> void override {
        m_flush_called.store(true, std::memory_order_relaxed);
        m_out.send_data(make_immutable<int>(1), timestamp{}, std::nullopt);  // the flushed tail packet
    }
    input_port<ibuf> m_in{"in"};
    output_port<ibuf> m_out{"out"};
    std::atomic<int> m_forwarded{0};
    std::atomic<bool> m_flush_called{false};
    component::auto_stop m_auto_stop{*this};
};
} // namespace

int main() {
    // ---- try_get(): empty ring vs a real zero-length packet ----
    {
        output_port<ibuf> o("o");
        input_port<ibuf> i("i");
        check(o.connect(&i), "try_get: connect");
        check(!i.try_get().has_value(), "try_get on empty ring -> nullopt");
        o.send_data(make_immutable<int>(0), timestamp{});  // a legitimately zero-length packet
        auto g = i.try_get();
        check(g.has_value(), "try_get after send -> engaged (even for a zero-length packet)");
        check(g.has_value() && std::get<0>(*g).size() == 0, "try_get: the packet's buffer is size 0");
        check(!i.try_get().has_value(), "try_get: drained -> nullopt again");
    }

    // ---- at_end(): closed AND drained; ordering holds by construction ----
    {
        output_port<ibuf> o("o");
        input_port<ibuf> i("i");
        o.connect(&i);
        check(!i.producer_closed() && !i.at_end(), "at_end: open, not closed");
        o.send_data(make_immutable<int>(1), timestamp{});
        o.send_eos();
        check(i.producer_closed(), "at_end: producer_closed after send_eos");
        check(!i.at_end(), "at_end: NOT at_end while a packet is still pending (data before EOS)");
        (void)i.get_data();  // drain the packet
        check(i.at_end(), "at_end: closed AND drained -> at_end");
    }

    // ---- fan-out: send_eos closes every connected input ----
    {
        output_port<ibuf> o("o");
        input_port<ibuf> a("a"), b("b");
        o.connect(&a); o.connect(&b);
        o.send_eos();
        check(a.producer_closed() && b.producer_closed(), "fan-out: send_eos closes all inputs");
    }

    // ---- release_producer clears the closed flag (a reconnected input is not born at-end) ----
    {
        output_port<ibuf> o("o");
        input_port<ibuf> i("i");
        o.connect(&i); o.send_eos();
        check(i.producer_closed(), "reconnect: closed before disconnect");
        o.disconnect(&i);
        check(!i.producer_closed(), "reconnect: release_producer clears closed");
    }

    // ---- end-to-end propagation: source -> relay -> sink all complete via auto-EOS ----
    {
        application app{"chain"};
        auto s = std::make_shared<src>("s", 10);
        auto r = std::make_shared<relay>("r");
        auto k = std::make_shared<sink>("k");
        app.add_component(s);
        app.add_component(r);
        app.add_component(k);
        check(s->connect("out", r, "in"), "chain: connect s->r");
        check(r->connect("out", k, "in"), "chain: connect r->k");
        app.start();
        check(app.wait_until_finished(5s), "chain: all components finished");
        check(s->finished_reason() == finish_reason::completed, "chain: source completed");
        check(r->finished_reason() == finish_reason::completed, "chain: relay completed (via EOS)");
        check(k->finished_reason() == finish_reason::completed, "chain: sink completed (via EOS)");
        check(r->m_forwarded.load() == 10, "chain: relay forwarded all 10");
        check(k->m_received.load() == 10, "chain: sink received all 10 (no loss, EOS after data)");
    }

    // ---- EOS-by-default — a PLAIN NOOP consumer self-FINISHes at EOS; on_end_of_stream()
    //         flushes held state BEFORE the close, so downstream sees it. ----
    {
        application app{"eos-default"};
        auto s = std::make_shared<src>("s", 10);
        auto d = std::make_shared<delay_relay>("d");   // returns NOOP on empty; no manual EOS check
        auto k = std::make_shared<sink>("k");
        app.add_component(s);
        app.add_component(d);
        app.add_component(k);
        check(s->connect("out", d, "in"), "eos-default: connect s->d");
        check(d->connect("out", k, "in"), "eos-default: connect d->k");
        app.start();
        check(app.wait_until_finished(5s),
              "eos-default: whole chain finished (base auto-FINISHed the plain NOOP consumer)");
        check(d->finished_reason() == finish_reason::completed,
              "eos-default: relay completed via the synthesized FINISH (no manual inputs_at_end)");
        check(d->m_flush_called.load(), "eos-default: on_end_of_stream() ran before FINISH");
        check(d->m_forwarded.load() == 10, "eos-default: relay forwarded all 10 live packets");
        // 10 forwarded + 1 flushed tail, and the tail arrived BEFORE EOS (else the sink would miss it).
        check(k->m_received.load() == 11, "eos-default: sink got 10 live + 1 flushed tail before EOS");
    }

    // ---- finish_at_end opt-out + no busy-spin: finish_at_end=false keeps a NOOP consumer running past EOS,
    //      and a closed input must still idle at noop_thread_delay (not busy-spin). ----
    {
        application app{"optout"};
        auto s = std::make_shared<src>("s", 5);
        auto k = std::make_shared<noop_consumer>("k");
        app.add_component(s);
        app.add_component(k);
        k->set_properties(properties::json{{"finish_at_end", false}});  // opt out of EOS-by-default
        s->connect("out", k, "in");
        app.start();
        check(s->wait_until_finished(3s), "optout: source finished (auto-EOS sent)");
        std::this_thread::sleep_for(50ms);  // let the close settle; k keeps NOOPing (opted out)
        check(!k->is_finished(), "optout: finish_at_end=false consumer does NOT finish at EOS");
        const auto before = k->m_calls.load();
        std::this_thread::sleep_for(300ms);
        const auto delta = k->m_calls.load() - before;
        // Idling at noop_thread_delay (~1 ms) => a few hundred calls in 300 ms. A busy-spin would be
        // hundreds of thousands+. Bound well below a spin but well above the idle rate.
        check(delta < 5000, "optout: a closed input does NOT busy-spin the opt-out consumer (idles at m_delay)");
        check(!k->is_finished(), "optout: still running after the idle window");
        app.stop();
    }

    // ---- REGRESSION: a producer restart clears the downstream EOS latch (not sticky) ----
    {
        application app{"restart"};
        auto s = std::make_shared<src>("s", 3);
        auto k = std::make_shared<sink>("k");
        app.add_component(s);
        app.add_component(k);
        s->connect("out", k, "in");
        app.start();
        check(app.wait_until_finished(3s), "reopen: first run completed");
        check(k->m_in.at_end(), "reopen: sink input at_end after the source completed + EOS");
        // Restart the source: start_locked reopens its outputs, clearing the sink's stale EOS latch.
        s->m_sent = 0;               // let the source re-emit (worker not running; safe)
        k->m_received.store(0);
        s->start();
        check(!k->m_in.at_end(), "reopen: sink input NOT at_end after the source restarted");
        k->start();                  // restart the (finished) sink so it consumes again
        check(app.wait_until_finished(3s), "reopen: second run completed");
        check(k->m_received.load() == 3, "reopen: sink received the second run's data");
    }

    if (g_failures) { std::printf("\n%d FAILURE(S)\n", g_failures); return 1; }
    std::puts("EOS PROPAGATION OK: try_get disambiguation, at_end (closed+drained), fan-out close, "
              "reconnect reset, end-to-end source->relay->sink completion via auto-EOS, R1 "
              "EOS-by-default (plain NOOP consumer auto-FINISHes + on_end_of_stream flush), and the "
              "finish_at_end=false opt-out");
    return 0;
}
