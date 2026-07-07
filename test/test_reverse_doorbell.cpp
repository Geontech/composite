// Reverse (backpressure) doorbell: a producer paced via can_send() returns AWAIT_OUTPUT
// when its downstream ring is full; the consumer's pop() then wakes it on the full->not-full
// edge (and its own arm + can_send re-check closes the pre-sleep race) — so the pipeline keeps
// flowing instead of the producer stalling out its NOOP backoff on every fill. We set a LARGE
// m_delay (4s) and a SMALL ring (depth 4): without the reverse doorbell the source would sleep
// ~4s on each of the ~N/4 fills, so draining N would take minutes; with it the whole N flows in
// milliseconds. Assert the consumer receives all N within a short window. Own main().
#include <composite/buffers/buffer.hpp>
#include <composite/core/component.hpp>

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

using namespace composite;
using composite::properties::config_type;
using json = composite::properties::json;

// Source that paces via can_send(): never overruns the downstream ring (lossless), and returns
// AWAIT_OUTPUT (idling on the reverse doorbell) while backpressured.
class pacing_source : public component {
public:
    output_port<immutable_buffer<float>> out{"out"};
    std::atomic<long> produced{0};
    long target{0};
    explicit pacing_source(std::string_view id) : component(id) { add_port(out); }
    auto process() -> retval override {
        if (produced.load(std::memory_order_relaxed) >= target) {
            return retval::NOOP;
        }
        if (!out.can_send()) {
            return retval::AWAIT_OUTPUT;
        } // backpressured -> reverse-doorbell idle
        out.send_data(make_immutable<float>({1.0F}), timestamp{});
        produced.fetch_add(1, std::memory_order_relaxed);
        return retval::NORMAL;
    }
    component::auto_stop m_auto_stop{*this};
};

class counting_sink : public component {
public:
    input_port<immutable_buffer<float>> in{"in", 4}; // small ring -> frequent backpressure
    std::atomic<long> consumed{0};
    explicit counting_sink(std::string_view id) : component(id) { add_port(in); }
    auto process() -> retval override {
        auto [buf, ts, md] = in.get_data();
        (void)ts;
        (void)md;
        if (buf.size() == 0) {
            return retval::NOOP;
        }
        consumed.fetch_add(1, std::memory_order_release);
        return retval::NORMAL;
    }
    component::auto_stop m_auto_stop{*this};
};

static int g_fails = 0;
static void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_fails;
    }
}
template <typename Pred>
static bool wait_until(Pred pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::yield();
    }
    return pred();
}

int main() {
    spdlog::set_level(spdlog::level::off);
    using namespace std::chrono_literals;

    constexpr long k_fallback_ns = 4'000'000'000L; // 4s NOOP backoff: a stall would dominate
    constexpr long N = 4000;                       // with depth 4, ~1000 backpressure fills

    auto src = std::make_shared<pacing_source>("src");
    auto sink = std::make_shared<counting_sink>("sink");
    src->target = N;
    src->set_properties(json{{"noop_thread_delay", k_fallback_ns}}, config_type::INITIALIZE);
    sink->set_properties(json{{"noop_thread_delay", k_fallback_ns}}, config_type::INITIALIZE);

    check(src->connect("out", sink, "in"), "connect src.out -> sink.in");

    const auto t0 = std::chrono::steady_clock::now();
    src->start();
    sink->start();

    // With the reverse doorbell, the consumer's drains keep waking the backpressured source, so
    // all N flow quickly. WITHOUT it the source would stall ~4s per fill (~1000 fills) and this
    // would time out far short of N.
    const bool got_all = wait_until([&] { return sink->consumed.load() >= N; }, 5s);
    const auto dt_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    check(got_all, "consumer drained all N (reverse doorbell kept the backpressured source flowing)");
    check(src->produced.load() == N, "source produced exactly N (lossless can_send pacing)");
    check(dt_ms < 5000, "completed well under the 4s-per-fill no-reverse-doorbell floor");
    std::printf("reverse doorbell: drained %ld/%ld in %lld ms (produced=%ld)\n", sink->consumed.load(), N,
                (long long)dt_ms, src->produced.load());

    src->stop();
    sink->stop();

    if (g_fails == 0) {
        std::puts("REVERSE DOORBELL OK");
    }
    return g_fails == 0 ? 0 : 1;
}
