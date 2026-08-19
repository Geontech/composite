// Deterministic coverage for the registry's observer/enumeration synchronization. These schedules
// are not reached by ordinary test runs — each is constructed by hand.
#include <composite/metrics/registry.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <utility>
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

    // ---- (4) CROSS-THREAD: another thread's enumeration must not flush our deferred removals ----
    // The deferred queue is global, so keying the flush on a thread-LOCAL "outermost enumeration"
    // lets an unrelated thread destroy metrics that this thread's enumeration has not walked yet.
    // Thread A defers a bulk removal and pauses mid-enumeration; thread C runs and finishes its own
    // enumeration; A then keeps dereferencing snapshot entries. Under ASan a regression here is a
    // heap-use-after-free, not a wrong count.
    {
        registry::instance().clear();
        constexpr int k_metrics = 24;
        for (int i = 0; i < k_metrics; ++i) {
            registry::instance().create_counter("xthread.m" + std::to_string(i), "", "1", {{"group", "A"}});
        }

        std::atomic<bool> deferred{false};
        std::atomic<bool> other_done{false};
        std::atomic<int> touched{0};
        std::atomic<bool> saw_null{false};
        bool first = true;

        std::thread enumerator{[&] {
            auto id = registry::instance().add_observer(
                [&](const metric_metadata& meta, void* ptr) {
                    if (ptr == nullptr) {
                        saw_null.store(true);
                        return;
                    }
                    if (first) {
                        first = false;
                        // Bulk-remove everything, including entries this enumeration has not
                        // reached. Reentrant, so it is deferred rather than destroyed now.
                        registry::instance().remove_by_label("group", "A");
                        deferred.store(true, std::memory_order_release);
                        while (!other_done.load(std::memory_order_acquire)) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        }
                    }
                    // Every entry after the first is a deferred-removed metric: touching it is
                    // safe ONLY if nobody flushed the queue while this enumeration was paused.
                    if (meta.type == metric_type::counter) {
                        static_cast<counter<uint64_t>*>(ptr)->add(1);
                    }
                    touched.fetch_add(1, std::memory_order_relaxed);
                },
                true);
            registry::instance().remove_observer(id);
        }};

        std::thread other{[&] {
            while (!deferred.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            // A complete, unrelated enumeration on ANOTHER thread — begins and ends while the
            // first enumeration is still paused mid-walk.
            auto id = registry::instance().add_observer([](const metric_metadata&, void*) {}, true);
            registry::instance().remove_observer(id);
            other_done.store(true, std::memory_order_release);
        }};

        enumerator.join();
        other.join();

        check(!saw_null.load(), "cross-thread: no null pointer was published to the enumeration");
        check(touched.load() == k_metrics,
              "cross-thread: every snapshot entry was still valid after another thread's enumeration ended");
        std::printf("cross-thread: touched %d/%d snapshot entries\n", touched.load(), k_metrics);
    }

    // ---- (5) a removal must wait for an in-flight REGISTRATION notification ----
    // create_*() publishes a raw pointer to observers after releasing the registry lock, exactly
    // as the initial enumeration does. Counting only the enumeration let a concurrent removal
    // destroy the metric while a registration callback still held its pointer — and, because the
    // retraction then landed BEFORE the registration, left the observer holding that pointer
    // permanently rather than transiently. Asserted as an ORDERING, so this fails without ASan.
    {
        registry::instance().clear();
        std::atomic<bool> in_callback{false};
        std::atomic<bool> registration_returned{false};
        std::atomic<bool> retraction_saw_completed_registration{true};
        std::atomic<bool> retracted{false};

        auto reg_id = registry::instance().add_observer(
            [&](const metric_metadata& meta, void*) {
                if (meta.name != "pub.x") {
                    return;
                }
                in_callback.store(true, std::memory_order_release);
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                registration_returned.store(true, std::memory_order_release);
            },
            false);
        auto dereg_id = registry::instance().add_deregistration_observer([&](const metric_metadata& meta, void*) {
            if (meta.name != "pub.x") {
                return;
            }
            retraction_saw_completed_registration.store(registration_returned.load(std::memory_order_acquire),
                                                        std::memory_order_release);
            retracted.store(true, std::memory_order_release);
        });

        std::thread creator{[] { registry::instance().create_counter("pub.x"); }};
        while (!in_callback.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        registry::instance().remove_counter("pub.x"); // must not outrun the registration above
        creator.join();

        check(retracted.load(), "registration/removal: the retraction was delivered");
        check(retraction_saw_completed_registration.load(),
              "registration/removal: the retraction waited for the in-flight registration callback");
        registry::instance().remove_observer(reg_id);
        registry::instance().remove_deregistration_observer(dereg_id);
    }

    // ---- (6) removing an observer that is ALSO running on another thread ----
    // The self-removal shortcut keyed off a thread-local list, so an observer removing itself on
    // one thread skipped the in-flight wait for EVERY thread — and remove_observer() returned
    // while the same callback was still running elsewhere, against state the caller then tore
    // down. The wait must come down to this thread's own frames, not be skipped outright.
    {
        registry::instance().clear();
        std::atomic<bool> other_in_callback{false};
        std::atomic<bool> remove_returned{false};
        std::atomic<bool> callback_ran_after_removal{false};
        std::atomic<std::size_t> obs_id{0};

        obs_id.store(registry::instance().add_observer(
            [&](const metric_metadata& meta, void*) {
                if (meta.name == "self.slow") {
                    other_in_callback.store(true, std::memory_order_release);
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                    if (remove_returned.load(std::memory_order_acquire)) {
                        callback_ran_after_removal.store(true, std::memory_order_release);
                    }
                } else if (meta.name == "self.trigger") {
                    registry::instance().remove_observer(obs_id.load(std::memory_order_acquire));
                    remove_returned.store(true, std::memory_order_release);
                }
            },
            false));

        std::thread slow{[] { registry::instance().create_counter("self.slow"); }};
        while (!other_in_callback.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        registry::instance().create_counter("self.trigger"); // self-removal runs on THIS thread
        slow.join();
        check(!callback_ran_after_removal.load(),
              "self-removal: remove_observer() waited for the same observer running on another thread");
    }

    // ---- (7) a deferred retraction must not cancel a RE-CREATED metric ----
    // Deferred removals used to be keyed by name+labels, which does not identify a metric across a
    // delete/recreate — a callback that removes "x" and creates "x" again is legal. The retraction
    // for the original then arrived while the replacement was live and cancelled it, leaving a
    // live metric silently unexported. Consumers match on the POINTER for this reason.
    {
        registry::instance().clear();
        std::map<std::string, void*> exported;
        auto reg_id = registry::instance().add_observer(
            [&](const metric_metadata& meta, void* ptr) { exported[meta.name] = ptr; }, false);
        auto dereg_id = registry::instance().add_deregistration_observer([&](const metric_metadata& meta, void* ptr) {
            auto it = exported.find(meta.name);
            if (it != exported.end() && it->second == ptr) { // identity, not name
                exported.erase(it);
            }
        });

        registry::instance().create_counter("ident.x");
        bool once = true;
        auto walker = registry::instance().add_observer(
            [&](const metric_metadata&, void*) {
                if (!std::exchange(once, false)) {
                    return;
                }
                registry::instance().remove_counter("ident.x"); // deferred: we are enumerating
                registry::instance().create_counter("ident.x"); // same name, different metric
            },
            true);

        auto& live = registry::instance().get_or_create_counter("ident.x"); // returns the existing one
        check(exported.count("ident.x") == 1, "recreate: the replacement metric is still exported");
        check(exported.count("ident.x") == 1 && exported["ident.x"] == static_cast<void*>(&live),
              "recreate: the exported pointer is the LIVE metric, not the destroyed original");
        registry::instance().remove_observer(walker);
        registry::instance().remove_observer(reg_id);
        registry::instance().remove_deregistration_observer(dereg_id);
    }

    // ---- (8) clear() called from inside an enumeration callback ----
    // clear() bypassed the deferral and the drain entirely, destroying the very metrics the
    // enclosing enumeration was still walking. Asserted as an ordering so it fails without ASan:
    // no retraction may be delivered until the enumeration has finished.
    {
        registry::instance().clear();
        for (int i = 0; i < 6; ++i) {
            registry::instance().create_counter("clr." + std::to_string(i));
        }
        // How much of the snapshot had been walked when the first retraction arrived. The flush
        // runs inside add_observer()'s guard, so "after the walk" is still inside the call — a
        // wall-clock or in-scope flag cannot tell the two apart, but the visit count can.
        int visited = 0;
        int visited_at_first_retraction = -1;
        auto dereg_id = registry::instance().add_deregistration_observer([&](const metric_metadata&, void*) {
            if (visited_at_first_retraction < 0) {
                visited_at_first_retraction = visited;
            }
        });

        bool first = true;
        auto id = registry::instance().add_observer(
            [&](const metric_metadata&, void* ptr) {
                ++visited;
                if (ptr != nullptr) {
                    static_cast<counter<uint64_t>*>(ptr)->add(1); // fatal under ASan if destroyed
                }
                if (std::exchange(first, false)) {
                    registry::instance().clear(); // reentrant, mid-walk
                }
            },
            true);

        check(visited == 6, "clear-during-enumeration: the whole snapshot was still walked");
        check(visited_at_first_retraction == 6,
              "clear-during-enumeration: no retraction was delivered until the walk had finished");
        registry::instance().remove_observer(id);
        registry::instance().remove_deregistration_observer(dereg_id);
    }

    registry::instance().clear();
    if (g_fails != 0) {
        std::fprintf(stderr, "%d observer-sync check(s) FAILED\n", g_fails);
        return 1;
    }
    std::puts("REGISTRY OBSERVER SYNC TESTS PASSED");
    return 0;
}
