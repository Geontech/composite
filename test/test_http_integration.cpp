/*
 * Copyright (C) 2024-2026 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * REST integration tests against the production make_server (server.cpp) using
 * the new JSON property routes: GET state, PATCH component, PUT/DELETE property,
 * RFC-7396 nested + keyed updates, validation -> 4xx.
 */

#include "composite/core/application.hpp"
#include "composite/core/component.hpp"
#include "helpers.hpp" // make_server consumes the production helpers

#include <catch2/catch_test_macros.hpp>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace composite;
using namespace std::chrono_literals;
using composite::properties::config_type;
using json = composite::properties::json;

// make_server is defined in server.cpp (compiled into this test target).
namespace composite {
auto make_server(application&) -> std::unique_ptr<httplib::Server>;
}

namespace {
enum class Win { hann, hamming };
struct Chan {
    double cf{};
    double bw{};
    Win win{Win::hann};
};
struct Net {
    std::string host{"localhost"};
    std::uint16_t port{8080};
};
} // namespace

COMPOSITE_ENUM(Win, hann, hamming);
COMPOSITE_STRUCT(Chan, cf, bw, win);
COMPOSITE_STRUCT(Net, host, port);

namespace {

class rest_component : public component {
public:
    explicit rest_component(std::string_view id) : component(id) {
        add_port(&m_in);
        add_port(&m_out);
        add_property("gain", m_gain, config_type::RUNTIME).validate([](const double& g) { return g > 0.0; });
        add_property("buf_size", m_buf, config_type::INITIALIZE);
        add_property("net", m_net, config_type::RUNTIME);
        add_keyed("channels", m_channels, config_type::RUNTIME).validate_element([](const std::string&, const Chan& c) {
            return c.bw > 0.0;
        });
    }
    auto process() -> retval override { return retval::NOOP; }
    input_port<mutable_buffer<float>> m_in{"data_in"};
    output_port<mutable_buffer<float>> m_out{"data_out"};
    double m_gain{1.0};
    std::int32_t m_buf{1024};
    Net m_net;
    std::map<std::string, Chan> m_channels;
    component::auto_stop m_auto_stop{*this}; // MUST be last
};

class rest_fixture {
public:
    rest_fixture() {
        m_app.add_component(std::make_shared<rest_component>("c1"));
        m_app.add_component(std::make_shared<rest_component>("c2")); // for batch-PATCH tests
        m_server = composite::make_server(m_app);
        // Bind to an ephemeral port (0) so concurrent runs / a busy 18091 can't make
        // listen() silently fail and leave every request returning a null Result
        // (which the assertions would then dereference -> segfault).
        m_port = m_server->bind_to_any_port("localhost");
        REQUIRE(m_port > 0); // bind must succeed before any request is issued
        m_thread = std::thread([this] { m_server->listen_after_bind(); });
        for (int i = 0; i < 400 && !m_server->is_running(); ++i) {
            std::this_thread::sleep_for(5ms);
        }
        REQUIRE(m_server->is_running()); // fail fast instead of deref-ing null responses later
    }
    ~rest_fixture() {
        m_server->stop();
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }
    auto client() -> httplib::Client { return httplib::Client("localhost", m_port); }

    application m_app{"testapp"};
    int m_port{0}; // assigned by bind_to_any_port() in the constructor
    std::unique_ptr<httplib::Server> m_server;
    std::thread m_thread;
};

} // namespace

TEST_CASE_METHOD(rest_fixture, "GET components + properties", "[http]") {
    auto cli = client();
    auto comps = cli.Get("/app/components");
    REQUIRE(comps);
    REQUIRE(comps->status == 200);
    REQUIRE(comps->body.find("c1") != std::string::npos);

    auto props = cli.Get("/app/components/c1/properties");
    REQUIRE(props);
    REQUIRE(props->status == 200);
    auto state = json::parse(props->body);
    REQUIRE(state["gain"] == 1.0);
    REQUIRE(state.contains("channels"));
    REQUIRE(state["net"]["host"] == "localhost");
}

