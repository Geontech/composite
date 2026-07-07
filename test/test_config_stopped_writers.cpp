// Regression: two concurrent inline property writers on a STOPPED component (no worker).
//
// A config<T> on_apply reaction is normally drained by the worker at loop-top. When there is
// NO worker (a stopped / source / INITIALIZE-time component) the writer drains it inline.
// Before the fix that inline drain ran on_apply OUTSIDE every lock (component.hpp), so two
// concurrent set_properties() callers ran their on_apply bodies at the same time: each read
// m_cfg->m_value (which here carries a std::string) while the other writer's parked apply()
// swapped it, and each wrote a shared non-atomic component member. With std::string members
// that is a use-after-free, not merely a torn scalar — and no prior test exercised concurrent
// writers on a stopped component, so TSan had never seen it.
//
// The fix routes the drain through park::with_worker_parked(); with no worker that takes the
// inline-writer-gated path, which holds m_data_mtx and registers in m_inline_writers, so the
// drains (and the on_apply bodies) are serialized against each other and against the value
// swap. This test must be TSan-clean. Own main(); linked against composite::composite.
#include <composite/core/component.hpp>

#include <spdlog/spdlog.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

using namespace composite;
using composite::properties::config_type;
using json = composite::properties::json;

// std::string field: the reaction reads/copies it, so an unserialized concurrent drain is a
// data race on the string's heap buffer, not just a torn int.
struct scfg {
    std::string s{"init"};
    COMPOSITE_FIELDS(scfg, (s, runtime));
};

class stopped_probe : public component {
public:
    explicit stopped_probe(std::string_view id) : component(id) {
        add_config(m_cfg, config_type::RUNTIME);
        // "rebuild derived state" reaction: mirror the committed string into a plain member.
        // Reads the committed config value (new_value -> m_cfg->m_value.s) AND writes a shared
        // non-atomic member — both race if two drains run concurrently.
        m_cfg.on_apply([this](const scfg&, const changes<scfg>& ch) {
            if (auto v = ch.new_value(&scfg::s)) {
                m_mirror = *v;
                m_applies.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Present so the type is concrete; never invoked (the component is never started).
    auto process() -> retval override { return retval::NOOP; }

    config<scfg> m_cfg{};
    std::string m_mirror{};                    // written only by on_apply
    std::atomic<long> m_applies{0};
    component::auto_stop m_auto_stop{*this};    // MUST be last
};

int main() {
    spdlog::set_level(spdlog::level::off);
    auto comp = std::make_shared<stopped_probe>("stopped");
    // NEVER started: has_worker() stays false, so every set_properties() drains its staged
    // reaction inline — the exact race path.

    constexpr int N = 20000;
    std::thread w1([&] {
        for (int k = 0; k < N; ++k) {
            comp->set_properties(json{{"s", "a" + std::to_string(k)}}, config_type::RUNTIME);
        }
    });
    std::thread w2([&] {
        for (int k = 0; k < N; ++k) {
            comp->set_properties(json{{"s", "b" + std::to_string(k)}}, config_type::RUNTIME);
        }
    });
    w1.join();
    w2.join();

    // Correctness is TSan's job here; the counters just prove the reactions actually ran.
    std::printf("K1 stopped-writer TSan: applies=%ld mirror=%s\n",
                comp->m_applies.load(std::memory_order_relaxed), comp->m_mirror.c_str());
    if (comp->m_applies.load(std::memory_order_relaxed) == 0) {
        std::fprintf(stderr, "FAIL: no reactions ran\n");
        return 1;
    }
    std::printf("K1 STOPPED-WRITER TSAN PASSED\n");
    return 0;
}
