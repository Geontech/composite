// Application component registry under ThreadSanitizer: concurrent shared readers
// (get_component / components() snapshot) racing against unique writers
// (add_component, which atomically rejects duplicate ids) and a clear(). Verifies
// the std::shared_mutex registry is race-free and that snapshots returned to a
// reader stay valid even while a writer mutates or clears the registry.
#include <composite/buffers/buffer.hpp>
#include <composite/core/application.hpp>
#include <composite/core/component.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

using namespace composite;

namespace {
class noop_component : public component {
public:
    explicit noop_component(std::string_view id) : component(id) {}
    auto process() -> retval override { return retval::NOOP; }
    component::auto_stop m_auto_stop{*this}; // MUST be last
};
} // namespace

int main() {
    spdlog::set_level(spdlog::level::off);

    application app{"reg"};
    // Seed a stable set the readers always expect to find.
    for (int i = 0; i < 16; ++i) {
        bool ok = app.add_component(std::make_shared<noop_component>("c" + std::to_string(i)));
        if (!ok) {
            std::puts("seed add failed");
            return 1;
        }
    }

    // Duplicate id is rejected atomically.
    if (app.add_component(std::make_shared<noop_component>("c7"))) {
        std::puts("FAIL: duplicate id accepted");
        return 1;
    }
    if (app.add_component(nullptr)) {
        std::puts("FAIL: null accepted");
        return 1;
    }

    std::atomic_bool stop{false};
    std::atomic<std::uint64_t> reads{0}, lookups_found{0}, bad{0};

    std::vector<std::thread> threads;

    // Shared readers: get_component() + components() snapshot iteration.
    for (int r = 0; r < 4; ++r) {
        threads.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                if (auto c = app.get_component("c7")) {
                    if (c->id() != "c7") {
                        bad.fetch_add(1, std::memory_order_relaxed);
                    }
                    lookups_found.fetch_add(1, std::memory_order_relaxed);
                }
                auto snap = app.components(); // snapshot copy
                for (const auto& c : snap) {
                    if (c == nullptr || c->id().empty()) {
                        bad.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Unique writers: each adds its own non-colliding id range.
    for (int w = 0; w < 2; ++w) {
        threads.emplace_back([&, w] {
            for (int i = 0; i < 200; ++i) {
                app.add_component(std::make_shared<noop_component>("w" + std::to_string(w) + "_" + std::to_string(i)));
            }
        });
    }

    // Let the readers run against a live, growing registry.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : threads) {
        t.join();
    }

    // 16 seed + 2*200 writer ids, all unique.
    const auto size_before_clear = app.components().size();
    if (size_before_clear != 16 + 400) {
        std::printf("FAIL: expected %d components, got %zu\n", 16 + 400, size_before_clear);
        return 1;
    }

    // Reader racing a clear(): the snapshot must stay valid (shared_ptr keeps the
    // components alive even as clear() empties the registry).
    std::atomic_bool clearing{false};
    std::thread reader([&] {
        for (int i = 0; i < 2000; ++i) {
            auto snap = app.components();
            for (const auto& c : snap) {
                if (c) {
                    (void)c->id();
                }
            }
        }
        clearing.store(true, std::memory_order_relaxed);
    });
    while (!clearing.load(std::memory_order_relaxed)) {
        app.clear();
    }
    reader.join();
    app.clear();

    if (!app.components().empty()) {
        std::puts("FAIL: registry not empty after clear");
        return 1;
    }
    if (bad.load() != 0) {
        std::printf("FAIL: %llu bad reads\n", (unsigned long long)bad.load());
        return 1;
    }

    std::printf("application registry TSAN PASSED: reads=%llu found=%llu final=%zu\n", (unsigned long long)reads.load(),
                (unsigned long long)lookups_found.load(), size_before_clear);

    // ---- remove_component(): a peer parked in m_unremovable must still be treated as a
    // producer/consumer when a LATER remove_component() disconnects edges into/out of its
    // target ----
    //
    // A component whose own removal failed to unwire an edge is retained (never destroyed —
    // that is the entire point of m_unremovable) but is invisible to lookups. It is still alive
    // and still wired, so a peer connected to it must be disconnected from it exactly like any
    // registered component when THAT peer is later removed. The bug: the disconnect loop was
    // built from m_components alone, so a producer sitting in m_unremovable was never
    // disconnected from a consumer being removed — the consumer was destroyed while the
    // producer's output port still held a raw pointer into it.
    //
    // Reproducing a genuinely wedged worker here is impractical: application::remove_component()
    // stops the target via the UNBOUNDED stop() (not try_stop()), so a component whose process()
    // truly never returns would hang this test forever rather than exercising the failure path.
    // Instead this uses the COMPOSITE_TESTING park_for_test() hook to hold a component's
    // admission gate shut from another thread, which fails a disconnect() the exact same way
    // (with_worker_parked() throws) without needing a live worker at all.
    {
        class registry_producer : public component {
        public:
            output_port<immutable_buffer<float>> out{"out"};
            explicit registry_producer(std::string_view id) : component(id) { add_port(out); }
            auto process() -> retval override { return retval::NOOP; }
            component::auto_stop m_auto_stop{*this};
        };
        class registry_consumer : public component {
        public:
            input_port<immutable_buffer<float>> in{"in", 16};
            explicit registry_consumer(std::string_view id) : component(id) { add_port(in); }
            auto process() -> retval override { return retval::NOOP; }
            component::auto_stop m_auto_stop{*this};
        };

        application reg_app{"unremovable"};
        auto p = std::make_shared<registry_producer>("P");
        auto c = std::make_shared<registry_consumer>("C");
        if (!reg_app.add_component(p) || !reg_app.add_component(c)) {
            std::puts("FAIL: could not seed producer/consumer");
            return 1;
        }
        if (!p->connect("out", c, "in")) {
            std::puts("FAIL: could not connect producer -> consumer");
            return 1;
        }
        if (p->out.connection_count() != 1) {
            std::puts("FAIL: producer not connected before the test begins");
            return 1;
        }

        // Force P's own removal to fail so it lands in m_unremovable rather than back in
        // m_components. Hold its admission gate shut from a thread OTHER than the one that
        // will call remove_component() — closing it from that same thread would just make the
        // thread the gate's own bypass owner and the disconnect below would never trip the
        // timeout. A short park timeout makes the failure surface in milliseconds rather than
        // the 5s default, while leaving the id re-take below a wide margin to land inside it.
        p->park_for_test().set_timeout(std::chrono::milliseconds(300));
        std::thread closer([&] { p->park_for_test().close_admission(); });
        closer.join();

        // Race a concurrent add_component() that re-takes id "P" while remove_component("P")
        // is blocked failing to disconnect P's own output. Re-taking the id is what routes the
        // retained component into m_unremovable instead of back into m_components (see
        // remove_component()'s id_free check).
        auto stand_in = std::make_shared<registry_producer>("P");
        std::thread retaker([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            reg_app.add_component(stand_in);
        });

        bool threw = false;
        try {
            reg_app.remove_component("P");
        } catch (const std::exception&) {
            threw = true;
        }
        retaker.join();

        if (!threw) {
            std::puts("FAIL: remove_component('P') should have failed while its admission gate was held shut");
            return 1;
        }
        if (reg_app.get_component("P").get() != stand_in.get()) {
            std::puts("FAIL: id 'P' was not re-taken by the stand-in (setup for m_unremovable is wrong)");
            return 1;
        }

        // Reopen the real producer's admission gate: the earlier closure was only there to fail
        // its OWN removal (routing it into m_unremovable). Leaving it shut would also fail the
        // disconnect below regardless of the fix, which is not what this case is checking.
        p->park_for_test().open_admission();

        // The real producer is now retained off to the side in m_unremovable, still connected
        // to C. Removing C must still disconnect that edge.
        auto removed_c = reg_app.remove_component("C");
        if (removed_c == nullptr) {
            std::puts("FAIL: remove_component('C') should have succeeded");
            return 1;
        }
        if (p->out.connection_count() != 0) {
            std::puts("FAIL: retained-unremovable producer's output is still connected after its consumer was "
                      "removed");
            return 1;
        }
        if (!p->connections().empty()) {
            std::puts("FAIL: retained-unremovable producer's connection bookkeeping was not cleared");
            return 1;
        }

        // Sending after the disconnect must be a safe no-op, and dropping the removed consumer
        // afterward must not fault (the disconnected send path never touches it).
        p->out.send_data(make_immutable<float>({1.0F}), timestamp{});
        removed_c.reset();
        p->out.send_data(make_immutable<float>({2.0F}), timestamp{});

        std::puts("remove_component m_unremovable union regression OK");
    }

    return 0;
}
