// snapshot<T>: atomically published, immutable config for threads the park never quiesces
// (pipeline pool workers, receiver threads). Semantics: default-constructed loads null; the
// initial-value constructor and both publish() overloads work; a reader holding an old value
// keeps it alive across republishes. Concurrency (meaningful under TSan/ASan): one publisher
// republishing a string+vector struct at full rate while readers load and validate an internal
// invariant — no torn reads, no use-after-free, and readers observe generations moving forward.
#include "composite/properties/snapshot.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
int g_failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

// Heap-owning fields make a freed or torn read sanitizer-visible; the invariant ties the
// fields together so a mixed-generation read is detectable even without sanitizers.
struct cfg {
    std::uint64_t gen{0};
    std::string tag;           // == "gen-" + gen
    std::vector<double> table; // size == (gen % 64) + 1, every element == gen
};

auto make_cfg(std::uint64_t gen) -> cfg {
    return cfg{gen, "gen-" + std::to_string(gen), std::vector<double>((gen % 64) + 1, static_cast<double>(gen))};
}

auto coherent(const cfg& c) -> bool {
    if (c.tag != "gen-" + std::to_string(c.gen) || c.table.size() != (c.gen % 64) + 1) {
        return false;
    }
    for (double v : c.table) {
        if (v != static_cast<double>(c.gen)) {
            return false;
        }
    }
    return true;
}
} // namespace

int main() {
    // Default-constructed: loads null (the documented "publish an initial value" contract).
    {
        composite::snapshot<cfg> snap;
        check(snap.load() == nullptr, "default: loads null");
    }

    // Initial-value constructor + by-value publish + prebuilt shared_ptr publish.
    {
        composite::snapshot<cfg> snap{make_cfg(1)};
        auto v1 = snap.load();
        check(v1 && v1->gen == 1 && coherent(*v1), "ctor: initial value visible");

        snap.publish(make_cfg(2));
        auto v2 = snap.load();
        check(v2 && v2->gen == 2 && coherent(*v2), "publish(T): new value visible");

        snap.publish(std::make_shared<const cfg>(make_cfg(3)));
        auto v3 = snap.load();
        check(v3 && v3->gen == 3 && coherent(*v3), "publish(shared_ptr): new value visible");

        // Lifetime: the reader-held generations survive the republishes untouched.
        check(v1->gen == 1 && coherent(*v1), "lifetime: held gen-1 alive and intact");
        check(v2->gen == 2 && coherent(*v2), "lifetime: held gen-2 alive and intact");
    }

    // Publisher/readers race: publisher republishes at full rate; readers load + validate.
    {
        composite::snapshot<cfg> snap{make_cfg(0)};
        std::atomic<bool> stop{false};
        std::atomic<bool> incoherent{false};
        std::atomic<std::uint64_t> loads{0};

        std::vector<std::thread> readers;
        for (int r = 0; r < 3; ++r) {
            readers.emplace_back([&] {
                std::uint64_t last_seen = 0;
                while (!stop.load(std::memory_order_acquire)) {
                    auto v = snap.load();
                    if (!v || !coherent(*v) || v->gen < last_seen) {
                        incoherent.store(true, std::memory_order_relaxed);
                        return;
                    }
                    last_seen = v->gen;
                    loads.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        constexpr std::uint64_t GENERATIONS = 20000;
        for (std::uint64_t gen = 1; gen <= GENERATIONS; ++gen) {
            snap.publish(make_cfg(gen));
        }
        stop.store(true, std::memory_order_release);
        for (auto& t : readers) {
            t.join();
        }

        check(!incoherent.load(), "race: every loaded value coherent and monotonic");
        check(loads.load() > 0, "race: readers made progress");
        auto last = snap.load();
        check(last && last->gen == GENERATIONS && coherent(*last), "race: final generation visible");
    }

    if (g_failures == 0) {
        std::puts("PASS: snapshot<T>");
        return 0;
    }
    std::printf("%d failure(s)\n", g_failures);
    return 1;
}