// Batch PATCH /app/components must NOT mask a per-component failure behind a later success
// (the old handler overwrote the response each iteration). It aggregates outcomes and returns
// 207 Multi-Status when any component fails.
TEST_CASE_METHOD(rest_fixture, "PATCH components (batch) surfaces failures, no masking", "[http]") {
    auto cli = client();

    // All-success batch -> 200.
    auto ok =
        cli.Patch("/app/components", R"({"components":[{"id":"c1","properties":{"gain":2.0}}]})", "application/json");
    REQUIRE(ok);
    REQUIRE(ok->status == 200);
    REQUIRE(json::parse(cli.Get("/app/components/c1/properties")->body)["gain"] == 2.0);

    // A NOT-FOUND component FOLLOWED BY a valid one. Old code returned the trailing 200,
    // masking the 404; now the 404 must surface as 207 and the valid one still applies.
    auto mixed =
        cli.Patch("/app/components",
                  R"({"components":[{"id":"ghost","properties":{"gain":2.0}},{"id":"c1","properties":{"gain":3.0}}]})",
                  "application/json");
    REQUIRE(mixed);
    REQUIRE(mixed->status == 207); // NOT 200 — failure is not masked
    auto body = json::parse(mixed->body);
    REQUIRE(body["failed"] == 1);
    REQUIRE(body["applied"] == 1);
    bool ghost_404 = false;
    bool c1_ok = false;
    for (const auto& r : body["results"]) {
        if (r.value("id", std::string{}) == "ghost") {
            ghost_404 = (r["status"] == 404);
        }
        if (r.value("id", std::string{}) == "c1") {
            c1_ok = (r["status"] == 200);
        }
    }
    REQUIRE(ghost_404);
    REQUIRE(c1_ok);
    // Best-effort: the valid component DID apply (committed live).
    REQUIRE(json::parse(cli.Get("/app/components/c1/properties")->body)["gain"] == 3.0);

    // A rejected value reports the component as failed (207) and per-component atomicity holds
    // (the bad value did not apply).
    auto badval =
        cli.Patch("/app/components", R"({"components":[{"id":"c1","properties":{"gain":-5.0}}]})", "application/json");
    REQUIRE(badval);
    REQUIRE(badval->status == 207);
    REQUIRE(json::parse(badval->body)["failed"] == 1);
    REQUIRE(json::parse(cli.Get("/app/components/c1/properties")->body)["gain"] == 3.0);

    // THE masking scenario: a VALIDATION failure (c2) FOLLOWED BY a success (c1). The old
    // handler caught c2's error, then overwrote the response with c1's 200 — reporting overall
    // success and hiding c2's failure. Now it must be 207 with c2 flagged failed and c1 applied.
    auto mask =
        cli.Patch("/app/components",
                  R"({"components":[{"id":"c2","properties":{"gain":-1.0}},{"id":"c1","properties":{"gain":5.0}}]})",
                  "application/json");
    REQUIRE(mask);
    REQUIRE(mask->status == 207); // old code would have returned 200 here
    auto mb = json::parse(mask->body);
    REQUIRE(mb["failed"] == 1);
    REQUIRE(mb["applied"] == 1);
    REQUIRE(json::parse(cli.Get("/app/components/c1/properties")->body)["gain"] == 5.0); // applied
    REQUIRE(json::parse(cli.Get("/app/components/c2/properties")->body)["gain"] == 1.0); // rejected -> default
}

TEST_CASE_METHOD(rest_fixture, "PATCH component sets a scalar", "[http]") {
    auto cli = client();
    auto r = cli.Patch("/app/components/c1", R"({"properties": {"gain": 2.5}})", "application/json");
    REQUIRE(r);
    REQUIRE(r->status == 200);
    auto state = json::parse(cli.Get("/app/components/c1/properties")->body);
    REQUIRE(state["gain"] == 2.5);
}

TEST_CASE_METHOD(rest_fixture, "PATCH single property", "[http]") {
    auto cli = client();
    auto r = cli.Patch("/app/components/c1/properties/gain", R"({"value": 3.0})", "application/json");
    REQUIRE(r);
    REQUIRE(r->status == 200);
    auto one = json::parse(cli.Get("/app/components/c1/properties/gain")->body);
    REQUIRE(one["gain"] == 3.0);
}

