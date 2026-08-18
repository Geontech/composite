// Regression: reconcile_enabled_locked() calls the private start_locked()/
// stop_locked(), NOT the virtual start()/stop(). So a pipeline_component started the PRODUCTION way
// (application::start() / a RUNTIME enabled write) must still spin up its worker pool. Before the
// fix, the pool never started via the app and fft/psd produced nothing forever. Every prior test
// masked it by calling ->start() directly on the concrete object. This test drives a
// pipeline_component through a real application (start / restart / RUNTIME disable+enable).
#include "composite/buffers/buffer.hpp"
#include "composite/core/application.hpp"
#include "composite/core/component.hpp"
#include "composite/core/pipeline_component.hpp"
#include "composite/ports/input_port.hpp"
#include "composite/ports/output_port.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>

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

// A pipeline_component whose pool doubles each element. If the pool never starts (the bug), work()
// never runs and downstream sees nothing.
class doubler : public pipeline_component<ibuf, ibuf> {
public:
    explicit doubler(std::string_view id) : pipeline_component(id, "in", "out", /*workers=*/2) {}
    auto work(ibuf in, timestamp /*ts*/, const metadata& /*md*/) -> ibuf override {
        m_work_calls.fetch_add(1, std::memory_order_relaxed); // includes zero-length packets (#5)
        auto out = make_mutable<int>(in.size());
        for (std::size_t i = 0; i < in.size(); ++i) {
            out[i] = in[i] * 2;
        }
        return std::move(out).to_immutable();
    }
    // Count pool teardowns so a test can prove the pool is reaped on SELF-finish on self-finish, not only on
    // an explicit stop(). Delegates to the real teardown.
    auto on_worker_stop() -> void override {
        pipeline_component::on_worker_stop();
        m_stops.fetch_add(1, std::memory_order_relaxed);
    }
    std::atomic<int> m_stops{0};
    std::atomic<int> m_work_calls{0};
};

// Identity pipeline whose finalize() DROPS every other packet (submission order). Exercises the
// finalize()-drop path UNDER downstream backpressure: a dropped head must retire
// immediately without waiting for output room it never needs, so drops never stall the pipeline.
class drop_alt_pipeline : public pipeline_component<ibuf, ibuf> {
public:
    explicit drop_alt_pipeline(std::string_view id) : pipeline_component(id, "in", "out", 2) {}
    auto work(ibuf in, timestamp /*ts*/, const metadata& /*md*/) -> ibuf override {
        auto out = make_mutable<int>(in.size());
        for (std::size_t i = 0; i < in.size(); ++i) {
            out[i] = in[i];
        }
        return std::move(out).to_immutable();
    }
    auto finalize(ibuf& /*out*/, timestamp /*ts*/, const metadata& /*md*/) -> bool override {
        return (m_fin++ % 2) == 0; // keep 0th,2nd,4th... drop the rest (finalize runs main-thread, in order)
    }
    int m_fin{0}; // main-worker only
};

// finalize() THROWS on one packet. With error_restart_max>0, a non-latched throwing finalize() would
// be replayed on restart; the round-5 latch (set even on throw) + local catch means finalize() runs
// exactly once per packet and the throwing packet is dropped (not an error exit).
class throw_finalize_pipeline : public pipeline_component<ibuf, ibuf> {
public:
    explicit throw_finalize_pipeline(std::string_view id) : pipeline_component(id, "in", "out", 1) {}
    auto work(ibuf in, timestamp /*ts*/, const metadata& /*md*/) -> ibuf override {
        auto out = make_mutable<int>(in.size());
        for (std::size_t i = 0; i < in.size(); ++i) {
            out[i] = in[i];
        }
        return std::move(out).to_immutable();
    }
    auto finalize(ibuf& /*out*/, timestamp /*ts*/, const metadata& /*md*/) -> bool override {
        const int n = m_fin_calls.fetch_add(1, std::memory_order_relaxed); // side effect: counts EVERY call
        if (n == 2) {
            throw std::runtime_error("finalize boom");
        } // throw once, on the 3rd packet
        return true;
    }
    std::atomic<int> m_fin_calls{0};
};

