// Application component registry under ThreadSanitizer: concurrent shared readers
// (get_component / components() snapshot) racing against unique writers
// (add_component, which atomically rejects duplicate ids) and a clear(). Verifies
// the std::shared_mutex registry is race-free and that snapshots returned to a
// reader stay valid even while a writer mutates or clears the registry.
#include <composite/core/application.hpp>
#include <composite/core/component.hpp>

#include <atomic>
#include <cstdio>
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
    component::auto_stop m_auto_stop{*this};  // MUST be last
};
} // namespace

int main() {
    spdlog::set_level(spdlog::level::off);

    application app{"reg"};
    // Seed a stable set the readers always expect to find.
    for (int i = 0; i < 16; ++i) {
        bool ok = app.add_component(std::make_shared<noop_component>("c" + std::to_string(i)));
        if (!ok) { std::puts("seed add failed"); return 1; }
    }

    // Duplicate id is rejected atomically.
    if (app.add_component(std::make_shared<noop_component>("c7"))) {
        std::puts("FAIL: duplicate id accepted"); return 1;
    }
    if (app.add_component(nullptr)) { std::puts("FAIL: null accepted"); return 1; }

    std::atomic_bool stop{false};
    std::atomic<std::uint64_t> reads{0}, lookups_found{0}, bad{0};

    std::vector<std::thread> threads;

    // Shared readers: get_component() + components() snapshot iteration.
    for (int r = 0; r < 4; ++r) {
        threads.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                if (auto c = app.get_component("c7")) {
                    if (c->id() != "c7") { bad.fetch_add(1, std::memory_order_relaxed); }
                    lookups_found.fetch_add(1, std::memory_order_relaxed);
                }
                auto snap = app.components();           // snapshot copy
                for (const auto& c : snap) {
                    if (c == nullptr || c->id().empty()) { bad.fetch_add(1, std::memory_order_relaxed); }
                }
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Unique writers: each adds its own non-colliding id range.
    for (int w = 0; w < 2; ++w) {
        threads.emplace_back([&, w] {
            for (int i = 0; i < 200; ++i) {
                app.add_component(std::make_shared<noop_component>(
                    "w" + std::to_string(w) + "_" + std::to_string(i)));
            }
        });
    }

    // Let the readers run against a live, growing registry.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : threads) { t.join(); }

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
            for (const auto& c : snap) { if (c) { (void)c->id(); } }
        }
        clearing.store(true, std::memory_order_relaxed);
    });
    while (!clearing.load(std::memory_order_relaxed)) { app.clear(); }
    reader.join();
    app.clear();

    if (!app.components().empty()) { std::puts("FAIL: registry not empty after clear"); return 1; }
    if (bad.load() != 0) { std::printf("FAIL: %llu bad reads\n", (unsigned long long)bad.load()); return 1; }

    std::printf("application registry TSAN PASSED: reads=%llu found=%llu final=%zu\n",
                (unsigned long long)reads.load(), (unsigned long long)lookups_found.load(),
                size_before_clear);
    return 0;
}
