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
            // Time each backpressure stall. This is the measurement that actually discriminates:
            // a reverse-doorbell wake is sub-millisecond, while a MISSED edge falls back to the
            // 4s NOOP backoff. A wall-clock bound on the whole run cannot tell those apart —
            // 60s is generous enough that a doorbell firing on only SOME full->not-full edges
            // still finishes inside it — but the longest single stall separates them by three
            // orders of magnitude regardless of how loaded the machine is.
            if (!m_await_started) {
                m_await_since = std::chrono::steady_clock::now();
                m_await_started = true;
            }
            return retval::AWAIT_OUTPUT;
        } // backpressured -> reverse-doorbell idle
        // Measured HERE, on the send that follows a wait — not on the next backpressured call.
        // A missed edge stalls until the NOOP backoff fires, by which point the consumer has
        // drained and can_send() is true again, so the stall is only ever visible from this side.
        if (m_await_started) {
            m_await_started = false;
            const auto waited =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_await_since)
                    .count();
            auto prev = max_stall_ms.load(std::memory_order_relaxed);
            while (waited > prev && !max_stall_ms.compare_exchange_weak(prev, waited, std::memory_order_relaxed)) {
            }
            if (waited >= 2000) {
                missed_edges.fetch_add(1, std::memory_order_relaxed);
            }
        }
        out.send_data(make_immutable<float>({1.0F}), timestamp{});
        produced.fetch_add(1, std::memory_order_relaxed);
        return retval::NORMAL;
    }
    std::atomic<long> max_stall_ms{0};
    std::atomic<long> missed_edges{0}; // stalls that fell through to the NOOP backoff

private:
    std::chrono::steady_clock::time_point m_await_since{};
    bool m_await_started{false};

public:
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
    // The claim here is a RATIO, not a wall-clock constant. With the doorbell this is a few tens
    // of milliseconds of real work; without it the source stalls the full NOOP backoff (4s) on
    // each of the ~N/depth fills, so draining N would take on the order of an hour. Any bound
    // between those discriminates perfectly, so pick one that is generous against machine load
    // and still orders of magnitude below the broken floor.
    //
    // The previous bound was 5s, which was tight against the LOADED fast path rather than the
    // broken one: under contention (CI runs sanitizer jobs alongside) a healthy run drained only
    // ~2000/4000 within it, failing ~3 runs in 15. The mechanism was never in doubt in those
    // runs — only the machine's speed was being measured.
    constexpr long k_no_doorbell_floor_ms = (N / 4) * 4000L; // ~1000 fills x 4s backoff
    constexpr long k_budget_ms = 60'000;                     // ~65x under the floor, ~1000x over a healthy run
    static_assert(k_budget_ms * 50 < k_no_doorbell_floor_ms, "budget must stay far below the broken-path floor");

    const bool got_all = wait_until([&] { return sink->consumed.load() >= N; }, std::chrono::milliseconds(k_budget_ms));
    const auto dt_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();

    check(got_all, "consumer drained all N (reverse doorbell kept the backpressured source flowing)");
    // `consumed == N` does NOT imply the producer has finished bookkeeping. The source increments
    // `produced` AFTER send_data returns, so the sink can consume packet N and satisfy the wait
    // above while the source has not yet run its fetch_add for that packet — leaving produced at
    // N-1 for a few instructions. Reading it immediately made this assertion a race that happened
    // to win on one machine and lose on another (it failed first on Rocky/GCC 14, having passed
    // locally). Wait for the counter the assertion is about, then assert on it.
    const bool produced_all =
        wait_until([&] { return src->produced.load() >= N; }, std::chrono::milliseconds(k_budget_ms));
    check(produced_all && src->produced.load() == N, "source produced exactly N (lossless can_send pacing)");
    // COUNT the stalls that fell through to the NOOP backoff, and allow a few.
    //
    // Demanding zero was wrong: the reverse doorbell is best-effort BY DESIGN (see the comment at
    // input_port.hpp's signal_data() call — a burst that a lagging consumer later observes as empty
    // through a coherence-stale m_tail can miss the fast wake and fall back to m_delay, which is
    // the documented liveness backstop). A zero-miss assertion therefore fails on correct code
    // roughly one run in eight. It was validated only against its true positive — reintroducing a
    // missed edge made it fail — and never against its false-positive rate on a healthy tree.
    //
    // With ~1000 backpressure fills, a handful of misses is the design working as documented while
    // a systemic regression misses essentially all of them (~1 hour of stalls). This bound still
    // separates those by two orders of magnitude, which is what the whole-run budget alone cannot.
    constexpr long k_max_missed_edges = 5;
    check(src->missed_edges.load() <= k_max_missed_edges,
          "backpressure stalls reaching the NOOP backoff stayed rare (doorbell is best-effort, not absent)");
    std::printf("reverse doorbell: drained %ld/%ld in %lld ms (produced=%ld, longest stall=%ld ms, "
                "missed edges=%ld/%ld)\n",
                sink->consumed.load(), N, (long long)dt_ms, src->produced.load(), src->max_stall_ms.load(),
                src->missed_edges.load(), k_max_missed_edges);

    src->stop();
    sink->stop();

    if (g_fails == 0) {
        std::puts("REVERSE DOORBELL OK");
    }
    return g_fails == 0 ? 0 : 1;
}