// EOS-aware sink that consumes SLOWLY, to force downstream backpressure on the pipeline's output.
class slow_summing_sink : public component {
public:
    explicit slow_summing_sink(std::string_view id) : component(id) { add_port(&m_in); }
    auto process() -> retval override {
        auto [buf, ts, md] = m_in.get_data();
        if (buf.empty()) {
            return inputs_at_end() ? retval::FINISH : retval::NOOP;
        }
        std::this_thread::sleep_for(1ms); // slower than the pipeline produces
        for (std::size_t i = 0; i < buf.size(); ++i) {
            m_sum.fetch_add(buf[i], std::memory_order_relaxed);
        }
        m_packets.fetch_add(1, std::memory_order_relaxed);
        return retval::NORMAL;
    }
    input_port<ibuf> m_in{"in"};
    std::atomic<int> m_sum{0};
    std::atomic<int> m_packets{0};
    component::auto_stop m_auto_stop{*this};
};

// Source: emits `total` single-element buffers (value = index), then idles.
class counter_src : public component {
public:
    counter_src(std::string_view id, int total) : component(id), m_total(total) { add_port(&m_out); }
    auto process() -> retval override {
        const int n = m_sent.load(std::memory_order_relaxed);
        if (n >= m_total) {
            return retval::NOOP;
        }
        auto b = make_mutable<int>(1);
        b[0] = n;
        m_out.send_data(std::move(b).to_immutable(), timestamp{}, std::nullopt);
        m_sent.store(n + 1, std::memory_order_relaxed);
        return retval::NORMAL;
    }
    output_port<ibuf> m_out{"out"};
    int m_total;
    std::atomic<int> m_sent{0}; // read by the worker + reset by the test main thread between runs
    component::auto_stop m_auto_stop{*this};
};

// Sink: sums the values it receives and counts packets. EOS-aware — FINISHes once upstream closed
// and drained, so it can participate in whole-graph self-completion.
class summing_sink : public component {
public:
    explicit summing_sink(std::string_view id) : component(id) { add_port(&m_in); }
    auto process() -> retval override {
        auto [buf, ts, md] = m_in.get_data();
        if (buf.empty()) {
            return inputs_at_end() ? retval::FINISH : retval::NOOP;
        }
        for (std::size_t i = 0; i < buf.size(); ++i) {
            m_sum.fetch_add(buf[i], std::memory_order_relaxed);
        }
        m_packets.fetch_add(1, std::memory_order_relaxed);
        return retval::NORMAL;
    }
    input_port<ibuf> m_in{"in"};
    std::atomic<int> m_sum{0};
    std::atomic<int> m_packets{0};
    component::auto_stop m_auto_stop{*this};
};

// Source that emits `total` elements then FINISHes (base auto-sends EOS), so the whole chain can
// self-complete through the pipeline_component.
class finishing_src : public component {
public:
    finishing_src(std::string_view id, int total) : component(id), m_total(total) { add_port(&m_out); }
    auto process() -> retval override {
        const int n = m_sent.load(std::memory_order_relaxed);
        if (n >= m_total) {
            return retval::FINISH;
        } // -> completed -> auto-EOS on m_out
        auto b = make_mutable<int>(1);
        b[0] = n;
        m_out.send_data(std::move(b).to_immutable(), timestamp{}, std::nullopt);
        m_sent.store(n + 1, std::memory_order_relaxed);
        return retval::NORMAL;
    }
    output_port<ibuf> m_out{"out"};
    int m_total;
    std::atomic<int> m_sent{0};
    component::auto_stop m_auto_stop{*this};
};

// Emits `data_count` size-1 packets, then ONE genuine zero-length packet, then FINISHes. Used to
// prove the pipeline submits a real zero-length packet through work() rather than dropping it (#5).
class zero_len_src : public component {
public:
    zero_len_src(std::string_view id, int data_count) : component(id), m_data(data_count) { add_port(&m_out); }
    auto process() -> retval override {
        const int n = m_sent.load(std::memory_order_relaxed);
        if (n < m_data) {
            auto b = make_mutable<int>(1);
            b[0] = n;
            m_out.send_data(std::move(b).to_immutable(), timestamp{}, std::nullopt);
        } else if (n == m_data) {
            m_out.send_data(make_immutable<int>(0), timestamp{}, std::nullopt); // legit zero-length packet
        } else {
            return retval::FINISH;
        }
        m_sent.store(n + 1, std::memory_order_relaxed);
        return retval::NORMAL;
    }
    output_port<ibuf> m_out{"out"};
    int m_data;
    std::atomic<int> m_sent{0};
    component::auto_stop m_auto_stop{*this};
};

