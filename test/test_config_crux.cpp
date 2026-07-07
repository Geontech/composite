// config<T> property_set integration: a config<T>'s fields are
// projected onto the property_set top level (wire contract unchanged), but the whole
// struct is the validate/commit unit. Proves: encode/describe flatten fields to top
// level; a single-field PATCH validates the WHOLE struct and commits once with a
// scoped on_apply; a batch mixing config fields AND a plain add() property works and
// is atomic (a rejection mutates nothing); cross-field invariants hold on every
// write; single-name apply (PUT .../properties/:name) routes to the field; get<T>
// reads a field. Own main(); explicit checks.
#include <composite/properties/property_set.hpp>

#include <cstdint>
#include <cstdio>
#include <string>

namespace cp = composite::properties;
using composite::changes;
using composite::config;
using cp::config_type;
using cp::json;
using cp::property_set;

struct net_cfg {
    std::string host{"0.0.0.0"};
    std::uint16_t port{5000};
    COMPOSITE_FIELDS(net_cfg, (host, runtime), (port, runtime, range(1, 65535)));
};

// a second config with DISJOINT field names, to test cross-config reaction ordering.
struct proc_cfg {
    double rate{1.0};
    COMPOSITE_FIELDS(proc_cfg, (rate, runtime, range(0.0, 1000)));
};

static int g_fails = 0;
static void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_fails;
    }
}

