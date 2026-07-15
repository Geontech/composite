// make_component<T>: the returned shared_ptr's deleter stops the component BEFORE ~T runs, so
// dropping the last reference of a still-RUNNING component joins the worker (and runs the
// on_worker_stop hook) while the leaf vtable and derived members are intact — the heap-side
// counterpart of the auto_stop member, with no member-ordering rule to remember. Verifies: the
// stop-then-destroy order; that the deleter survives the upcast to shared_ptr<component> (the
// application registry's type); idempotence for stopped and never-started components; that
// COMPOSITE_REGISTER_SIMPLE builds through it; and the connect-time mutability diagnostics
// (an immutable->mutable connect warns about the per-frame deep copy but still succeeds).
#include "composite/buffers/buffer.hpp"
#include "composite/core/component.hpp"
#include "composite/core/register.hpp"
#include "composite/ports/input_port.hpp"
#include "composite/ports/output_port.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

using namespace composite;
using namespace std::chrono_literals;

namespace {
int g_failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

// Teardown order recorded through external slots (the object is gone by assertion time).
struct teardown_seq {
    std::atomic<int> next{0};
    std::atomic<int> worker_stopped{-1}; // sequence slot when on_worker_stop ran (-1 = never)
    std::atomic<int> destroyed{-1};      // sequence slot when the leaf destructor ran
    std::atomic<int> stop_hooks{0};      // how many times on_worker_stop ran
    std::atomic<std::uint64_t> iterations{0};
};

// Worker writes derived HEAP state every iteration. Deliberately NO auto_stop member: heap
// lifetime safety comes from the make_component deleter — if it failed to stop first, the
// still-running worker would write m_data after ~churner freed it (ASan/TSan-visible).
class churner : public component {
public:
    churner(std::string_view id, teardown_seq* seq) : component(id), m_seq(seq), m_data(1024, 0) {}
    ~churner() override { m_seq->destroyed = m_seq->next.fetch_add(1); }
    auto process() -> retval override {
        for (auto& v : m_data) {
            ++v;
        }
        m_seq->iterations.fetch_add(1, std::memory_order_relaxed);
        return retval::NORMAL;
    }

protected:
    auto on_worker_stop() -> void override {
        m_seq->worker_stopped = m_seq->next.fetch_add(1);
        m_seq->stop_hooks.fetch_add(1, std::memory_order_relaxed);
    }

private:
    teardown_seq* m_seq;
    std::vector<std::uint64_t> m_data;
};

auto wait_for_iterations(const teardown_seq& seq, std::uint64_t n) -> bool {
    for (int i = 0; i < 2000; ++i) {
        if (seq.iterations.load(std::memory_order_relaxed) >= n) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

// Minimal endpoints for the connect-time mutability diagnostics.
class imm_producer : public component {
public:
    explicit imm_producer(std::string_view id) : component(id) { add_port(&m_out); }
    auto process() -> retval override { return retval::NOOP; }

private:
    output_port<immutable_buffer<float>> m_out{"data_out"};
};

class mut_consumer : public component {
public:
    explicit mut_consumer(std::string_view id) : component(id) { add_port(&m_in); }
    auto process() -> retval override { return retval::NOOP; }

private:
    input_port<mutable_buffer<float>> m_in{"data_in"};
};

class imm_consumer : public component {
public:
    explicit imm_consumer(std::string_view id) : component(id) { add_port(&m_in); }
    auto process() -> retval override { return retval::NOOP; }

private:
    input_port<immutable_buffer<float>> m_in{"data_in"};
};

// Exercise the registration macro's default factory (which builds via make_component).
class reg_comp : public component {
public:
    explicit reg_comp(std::string_view id) : component(id) {}
    auto process() -> retval override { return retval::NOOP; }
};
} // namespace

COMPOSITE_REGISTER_SIMPLE(reg_comp)

int main() {
    // 1) Destroy while RUNNING: the deleter must stop (joining the worker, firing the derived
    //    on_worker_stop) before the leaf destructor runs.
    {
        teardown_seq seq;
        auto comp = make_component<churner>("churn-running", &seq);
        comp->start();
        check(wait_for_iterations(seq, 100), "running: worker iterated");
        comp.reset();
        check(seq.worker_stopped.load() != -1, "running: on_worker_stop ran");
        check(seq.destroyed.load() != -1, "running: destructor ran");
        check(seq.worker_stopped.load() < seq.destroyed.load(), "running: stopped BEFORE destroyed");
        check(seq.stop_hooks.load() == 1, "running: on_worker_stop ran exactly once");
    }

    // 2) The deleter survives the upcast to shared_ptr<component> — the type the application
    //    registry holds — so registry-owned components get the same protection.
    {
        teardown_seq seq;
        std::shared_ptr<component> comp = make_component<churner>("churn-upcast", &seq);
        comp->start();
        check(wait_for_iterations(seq, 100), "upcast: worker iterated");
        comp.reset();
        check(seq.worker_stopped.load() != -1 && seq.destroyed.load() != -1 &&
                  seq.worker_stopped.load() < seq.destroyed.load(),
              "upcast: stopped BEFORE destroyed");
    }

    // 3) Never started: destruction is a clean no-op stop (no hook, no crash).
    {
        teardown_seq seq;
        auto comp = make_component<churner>("churn-cold", &seq);
        comp.reset();
        check(seq.destroyed.load() != -1, "cold: destructor ran");
        check(seq.worker_stopped.load() == -1, "cold: no worker -> no on_worker_stop");
    }

    // 4) Explicitly stopped before destruction: the deleter's stop() is idempotent, the hook
    //    still runs exactly once (on the explicit stop).
    {
        teardown_seq seq;
        auto comp = make_component<churner>("churn-stopped", &seq);
        comp->start();
        check(wait_for_iterations(seq, 10), "stopped: worker iterated");
        comp->stop();
        check(seq.stop_hooks.load() == 1, "stopped: hook ran on the explicit stop");
        comp.reset();
        check(seq.stop_hooks.load() == 1, "stopped: deleter did not re-run the hook");
        check(seq.destroyed.load() != -1, "stopped: destructor ran");
    }

    // 5) The dynamic-registration default factory builds through make_component: create(),
    //    start, and drop the last reference while running.
    {
        auto comp = create("reg-live", composite::create_args{});
        check(comp != nullptr, "macro: create() built the component");
        if (comp) {
            comp->start();
            // is_running() flips when the spawned worker registers with the park — poll, don't
            // assert instantly (start() returns before the thread's first instruction).
            bool running = false;
            for (int i = 0; i < 2000 && !running; ++i) {
                running = comp->is_running();
                if (!running) {
                    std::this_thread::sleep_for(1ms);
                }
            }
            check(running, "macro: component running");
            comp.reset(); // deleter stops the live worker; sanitizers police the teardown
        }
    }

    // 6) Connect-time mutability diagnostics: immutable -> mutable warns (per-frame deep copy)
    //    but MUST still connect; immutable -> immutable stays on the quiet trace path.
    {
        auto prod = make_component<imm_producer>("warn-prod");
        auto warm = make_component<mut_consumer>("warn-mut");
        auto cold = make_component<imm_consumer>("warn-imm");
        check(prod->connect("data_out", warm, "data_in"), "connect: immutable->mutable still succeeds (warns)");
        check(prod->connect("data_out", cold, "data_in"), "connect: immutable->immutable succeeds");
    }

    if (g_failures == 0) {
        std::puts("PASS: make_component / final-lifecycle / connect diagnostics");
        return 0;
    }
    std::printf("%d failure(s)\n", g_failures);
    return 1;
}
