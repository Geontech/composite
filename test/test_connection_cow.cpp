// Copy-on-write fan-out list: a producer iterates the lock-free connection
// snapshot in send_data() while another thread connect/disconnects a second
// input and a third reads introspection — all concurrently. Verifies the
// atomic<shared_ptr> COW is race-free (the in-flight snapshot keeps the old
// list alive) and the steadily-connected sink still receives data. Under TSan.
#include "composite/buffers/buffer.hpp"
#include "composite/ports/input_port.hpp"
#include "composite/ports/output_port.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>

using namespace composite;

int main() {
    output_port<mutable_buffer<std::uint64_t>> out{"o"};
    input_port<mutable_buffer<std::uint64_t>> a{"a", 256};   // steady sink (drained)
    input_port<mutable_buffer<std::uint64_t>> b{"b", 16};    // churned sink (connect/disconnect)
    if (!out.connect(&a)) { std::puts("FAIL: connect a"); return 1; }

    std::atomic<bool> stop{false}, bad{false};
    std::atomic<std::uint64_t> recv_a{0}, churns{0}, introspections{0};

    // Consumer for the steady sink.
    std::thread cons_a([&] {
        while (!stop.load(std::memory_order_acquire)) {
            auto [buf, ts, md] = a.get_data();
            if (buf.size() != 0) { recv_a.fetch_add(1, std::memory_order_relaxed); }
        }
        while (a.size() != 0) { (void)a.get_data(); }  // drain
    });
    // Churn: connect/disconnect b on the live output while the producer sends.
    std::thread churn([&] {
        while (!stop.load(std::memory_order_acquire)) {
            if (out.connect(&b)) {
                // keep b drained so it doesn't merely fill+drop forever
                for (int i = 0; i < 8; ++i) { (void)b.get_data(); }
                out.disconnect(&b);
                churns.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    // Introspection reader (REST-style), concurrent with send + churn.
    std::thread introspect([&] {
        while (!stop.load(std::memory_order_acquire)) {
            (void)out.connection_count();
            (void)out.is_connected();
            introspections.fetch_add(1, std::memory_order_relaxed);
        }
    });

    // Producer: send continuously (single producer thread).
    for (std::uint64_t s = 0; s < 2'000'000; ++s) {
        while (a.is_full()) { std::this_thread::yield(); }  // throttle on the steady sink
        auto buf = make_mutable<std::uint64_t>(2);
        buf.as_span()[0] = s;
        out.send_data(std::move(buf), timestamp{});
    }
    stop.store(true, std::memory_order_release);
    cons_a.join();
    churn.join();
    introspect.join();

    if (bad.load()) { std::puts("FAIL: corruption"); return 1; }
    if (recv_a.load() == 0) { std::puts("FAIL: steady sink received nothing"); return 1; }
    std::printf("CONNECTION COW OK: a_recv=%llu churns=%llu introspections=%llu\n",
                (unsigned long long)recv_a.load(), (unsigned long long)churns.load(),
                (unsigned long long)introspections.load());
    return 0;
}
