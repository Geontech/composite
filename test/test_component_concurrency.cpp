// Component-level integration on the NEW property core: worker reads properties
// lock-free while JSON set_properties() mutates them (parked) + a REST reader
// reads them + lifecycle churn. Under TSan.
#include <composite/core/component.hpp>
#include <composite/ports/input_port.hpp>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>
#include <spdlog/spdlog.h>

using namespace composite;
using json = composite::properties::json;
using composite::properties::config_type;

class probe : public component {
public:
    explicit probe(std::string_view id) : component(id) {
        add_port(&m_in);
        add_property("a", m_a, config_type::RUNTIME);
        add_property("b", m_b, config_type::RUNTIME);
    }
    auto process() -> retval override {
        // Lock-free reads; a and b are written together in one parked batch,
        // so the worker must never see a != b.
        int32_t a = m_a, b = m_b;
        if (a != b) m_mismatch.fetch_add(1, std::memory_order_relaxed);
        m_reads.fetch_add(1, std::memory_order_relaxed);
        return retval::NORMAL;
    }
    std::atomic<uint64_t> m_reads{0}, m_mismatch{0};
    input_port<mutable_buffer<float>> m_in{"in"};
    int32_t m_a{0};
    int32_t m_b{0};
    component::auto_stop m_auto_stop{*this};   // MUST be last
};

int main() {
    spdlog::set_level(spdlog::level::off);
    std::atomic_bool stop{false};
    std::atomic<uint64_t> bad_reader{0};
    auto comp = std::make_shared<probe>("probe");
    comp->start();

    std::thread writer([&]{
        for (int k = 1; k <= 20000 && !stop.load(); ++k) {
            comp->set_properties(json{{"a", k}, {"b", k}}, config_type::RUNTIME);
        }
    });
    std::thread reader([&]{
        while (!stop.load(std::memory_order_acquire)) {
            comp->with_property_read_lock([&]{
                // read both under the SAME held read-lock: use the no-lock accessor,
                // since the locking get_property() would recursively acquire the
                // (non-recursive) shared_mutex we already hold.
                auto a = comp->get_property_locked<int32_t>("a");
                auto b = comp->get_property_locked<int32_t>("b");
                if (a != b) bad_reader.fetch_add(1, std::memory_order_relaxed);
            });
        }
    });
    std::thread churn([&]{
        for (int i = 0; i < 400 && !stop.load(); ++i) {
            comp->set_properties(json{{"enabled", (i & 1) == 0}}, config_type::RUNTIME);
            comp->apply_lifecycle_changes();
        }
    });

    writer.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop.store(true, std::memory_order_release);
    reader.join();
    churn.join();
    comp->set_properties(json{{"enabled", true}}, config_type::RUNTIME);
    comp->apply_lifecycle_changes();
    comp->stop();

    std::printf("component TSan: reads=%llu worker_mismatch=%llu reader_mismatch=%llu\n",
                (unsigned long long)comp->m_reads.load(),
                (unsigned long long)comp->m_mismatch.load(),
                (unsigned long long)bad_reader.load());
    assert(comp->m_mismatch.load() == 0 && bad_reader.load() == 0);
    std::printf("COMPONENT INTEGRATION TSAN PASSED\n");
    return 0;
}