// PUT was withdrawn from the single-property endpoint (see server.cpp): it was an alias for the
// merge handler, which contradicts HTTP PUT-as-replace. It is answered with an explicit 405 +
// Allow rather than a bare 404, so a pre-0.5 client gets a usable migration signal. Pin both the
// status and the no-op so PUT cannot be reintroduced as an alias by accident — if it returns, it
// must return as a real replace with its own test, not as a second name for PATCH.
TEST_CASE_METHOD(rest_fixture, "PUT single property is rejected with 405, not silently merged", "[http]") {
    auto cli = client();
    const auto before = json::parse(cli.Get("/app/components/c1/properties/gain")->body)["gain"];
    auto r = cli.Put("/app/components/c1/properties/gain", R"({"value": 3.0})", "application/json");
    REQUIRE(r);
    REQUIRE(r->status == 405);
    REQUIRE(r->get_header_value("Allow") == "GET, PATCH, DELETE");
    const auto after = json::parse(cli.Get("/app/components/c1/properties/gain")->body)["gain"];
    REQUIRE(after == before); // the rejected PUT applied nothing
    REQUIRE(after != 3.0);
}

TEST_CASE_METHOD(rest_fixture, "validation -> 400, config -> 403", "[http]") {
    auto cli = client();
    auto bad = cli.Patch("/app/components/c1", R"({"properties": {"gain": -1.0}})", "application/json");
    REQUIRE(bad);
    REQUIRE(bad->status == 400);
    // unchanged
    REQUIRE(json::parse(cli.Get("/app/components/c1/properties")->body)["gain"] == 1.0);

    auto init = cli.Patch("/app/components/c1", R"({"properties": {"buf_size": 4096}})", "application/json");
    REQUIRE(init);
    REQUIRE(init->status == 403); // INITIALIZE-only at runtime
}

TEST_CASE_METHOD(rest_fixture, "nested struct + keyed collection via JSON", "[http]") {
    auto cli = client();
    // nested 7396 merge
    REQUIRE(cli.Patch("/app/components/c1", R"({"properties": {"net": {"port": 9000}}})", "application/json")->status ==
            200);
    auto st = json::parse(cli.Get("/app/components/c1/properties")->body);
    REQUIRE(st["net"]["port"] == 9000);
    REQUIRE(st["net"]["host"] == "localhost"); // preserved

    // keyed add
    REQUIRE(cli.Patch("/app/components/c1", R"({"properties": {"channels": {"L": {"bw": 10e6, "cf": 1e9}}}})",
                      "application/json")
                ->status == 200);
    st = json::parse(cli.Get("/app/components/c1/properties")->body);
    REQUIRE(st["channels"].contains("L"));
    REQUIRE(st["channels"]["L"]["bw"] == 10e6);

    // keyed remove via nested null
    REQUIRE(
        cli.Patch("/app/components/c1", R"({"properties": {"channels": {"L": null}}})", "application/json")->status ==
        200);
    st = json::parse(cli.Get("/app/components/c1/properties")->body);
    REQUIRE(st["channels"].empty());
}

TEST_CASE_METHOD(rest_fixture, "DELETE resets a property to default", "[http]") {
    auto cli = client();
    cli.Patch("/app/components/c1", R"({"properties": {"net": {"port": 7777}}})", "application/json");
    REQUIRE(json::parse(cli.Get("/app/components/c1/properties")->body)["net"]["port"] == 7777);
    auto del = cli.Delete("/app/components/c1/properties/net");
    REQUIRE(del);
    REQUIRE(del->status == 200);
    REQUIRE(json::parse(cli.Get("/app/components/c1/properties")->body)["net"]["port"] == 8080); // default
}

// GET /app/components/:id/schema returns ONE JSON Schema 2020-12 document describing the
// component's properties, incl. the `enabled` virtual. This is the published wire contract:
// a generic validator or form generator must be able to consume it without composite-specific
// knowledge, so the structural keywords are standard and composite metadata is x-prefixed.
TEST_CASE_METHOD(rest_fixture, "GET component schema is a JSON Schema 2020-12 document", "[http]") {
    auto cli = client();
    auto r = cli.Get("/app/components/c1/schema");
    REQUIRE(r);
    REQUIRE(r->status == 200);
    auto schema = json::parse(r->body);

    REQUIRE(schema.is_object());
    REQUIRE(schema["$schema"] == "https://json-schema.org/draft/2020-12/schema");
    REQUIRE(schema["type"] == "object");
    REQUIRE(schema["additionalProperties"] == false);
    REQUIRE(schema["title"] == "c1");
    REQUIRE(!schema.contains("required")); // partial PATCH bodies must validate

    const auto& props = schema.at("properties");
    REQUIRE(props.contains("gain"));    // a registered property
    REQUIRE(props.contains("enabled")); // the spec/status virtual is advertised for UIs
    REQUIRE(props["gain"]["x-composite-configurability"] == "runtime");

    // None of the internal vocabulary may leak into the published document.
    for (const auto& [name, entry] : props.items()) {
        INFO("property: " << name);
        REQUIRE(!entry.contains("fields"));
        REQUIRE(!entry.contains("choices"));
        REQUIRE(!entry.contains("unit"));
        REQUIRE(!entry.contains("powerOfTwo"));
        REQUIRE(!entry.contains("configurability"));
        REQUIRE(!entry.contains("name")); // the name is the key
    }

    // Unknown component -> 404.
    REQUIRE(cli.Get("/app/components/nope/schema")->status == 404);
}

