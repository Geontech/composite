// Worker-originated property write. A component's process() updates its own
// RUNTIME property via set_properties(). The worker thread cannot park itself, so
// this exercises the with_worker_parked() worker-self path (skip the park, but
// still take the data write lock). A concurrent reader hammers property_state()
// to exercise the worker-writer vs REST-reader exclusion on m_data_mtx. Under TSan.
//
// If the worker-self path were missing, set_properties() would request a park and
// wait for the worker (itself) to reach a park point -> timeout -> throw out of
// process() -> the worker FINISHes and `count` never advances. Success == count
// advanced and the change listener fired.
#include <composite/core/component.hpp>

#include <atomic>
#include <cstdio>
#include <thread>

#include <spdlog/spdlog.h>

using namespace composite;
using composite::properties::config_type;
using json = composite::properties::json;

namespace {
class self_writer : public component {
public:
    explicit self_writer(std::string_view id) : component(id) {
        add_property("count", m_count, config_type::RUNTIME)
            .on_change([this](const json&) { m_notifies.fetch_add(1, std::memory_order_relaxed); });
    }
    auto process() -> retval override {
        // Worker mutating its OWN property mid-process() — the re-entrancy case.
        set_properties(json{{"count", m_count + 1}}, config_type::RUNTIME);
        return retval::NORMAL;
    }
    int m_count{0};
    std::atomic<std::uint64_t> m_notifies{0};
    component::auto_stop m_auto_stop{*this};  // MUST be last
};
} // namespace

int main() {
    spdlog::set_level(spdlog::level::off);

    auto comp = std::make_shared<self_writer>("self_writer");
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> reads{0};

    std::thread reader([&] {
        while (!stop.load(std::memory_order_acquire)) {
            auto state = comp->property_state();  // takes the shared read lock
            if (state.contains("count")) { reads.fetch_add(1, std::memory_order_relaxed); }
        }
    });

    comp->start();
    // Poll the atomic notify counter until the worker has self-written at least once
    // (race-free; m_count is non-atomic and only safe to read after the join below).
    // Avoids a fixed sleep that could be too short under sanitizer/CI load.
    for (int i = 0; i < 2000 && comp->m_notifies.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    comp->stop();  // worker joined here -> m_count is stable for the main thread
    stop.store(true, std::memory_order_release);
    reader.join();

    if (comp->m_count <= 0) {
        std::printf("FAIL: worker self-write did not advance (self-park deadlock?) count=%d\n", comp->m_count);
        return 1;
    }
    if (comp->m_notifies.load() == 0) {
        std::printf("FAIL: on_change never fired during worker self-write\n");
        return 1;
    }
    std::printf("WORKER SELF-WRITE OK: count=%d notifies=%llu concurrent_reads=%llu\n",
                comp->m_count, (unsigned long long)comp->m_notifies.load(),
                (unsigned long long)reads.load());
    return 0;
}