// Poll until sink has `n` packets or the deadline passes.
auto wait_for_packets(const std::shared_ptr<summing_sink>& k, int n, std::chrono::milliseconds timeout) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (k->m_packets.load(std::memory_order_relaxed) < n && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(2ms);
    }
    return k->m_packets.load(std::memory_order_relaxed) >= n;
}
} // namespace

int main() {
    constexpr int N = 20;
    // sum of doubled indices 0..N-1 = 2 * (N-1)N/2 = N*(N-1)
    constexpr int EXPECTED_SUM = N * (N - 1);

    application app{"pipe"};
    auto s = std::make_shared<counter_src>("s", N);
    auto d = std::make_shared<doubler>("d");
    auto k = std::make_shared<summing_sink>("k");
    app.add_component(s);
    app.add_component(d);
    app.add_component(k);
    check(s->connect("out", d, "in"), "connect s->d");
    check(d->connect("out", k, "in"), "connect d->k");

    // ---- (1) started via the APPLICATION: the pool MUST run (this is the regression) ----
    app.start();
    check(wait_for_packets(k, N, 3s), "app.start(): sink received all packets (pool ran via reconcile)");
    check(k->m_sum.load() == EXPECTED_SUM, "app.start(): values were doubled by the pool");
    app.stop();

    // ---- (2) restart via the application: the pool comes back up ----
    s->m_sent = 0; // let the source re-emit
    k->m_sum.store(0);
    k->m_packets.store(0);
    app.start();
    check(wait_for_packets(k, N, 3s), "app restart: sink received all packets again");
    check(k->m_sum.load() == EXPECTED_SUM, "app restart: values doubled again");

    // ---- (3) RUNTIME disable then enable (reconcile stop_locked/start_locked -> pool stop/start) ----
    //
    // QUIESCE THE SOURCE across the whole disable window. Disabling `d` calls
    // pause_input_ports(), which sets its inputs to depth 0 — and a depth-0 input DISCARDS on
    // send, by design (see port_base.hpp: "A paused input ... discards on send by design").
    // `s` is a separate component and keeps running, so resetting s->m_sent while `d` is down
    // makes `s` re-emit the entire run into a paused, drop-all port. Whether any of it survived
    // depended on how far `s` got before resume_input_ports() — which made this check fail
    // ~1-3 runs in 10, and predates the containment/ports work (verified by stressing
    // the same case on an unmodified checkout).
    // The subject of this case is the POOL restart, not the drop-on-pause behaviour, so remove
    // the race rather than widening the timeout: hold the source down, reconfigure, bring the
    // pipeline back, and only then let the source produce again.
    s->set_properties(properties::json{{"enabled", false}}, properties::config_type::RUNTIME);
    d->set_properties(properties::json{{"enabled", false}}, properties::config_type::RUNTIME);
    check(!d->is_running(), "runtime disable: pipeline not running");
    check(!s->is_running(), "runtime disable: source quiesced so nothing is produced into a paused input");
    s->m_sent = 0;
    k->m_sum.store(0);
    k->m_packets.store(0);
    d->set_properties(properties::json{{"enabled", true}}, properties::config_type::RUNTIME);
    s->set_properties(properties::json{{"enabled", true}}, properties::config_type::RUNTIME);
    check(wait_for_packets(k, N, 3s), "runtime re-enable: pool restarted and processed");
    check(k->m_sum.load() == EXPECTED_SUM, "runtime re-enable: values doubled");
    app.stop();

    // ---- (4) SELF-COMPLETION through a pipeline_component, and pool reap on self-finish ----
    {
        application app2{"pipe2"};
        auto fs = std::make_shared<finishing_src>("fs", N);
        auto d2 = std::make_shared<doubler>("d2");
        auto k2 = std::make_shared<summing_sink>("k2");
        app2.add_component(fs);
        app2.add_component(d2);
        app2.add_component(k2);
        check(fs->connect("out", d2, "in"), "self-complete: connect fs->d2");
        check(d2->connect("out", k2, "in"), "self-complete: connect d2->k2");
        app2.start();
        // The WHOLE graph must self-finish: source FINISH -> EOS -> doubler drains + inputs_at_end ->
        // FINISH -> EOS -> sink at_end -> FINISH. Before self-completion the doubler never returned FINISH, so
        // this bounded wait timed out (the pipeline stage hung forever).
        check(app2.wait_until_finished(30s), "self-completion: whole pipeline self-finished (bounded wait true)");
        check(d2->is_finished(), "self-completion: pipeline_component reports finished");
        check(d2->finished_reason() == finish_reason::completed, "self-completion: pipeline finished == completed");
        check(k2->m_packets.load() == N, "self-completion: sink received every packet (nothing dropped)");
        check(k2->m_sum.load() == EXPECTED_SUM, "self-completion: values were doubled end to end");
        // The pool was reaped ON SELF-FINISH, before any explicit stop().
        check(d2->m_stops.load() >= 1,
              "self-finish reap: pool reaped (on_worker_stop) on self-finish, no explicit stop");
        const int stops_at_finish = d2->m_stops.load();
        app2.stop(); // idempotent: guard must prevent a SECOND pool teardown
        check(d2->m_stops.load() == stops_at_finish,
              "self-finish reap: explicit stop after self-finish does NOT double-reap");
    }

    // ---- (5) NO DROP under downstream backpressure ----
    {
        application app3{"pipe3"};
        auto fs = std::make_shared<finishing_src>("fs", N);
        auto d3 = std::make_shared<doubler>("d3");
        auto k3 = std::make_shared<slow_summing_sink>("k3");
        app3.add_component(fs);
        app3.add_component(d3);
        app3.add_component(k3);
        k3->m_in.depth(8); // tiny ring downstream of the pipeline: a non-pacing retire would drop
        check(fs->connect("out", d3, "in"), "backpressure: connect fs->d3");
        check(d3->connect("out", k3, "in"), "backpressure: connect d3->k3");
        app3.start();
        check(app3.wait_until_finished(30s), "backpressure: pipeline graph finished");
        // The pipeline paces its OUTPUT (retire_ready -> AWAIT_OUTPUT on a full ring), so EVERY doubled
        // packet reaches the slow sink. Before the fix, retire_ready() sent unconditionally, dropped on
        // the full ring, and the self-completion FINISH still reported completed having swallowed
        // most of the stream.
        check(k3->m_packets.load() == N,
              "backpressure: slow sink received ALL packets — pipeline paced, nothing dropped");
        check(k3->m_sum.load() == EXPECTED_SUM, "backpressure: all values doubled + delivered under backpressure");
        check(d3->finished_reason() == finish_reason::completed,
              "backpressure: pipeline completed (only after output flushed)");
    }

    // ---- (6) a genuine zero-length packet is submitted to work(), not dropped at ingest ----
    {
        application app4{"pipe4"};
        constexpr int DATA = 5;
        auto zs = std::make_shared<zero_len_src>("zs", DATA);
        auto d4 = std::make_shared<doubler>("d4");
        auto k4 = std::make_shared<summing_sink>("k4");
        app4.add_component(zs);
        app4.add_component(d4);
        app4.add_component(k4);
        check(zs->connect("out", d4, "in"), "zero-len: connect zs->d4");
        check(d4->connect("out", k4, "in"), "zero-len: connect d4->k4");
        app4.start();
        check(app4.wait_until_finished(30s), "zero-len: graph finished");
        // work() must have run for all DATA packets PLUS the zero-length one — try_get() distinguishes
        // a real size-0 packet from an empty ring, so it is submitted rather than dropped at ingest.
        check(d4->m_work_calls.load() == DATA + 1,
              "zero-length packet: zero-length packet submitted to work(), not dropped");
    }

    // ---- (7) finalize()-DROP path under backpressure does not stall ----
    {
        application app5{"pipe5"};
        auto fs = std::make_shared<finishing_src>("fs", N);
        auto dp = std::make_shared<drop_alt_pipeline>("dp");
        auto k5 = std::make_shared<slow_summing_sink>("k5");
        app5.add_component(fs);
        app5.add_component(dp);
        app5.add_component(k5);
        k5->m_in.depth(8); // slow + tiny ring: kept packets fill it; the DROPPED heads must not block
        check(fs->connect("out", dp, "in"), "drop-bp: connect fs->dp");
        check(dp->connect("out", k5, "in"), "drop-bp: connect dp->k5");
        app5.start();
        // A dropped head needs no output slot; if it were gated on output room (the round-3 bug) it
        // would stall behind the full sink. It must retire immediately so the pipeline completes.
        check(app5.wait_until_finished(30s), "finalize-drop: finalize-drop pipeline finished (drops didn't stall)");
        check(dp->finished_reason() == finish_reason::completed, "finalize-drop: drop pipeline completed");
        // Kept = the even-indexed of N submissions = ceil(N/2); dropped packets never reach the sink.
        check(k5->m_packets.load() == (N + 1) / 2, "finalize-drop: sink got exactly the KEPT packets, no more/less");
    }

    // ---- (8) a paused (depth-0) downstream does NOT hang the pipeline ----
    {
        application app6{"pipe6"};
        auto fs = std::make_shared<finishing_src>("fs", N);
        auto d6 = std::make_shared<doubler>("d6");
        auto k6 = std::make_shared<summing_sink>("k6");
        app6.add_component(fs);
        app6.add_component(d6);
        app6.add_component(k6);
        k6->m_in.depth(0); // paused/disabled sink: discards on send, must NOT backpressure the pipeline
        check(fs->connect("out", d6, "in"), "paused-dn: connect fs->d6");
        check(d6->connect("out", k6, "in"), "paused-dn: connect d6->k6");
        app6.start();
        // producer_can_send() treats a depth-0 port as sendable (drops), so the pipeline sends-and-drops
        // to the paused sink, drains its input, and self-finishes. Before the fix it blocked forever.
        check(d6->wait_until_finished(30s), "paused sink: pipeline with a paused (depth-0) sink still self-finishes");
        check(d6->finished_reason() == finish_reason::completed,
              "paused sink: pipeline completed (paused sink didn't wedge it)");
    }

    // ---- (9) a throwing finalize() is caught+dropped, NOT replayed under error-restart ----
    {
        application app7{"pipe7"};
        constexpr int M = 8;
        auto fs = std::make_shared<finishing_src>("fs", M);
        auto tp = std::make_shared<throw_finalize_pipeline>("tp");
        auto k7 = std::make_shared<summing_sink>("k7");
        // Opt into restart — the exact config under which a non-latched finalize-throw would replay.
        tp->set_properties(properties::json{{"error_restart_max", 3}, {"error_restart_backoff_ms", 1}},
                           properties::config_type::INITIALIZE);
        app7.add_component(fs);
        app7.add_component(tp);
        app7.add_component(k7);
        check(fs->connect("out", tp, "in"), "finalize-throw: connect fs->tp");
        check(tp->connect("out", k7, "in"), "finalize-throw: connect tp->k7");
        app7.start();
        check(app7.wait_until_finished(30s), "finalize-throw: finalize-throw pipeline finished");
        check(tp->finished_reason() == finish_reason::completed,
              "finalize-throw: completed (finalize-throw caught+dropped, not an error exit)");
        check(tp->m_fin_calls.load() == M,
              "finalize-throw: finalize() ran EXACTLY once per packet (throwing packet not replayed)");
        check(k7->m_packets.load() == M - 1,
              "finalize-throw: the throwing packet was dropped; the other M-1 delivered");
    }

    if (g_failures) {
        std::printf("\n%d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::puts("PIPELINE APP LIFECYCLE OK: pool starts via application::start() and RUNTIME enable "
              "(P0 fix: reconcile drives the virtual start/stop hooks), across restart + disable/enable");
    return 0;
}