// DELETE /app/components/:id stops + unloads the component; it then 404s and drops from the list.
TEST_CASE_METHOD(rest_fixture, "DELETE component removes it", "[http]") {
    auto cli = client();
    REQUIRE(cli.Get("/app/components/c2")->status == 200); // present to start
    auto del = cli.Delete("/app/components/c2");
    REQUIRE(del);
    REQUIRE(del->status == 200);
    REQUIRE(cli.Get("/app/components/c2")->status == 404); // gone
    REQUIRE(cli.Get("/app/components/c1")->status == 200); // sibling untouched
    auto comps = json::parse(cli.Get("/app/components")->body);
    REQUIRE(comps.size() == 1);
    // Deleting an unknown component -> 404.
    REQUIRE(cli.Delete("/app/components/nope")->status == 404);
}

// DELETE is connection-safe: removing a connected consumer leaves the producer intact.
TEST_CASE_METHOD(rest_fixture, "DELETE a connected component is safe", "[http]") {
    auto cli = client();
    // Wire c1:data_out -> c2:data_in via REST, then delete the consumer c2.
    auto conn =
        cli.Post("/app/connections",
                 R"({"output":{"component":"c1","port":"data_out"},"input":{"component":"c2","port":"data_in"}})",
                 "application/json");
    REQUIRE(conn);
    if (conn->status == 201) { // only if rest_component actually exposes those ports
        REQUIRE(cli.Delete("/app/components/c2")->status == 200);
        REQUIRE(cli.Get("/app/components/c2")->status == 404);
        REQUIRE(cli.Get("/app/components/c1")->status == 200); // producer survives, no dangling edge
    }
}

// POST /app/start and /app/stop drive lifecycle across all components.
TEST_CASE_METHOD(rest_fixture, "POST app start/stop", "[http]") {
    auto cli = client();
    auto stop = cli.Post("/app/stop");
    REQUIRE(stop);
    REQUIRE(stop->status == 200);
    auto start = cli.Post("/app/start");
    REQUIRE(start);
    REQUIRE(start->status == 200);
}

// GET /app/openapi.json serves a well-formed OpenAPI 3.1 doc whose path+method set exactly
// matches the expected REST surface — a drift guard so a route added/removed without updating
// rest_catalog() (or this list) fails CI.
TEST_CASE_METHOD(rest_fixture, "OpenAPI spec is well-formed and matches the route surface", "[http]") {
    auto cli = client();
    auto r = cli.Get("/app/openapi.json");
    REQUIRE(r);
    REQUIRE(r->status == 200);
    auto spec = json::parse(r->body);
    REQUIRE(spec.at("openapi") == "3.1.0");
    REQUIRE(spec.at("info").contains("title"));
    REQUIRE(spec.at("info").contains("version"));
    REQUIRE(spec.at("paths").is_object());

    std::vector<std::string> actual;
    for (auto& [path, methods] : spec["paths"].items()) {
        for (auto& [method, op] : methods.items()) {
            actual.push_back(method + " " + path);
        }
    }
    std::vector<std::string> expected = {
        "get /app/healthz",
        "get /app/openapi.json",
        "get /app/metrics",
        "get /app/metrics/stream",
        "get /app",
        "post /app/start",
        "post /app/stop",
        "get /app/components",
        "post /app/components",
        "patch /app/components",
        "get /app/components/{id}",
        "delete /app/components/{id}",
        "patch /app/components/{id}",
        "get /app/components/{id}/schema",
        "get /app/components/{id}/properties",
        "get /app/components/{id}/properties/{name}",
        "patch /app/components/{id}/properties/{name}",
        "delete /app/components/{id}/properties/{name}",
        "get /app/components/{id}/ports",
        "get /app/components/{id}/ports/{port_name}",
        "delete /app/components/{id}/ports/{port_name}/connections",
        "post /app/connections",
        "delete /app/connections",
    };
    std::sort(actual.begin(), actual.end());
    std::sort(expected.begin(), expected.end());
    REQUIRE(actual == expected);
}

