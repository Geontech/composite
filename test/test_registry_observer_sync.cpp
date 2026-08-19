// Deterministic coverage for the registry's observer/enumeration synchronization. These schedules
// are not reached by ordinary test runs — each is constructed by hand.
#include <composite/metrics/registry.hpp>

#include <cstdio>
#include <string>
#include <vector>

using namespace composite::metrics;

namespace {
int g_fails = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_fails;
    }
}
} // namespace

int main() {
    // ---- (1) an observer that removes a metric DURING initial enumeration ----
    // The callback for one snapshot entry removes another. Destroying it immediately would leave
    // the enumeration walking a dangling pointer, so the removal must be deferred. Run under ASan:
    // a regression here is a heap-use-after-free, not a wrong count.
    {
        registry::instance().clear();
        registry::instance().create_counter("enum.a");
        registry::instance().create_counter("enum.b");

        std::vector<std::string> seen;
        bool dereferenced_all = true;
        auto id = registry::instance().add_observer(
            [&](const metric_metadata& meta, void* ptr) {
                seen.push_back(meta.name);
                if (ptr == nullptr) {
                    dereferenced_all = false;
                    return;
                }
                // Touch the metric, which is what makes a premature destruction fatal.
                if (meta.type == metric_type::counter) {
                    static_cast<counter<uint64_t>*>(ptr)->add(1);
                }
                if (meta.name == "enum.a") {
                    registry::instance().remove_counter("enum.b"); // reentrant removal
                }
            },
            true);

        check(seen.size() == 2, "enumeration visited both metrics despite a reentrant removal");
        check(dereferenced_all, "every enumerated pointer was still valid when the callback ran");
        registry::instance().remove_observer(id);
    }

    // ---- (2) an observer that removes ITSELF from inside its own callback ----
    // retire_observer() waits for in_flight to reach zero, which only this call can do — so it
    // must recognise self-removal and skip the wait. A regression is a hang.
    {
        registry::instance().clear();
        std::size_t self_id = 0;
        int calls = 0;
        self_id = registry::instance().add_observer(
            [&](const metric_metadata&, void*) {
                ++calls;
                registry::instance().remove_observer(self_id); // must not deadlock
            },
            false);

        registry::instance().create_counter("self.one");
        registry::instance().create_counter("self.two");
        check(calls == 1, "a self-removed observer stopped being called after it retired");
        std::puts("self-removal returned without deadlocking");
    }

    // ---- (3) bulk removal during enumeration is deferred too ----
    {
        registry::instance().clear();
        registry::instance().create_counter("bulk.keep", "", "1", {{"component_id", "keep"}});
        registry::instance().create_counter("bulk.drop", "", "1", {{"component_id", "drop"}});

        int visited = 0;
        bool all_valid = true;
        auto id = registry::instance().add_observer(
            [&](const metric_metadata& meta, void* ptr) {
                ++visited;
                if (ptr == nullptr) {
                    all_valid = false;
                    return;
                }
                static_cast<counter<uint64_t>*>(ptr)->add(1);
                if (meta.name == "bulk.keep") {
                    registry::instance().remove_by_label("component_id", "drop"); // reentrant bulk
                }
            },
            true);

        check(visited == 2, "bulk: enumeration visited both metrics");
        check(all_valid, "bulk: enumerated pointers stayed valid across a reentrant bulk removal");
        registry::instance().remove_observer(id);
    }

    registry::instance().clear();
    if (g_fails != 0) {
        std::fprintf(stderr, "%d observer-sync check(s) FAILED\n", g_fails);
        return 1;
    }
    std::puts("REGISTRY OBSERVER SYNC TESTS PASSED");
    return 0;
}