int main() {
    property_set ps;
    config<net_cfg> cfg;
    double rate = 1.0;

    int on_apply_fires = 0;
    net_cfg seen_prev{};
    bool host_ch = false;
    bool port_ch = false;
    cfg.on_apply([&](const net_cfg& prev, const changes<net_cfg>& ch) {
        ++on_apply_fires;
        seen_prev = prev;
        host_ch = ch.changed(&net_cfg::host);
        port_ch = ch.changed(&net_cfg::port);
    });
    // whole-struct (cross-field) invariant: a non-loopback host requires port >= 1024.
    cfg.validate([](const net_cfg& c) { return c.host == "127.0.0.1" || c.port >= 1024; },
                 "a non-loopback host requires port >= 1024");

    ps.add_config(cfg, config_type::INITIALIZE); // host/port -> top-level (runtime attr)
    ps.add("rate", rate, config_type::RUNTIME).validate([](const double& r) { return r > 0.0; });

    // ---- encode flattens config fields next to the plain property ----
    {
        const json st = ps.encode();
        check(st["host"] == "0.0.0.0" && st["port"] == 5000 && st["rate"] == 1.0,
              "encode shows config fields AND plain prop at top level");
        check(ps.contains("port") && ps.contains("rate"), "contains() sees fields and plain props");
        check(ps.get<std::uint16_t>("port") == 5000, "get<T> reads a config field");
    }

    // ---- single-field PATCH: whole-struct validate, one commit, scoped reaction ----
    {
        const json d = ps.apply(json::parse(R"({"port": 8080})"), config_type::RUNTIME);
        check(cfg->port == 8080, "single-field patch updates the struct (value synchronous)");
        check(on_apply_fires == 0, "B2: apply stages the reaction; it has not run yet");
        ps.run_pending_reactions(); // worker loop-top / inline drain
        check(on_apply_fires == 1 && port_ch && !host_ch, "on_apply ran once on drain, scoped to port");
        check(seen_prev.port == 5000, "on_apply prev is the pre-change value");
        check(d.contains("port") && !d.contains("host") && !d.contains("rate"),
              "aggregate diff is field-flat and scoped");
    }

    // ---- batch mixing config fields + a plain property in ONE apply ----
    {
        on_apply_fires = 0;
        const json d =
            ps.apply(json::parse(R"({"host": "10.0.0.1", "port": 9000, "rate": 2.0})"), config_type::RUNTIME);
        check(cfg->host == "10.0.0.1" && cfg->port == 9000 && rate == 2.0, "mixed batch applied");
        ps.run_pending_reactions();
        check(on_apply_fires == 1, "one config reaction for the whole batch (host+port in one struct diff)");
        check(d.contains("host") && d.contains("port") && d.contains("rate"), "diff carries all three at top level");
    }

    // ---- cross-field invariant enforced on a single-field write; nothing mutates ----
    {
        bool threw = false;
        try {
            ps.apply(json::parse(R"({"port": 500})"), config_type::RUNTIME);
        } // host=10.0.0.1, 500 < 1024
        catch (const cp::validation_error& e) {
            threw = true;
            check(std::string(e.what()).find("non-loopback host requires port") != std::string::npos,
                  "cross-field reason surfaced");
        }
        check(threw && cfg->port == 9000, "whole-struct invariant rejects single-field write; struct intact");
    }

    // ---- atomic batch: one bad value rejects the whole batch, nothing commits ----
    {
        bool threw = false;
        try {
            ps.apply(json::parse(R"({"port": 7000, "rate": -1.0})"), config_type::RUNTIME);
        } catch (const cp::validation_error&) {
            threw = true;
        }
        check(threw && cfg->port == 9000 && rate == 2.0, "rejection in a mixed batch mutates nothing");
    }

    // ---- single-name apply (the PUT .../properties/:name route) routes to the field ----
    {
        const bool changed = ps.apply("port", json(1234), config_type::RUNTIME); // host non-loopback, 1234 >= 1024
        check(changed && cfg->port == 1234, "single-name apply routes to the config field + commits the struct");
        const bool noop = ps.apply("port", json(1234), config_type::RUNTIME);
        check(!noop, "single-name apply reports no change on a no-op");
    }

    // ---- unknown top-level key is still rejected ----
    {
        bool threw = false;
        try {
            ps.apply(json::parse(R"({"prt": 1})"), config_type::RUNTIME);
        } catch (const cp::unknown_property&) {
            threw = true;
        }
        check(threw, "unknown top-level key rejected");
    }

    // ---- describe emits a per-field entry for each field + the plain prop ----
    {
        const json sch = ps.describe();
        check(sch.size() == 3, "describe has 3 entries (host, port, rate)");
        // entries carry name + configurability; the port entry carries its range.
        bool found_port = false;
        for (const auto& e : sch) {
            if (e["name"] == "port") {
                found_port = true;
                check(e["minimum"] == 1.0 && e["maximum"] == 65535.0, "port describe carries range");
                check(e["configurability"] == "runtime", "port is runtime");
            }
        }
        check(found_port, "port appears in describe at top level");
    }

    // ---- duplicate field/property name is a loud registration error ----
    {
        property_set ps2;
        config<net_cfg> cfg2;
        ps2.add_config(cfg2);
        double host_clash = 0;
        bool threw = false;
        try {
            ps2.add("host", host_clash);
        } // collides with config field "host"
        catch (const std::logic_error&) {
            threw = true;
        }
        check(threw, "a plain property colliding with a config field name throws");
    }

    // ---- two config<T> in one set: reactions fire in REGISTRATION order, not pointer order ----
    {
        property_set ps2;
        config<net_cfg> a;
        config<proc_cfg> b;
        std::string order;
        a.on_apply([&](const net_cfg&, const changes<net_cfg>&) { order += "A"; });
        b.on_apply([&](const proc_cfg&, const changes<proc_cfg>&) { order += "B"; });
        ps2.add_config(a); // registered first
        ps2.add_config(b); // registered second
        // one batch touching a field of each (disjoint names: port -> A, rate -> B)
        ps2.apply(json::parse(R"({"rate": 2.0, "port": 8080})"), config_type::RUNTIME);
        ps2.run_pending_reactions(); // drain both staged reactions
        check(order == "AB", "config reactions fire in registration order (A before B), independent of pointers");
    }

    if (g_fails != 0) {
        std::fprintf(stderr, "%d config-crux check(s) FAILED\n", g_fails);
        return 1;
    }
    std::puts("CONFIG<T> CRUX (property_set integration) TESTS PASSED");
    return 0;
}