// The case above proves the OpenAPI document matches a list maintained in THIS TEST. It does
// not prove either matches what the server actually serves: a route registered in make_server()
// but missing from rest_catalog() satisfies both and stays invisible, and a catalogued route
// that was never registered would too. Probe every advertised route against the live server so
// the catalog cannot advertise something that does not exist.
//
// Probing uses a component id that does not exist, so mutating verbs are no-ops. The signal is
// the response BODY, not the status: every handler of ours answers with JSON (data or
// {"error": ...}), whereas a path httplib does not know falls through to its default 404 with
// an EMPTY body. A 404 is therefore only a failure when the body is empty.
TEST_CASE_METHOD(rest_fixture, "every advertised route is actually registered", "[http]") {
    auto cli = client();
    cli.set_read_timeout(5, 0);
    auto spec = json::parse(cli.Get("/app/openapi.json")->body);

    auto fill = [](std::string p) {
        for (const auto* tok : {"{id}", "{name}", "{port_name}"}) {
            const std::string t{tok};
            for (auto i = p.find(t); i != std::string::npos; i = p.find(t)) {
                p.replace(i, t.size(), "__probe_absent__");
            }
        }
        return p;
    };

    int probed = 0;
    for (auto& [path, methods] : spec["paths"].items()) {
        // Server-Sent Events: an open-ended stream, so a probe would block rather than answer.
        // Its registration is covered by the metrics-stream case elsewhere in this file.
        if (path == "/app/metrics/stream") {
            continue;
        }
        for (auto& [method, op] : methods.items()) {
            const std::string url = fill(path);
            INFO("probe: " << method << " " << url);
            httplib::Result r;
            if (method == "get") {
                r = cli.Get(url);
            } else if (method == "post") {
                r = cli.Post(url, "{}", "application/json");
            } else if (method == "patch") {
                r = cli.Patch(url, "{}", "application/json");
            } else if (method == "put") {
                r = cli.Put(url, "{}", "application/json");
            } else if (method == "delete") {
                r = cli.Delete(url);
            } else {
                FAIL("unhandled method in the OpenAPI document: " << method);
            }
            REQUIRE(r);
            const bool unregistered = (r->status == 404 && r->body.empty());
            REQUIRE(!unregistered);
            ++probed;
        }
    }
    // EXACT, not a floor. `> 15` let six routes silently disappear from the catalogue with only
    // the sorted-list case (a separate assertion, on a separate document) noticing — so the probe
    // itself, whose whole job is to prove each catalogued route is really registered, could quietly
    // stop covering a third of them.
    REQUIRE(probed == 22);
}

// Route-diff: every GET route the spec advertises must actually be REGISTERED on the server.
// An unregistered path returns httplib's default 404 with a non-JSON (empty) body; every real
// handler returns a JSON body (200 or {"error":...}). So "body parses as JSON" == "route exists".
TEST_CASE_METHOD(rest_fixture, "every advertised GET route is registered", "[http]") {
    auto cli = client();
    auto spec = json::parse(cli.Get("/app/openapi.json")->body);
    for (auto& [path, methods] : spec["paths"].items()) {
        if (!methods.contains("get")) {
            continue;
        }
        if (path == "/app/metrics/stream") {
            continue;
        } // SSE stream: body is text/event-stream, not JSON
        std::string probe = path;
        for (auto ph : {"{id}", "{name}", "{port_name}"}) {
            std::size_t pos;
            while ((pos = probe.find(ph)) != std::string::npos) {
                probe.replace(pos, std::string(ph).size(), "c1");
            }
        }
        auto resp = cli.Get(probe);
        INFO("probing GET " << probe);
        REQUIRE(resp);
        bool is_json = false;
        try {
            auto parsed = json::parse(resp->body);
            (void)parsed;
            is_json = true;
        } // parsed used -> no warn_unused_result
        catch (...) {
            is_json = false;
        }
        REQUIRE(is_json); // a non-JSON body would mean the route is not registered
    }
}

// ===========================================================================
// Control-plane hardening
// ===========================================================================

namespace {
// A producer whose worker actively sends into its output every iteration — so a removal
// of its downstream consumer overlaps an in-flight send (the UAF window the topology lock closes).
class stress_producer : public component {
public:
    explicit stress_producer(std::string_view id) : component(id) { add_port(&m_out); }
    auto process() -> retval override {
        m_out.send_data(make_mutable<float>(8), timestamp{}, std::nullopt);
        return retval::NORMAL;
    }
    output_port<mutable_buffer<float>> m_out{"data_out"};
    component::auto_stop m_auto_stop{*this};
};
class stress_consumer : public component {
public:
    explicit stress_consumer(std::string_view id) : component(id) { add_port(&m_in); }
    auto process() -> retval override {
        auto [b, ts, md] = m_in.get_data();
        return b.empty() ? retval::NOOP : retval::NORMAL;
    }
    input_port<mutable_buffer<float>> m_in{"data_in"};
    component::auto_stop m_auto_stop{*this};
};
} // namespace

// remove_component must be race-free against a concurrent connect AND a still-running producer
// sending into the consumer being torn down. The topology lock serializes connect vs remove, and
// remove parks the producer before releasing the consumer's input claim — so no send hits a freed
// ring. Run under TSan/ASan to catch a regression of the HIGH finding this guards.
TEST_CASE("remove_component is race-free vs a live sender + concurrent connect", "[application][hardening]") {
    application app{"stress"};
    auto p = std::make_shared<stress_producer>("P");
    app.add_component(p);
    app.add_component(std::make_shared<stress_consumer>("C"));
    {
        auto topo = app.topology_lock();
        p->connect("data_out", app.get_component("C"), "data_in");
    }
    p->start(); // producer worker is now actively sending into C's input ring

    // Churn the consumer: remove it (tears down its ring while P sends) then re-add + reconnect.
    std::atomic<bool> go{false};
    std::thread churn([&] {
        go.store(true, std::memory_order_release);
        for (int i = 0; i < 150; ++i) {
            app.remove_component("C"); // parks P, disconnects, destroys old C
            auto c = std::make_shared<stress_consumer>("C");
            app.add_component(c);
            auto topo = app.topology_lock(); // serialize the reconnect vs any remove
            p->connect("data_out", c, "data_in");
        }
    });
    // A second thread races connects against the churn (the exact connect-vs-remove window).
    std::thread racer([&] {
        while (!go.load(std::memory_order_acquire)) {
        }
        for (int i = 0; i < 150; ++i) {
            auto topo = app.topology_lock();
            if (auto c = app.get_component("C")) {
                (void)p->connect("data_out", c, "data_in");
            }
        }
    });
    churn.join();
    racer.join();
    p->stop();
    SUCCEED(); // no crash and (under TSan/ASan) no data race / use-after-free
}

// The registry rejects a second component with an existing id atomically, so two
// concurrent POSTs of the same id cannot both win (no check-then-add TOCTOU).
TEST_CASE("application rejects duplicate component ids", "[application][hardening]") {
    application app{"a"};
    REQUIRE(app.add_component(std::make_shared<rest_component>("dup")));
    REQUIRE_FALSE(app.add_component(std::make_shared<rest_component>("dup")));
    REQUIRE_FALSE(app.add_component(nullptr));
    REQUIRE(app.add_component(std::make_shared<rest_component>("other")));
    REQUIRE(app.components().size() == 2);
}

TEST_CASE_METHOD(rest_fixture, "POST duplicate component id -> 409", "[http][hardening]") {
    auto cli = client();
    auto r = cli.Post("/app/components", R"({"id": "c1", "library": "anything"})", "application/json");
    REQUIRE(r);
    REQUIRE(r->status == 409);
}

TEST_CASE_METHOD(rest_fixture, "malformed JSON body -> 400 (no connection drop)", "[http][hardening]") {
    auto cli = client();
    auto r = cli.Patch("/app/components/c1", "{ this is not json", "application/json");
    REQUIRE(r); // connection served, not dropped
    REQUIRE(r->status == 400);
}

TEST_CASE_METHOD(rest_fixture, "PATCH unknown property -> 404", "[http][hardening]") {
    auto cli = client();
    auto r = cli.Patch("/app/components/c1/properties/does_not_exist", R"({"value": 1})", "application/json");
    REQUIRE(r);
    REQUIRE(r->status == 404);
}

TEST_CASE_METHOD(rest_fixture, "oversized payload -> 413", "[http][hardening]") {
    auto cli = client();
    // Exceed the 8 MiB cap; httplib rejects before invoking the handler.
    std::string big = R"({"properties": {"gain": )" + std::string(9 * 1024 * 1024, '1') + "}}";
    auto r = cli.Patch("/app/components/c1", big, "application/json");
    REQUIRE(r);
    REQUIRE(r->status == 413);
}

// Hammer property reads while a writer mutates the same component: the GET path
// takes the shared read lock (property_state) and the PATCH path parks the worker
// under the unique write lock, so reads stay consistent and nothing tears.
TEST_CASE_METHOD(rest_fixture, "concurrent property GET during PATCH", "[http][hardening]") {
    std::atomic<bool> stop{false};
    std::atomic<int> reads{0};
    std::atomic<int> bad{0};
    // NOTE: Catch2 assertion macros are NOT thread-safe — reader threads must only
    // touch atomics; all REQUIREs run on the main thread after join.
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i) {
        readers.emplace_back([&] {
            httplib::Client c("localhost", m_port);
            while (!stop.load()) {
                auto g = c.Get("/app/components/c1/properties");
                if (g && g->status == 200) {
                    auto st = json::parse(g->body); // GET must always return well-formed state
                    if (st.contains("gain")) {
                        reads.fetch_add(1);
                    } else {
                        bad.fetch_add(1);
                    }
                } else {
                    bad.fetch_add(1);
                }
            }
        });
    }
    auto cli = client();
    for (int i = 1; i <= 50; ++i) {
        auto body = std::string(R"({"properties": {"gain": )") + std::to_string(i) + "}}";
        REQUIRE(cli.Patch("/app/components/c1", body, "application/json")->status == 200);
    }
    stop = true;
    for (auto& t : readers) {
        t.join();
    }
    REQUIRE(bad.load() == 0); // every concurrent GET returned consistent, well-formed state
    REQUIRE(reads.load() > 0);
    REQUIRE(json::parse(cli.Get("/app/components/c1/properties")->body)["gain"] == 50.0);
}

// The SSE stream cap (8) bounds concurrent streams; a 9th is rejected with 503,
// and a slot frees once a stream ends (RAII release).
TEST_CASE_METHOD(rest_fixture, "SSE metric stream cap -> 503, then frees", "[http][hardening]") {
    constexpr int cap = 8;
    std::atomic<int> established{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> streams;

    for (int i = 0; i < cap; ++i) {
        streams.emplace_back([&] {
            httplib::Client c("localhost", m_port);
            c.set_read_timeout(10, 0);
            bool counted = false;
            c.Get("/app/metrics/stream?interval=100", [&](const char* /*data*/, size_t /*len*/) -> bool {
                if (!counted) {
                    counted = true;
                    established.fetch_add(1);
                }
                while (!stop.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                return false; // end this stream
            });
        });
    }

    // Wait until all cap streams have produced data (slots are held).
    for (int i = 0; i < 500 && established.load() < cap; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(established.load() == cap);

    // The (cap+1)-th stream is rejected immediately with 503.
    {
        httplib::Client c("localhost", m_port);
        c.set_read_timeout(5, 0);
        auto r = c.Get("/app/metrics/stream?interval=100");
        REQUIRE(r);
        REQUIRE(r->status == 503);
    }

    // Release all held streams; the server frees each slot when it next notices
    // the closed connection (within ~one interval). Poll until a new stream is
    // admitted again, proving the RAII slot release works.
    stop = true;
    for (auto& t : streams) {
        t.join();
    }

    bool admitted_again = false;
    for (int attempt = 0; attempt < 30 && !admitted_again; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        httplib::Client c("localhost", m_port);
        c.set_read_timeout(2, 0);
        bool got_chunk = false;
        c.Get("/app/metrics/stream?interval=100", [&](const char*, size_t) -> bool {
            got_chunk = true;
            return false;
        });
        admitted_again = got_chunk;
    }
    REQUIRE(admitted_again);
}
