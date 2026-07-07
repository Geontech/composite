// Strict-decode tests for reflect.hpp: an unknown field at any
// depth, an out-of-range / fractional / negative-into-unsigned integer, a
// type mismatch, a non-object where a struct is expected, and a bad enum are
// each rejected with a precise JSON path — and multiple problems in one patch
// are accumulated, not reported one-at-a-time. Also: json leaves merge_patch
// (deep-merge) rather than replace, and valid input still round-trips. Own
// main(); explicit checks (NDEBUG-safe).
#include <composite/properties/reflect.hpp>

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace rfl = composite::reflect;
using rfl::json;

enum class Window { hann, hamming };
COMPOSITE_ENUM(Window, hann, hamming);

struct Net {
    std::string host;
    std::uint16_t port{};
};
COMPOSITE_STRUCT(Net, host, port);

struct Cfg {
    std::uint16_t channels{1};
    std::int32_t offset{};
    double gain{1.0};
    bool enabled{true};
    Window window{Window::hann};
    Net net;
    std::optional<double> bandwidth;
    std::vector<std::uint16_t> taps;
    json meta; // opaque leaf
};
COMPOSITE_STRUCT(Cfg, channels, offset, gain, enabled, window, net, bandwidth, taps, meta);

// Nested struct with non-zero member initializers, to prove a nested-field null
// resets to the initializer (not the zero value).
struct Inner {
    int delay{50};
    double scale{2.0};
};
COMPOSITE_STRUCT(Inner, delay, scale);
struct Outer {
    Inner sub;
    int top{7};
};
COMPOSITE_STRUCT(Outer, sub, top);

static int g_fails = 0;
static void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_fails;
    }
}

// Run merge(patch) on a fresh Cfg; assert it throws and that SOME accumulated
// error has the given path and a message containing `substr`. Returns the error
// count so callers can assert accumulation.
static std::size_t expect_fail(const char* patch_json, const char* path, const char* substr, const char* what) {
    Cfg c;
    try {
        rfl::merge(c, json::parse(patch_json));
    } catch (const rfl::decode_failure& e) {
        bool found = false;
        for (const auto& err : e.errors) {
            if (err.path == path && err.message.find(substr) != std::string::npos) {
                found = true;
            }
        }
        check(found, what);
        return e.errors.size();
    } catch (...) {
        check(false, what); // wrong exception type
        return 0;
    }
    check(false, what); // did not throw at all
    return 0;
}

int main() {
    // ---- type / range / domain rejections, each with a precise path ----
    expect_fail(R"({"channels": 70000})", "channels", "out of range", "uint16 overflow rejected");
    expect_fail(R"({"channels": 70000})", "channels", "[0, 65535]", "overflow message names the range");
    expect_fail(R"({"channels": -1})", "channels", "non-negative", "negative into unsigned rejected");
    expect_fail(R"({"offset": 2.5})", "offset", "fractional", "fractional into integer rejected");
    expect_fail(R"({"gain": "loud"})", "gain", "expected a number", "string for number rejected");
    expect_fail(R"({"enabled": 1})", "enabled", "expected a boolean", "number for bool rejected");
    expect_fail(R"({"window": "bartlett"})", "window", "hann, hamming", "bad enum lists the choices");
    expect_fail(R"({"net": 5})", "net", "expected an object", "non-object into struct rejected");
    expect_fail(R"({"taps": 5})", "taps", "expected an array", "non-array into vector rejected");
    expect_fail(R"({"taps": [1, 70000, 3]})", "taps[1]", "out of range", "vector element path is indexed");
    // an integer literal above uint64 range is parsed as a float by nlohmann; it must
    // still be reported as out-of-range, not "fractional".
    expect_fail(R"({"channels": 18446744073709551616})", "channels", "out of range",
                "above-uint64 integer literal reported as out-of-range, not fractional");

    // ---- unknown fields, with did-you-mean, at top level and nested ----
    expect_fail(R"({"channel": 2})", "channel", "unknown field", "unknown top-level field rejected");
    expect_fail(R"({"channel": 2})", "channel", "did you mean 'channels'", "did-you-mean suggests the near name");
    expect_fail(R"({"net": {"prt": 5}})", "net.prt", "unknown field", "unknown nested field path is dotted");
    expect_fail(R"({"net": {"prt": 5}})", "net.prt", "did you mean 'port'", "nested did-you-mean");

    // ---- error accumulation: one patch, several problems, all reported ----
    {
        const std::size_t n = expect_fail(R"({"channels": 70000, "gain": "x", "bogus": 1})", "channels", "out of range",
                                          "accumulation: channels still reported");
        check(n == 3, "all three problems accumulated into one failure");
    }

    // ---- json opaque leaf is REPLACED wholesale (so diff/merge round-trips exactly,
    //      and a nested json leaf behaves like a top-level json property) ----
    {
        Cfg c;
        c.meta = json::parse(R"({"a": 1, "b": 2})");
        rfl::merge(c, json::parse(R"({"meta": {"b": 3, "c": 4}})"));
        check(c.meta == json::parse(R"({"b": 3, "c": 4})"), "json leaf is replaced wholesale (not deep-merged)");

        // the documented invariant merge(a, diff(a,b)) == b must hold even when b
        // removes a key from an opaque json leaf (the merge_patch form broke this).
        Cfg av;
        av.meta = json::parse(R"({"x": 1, "y": 2})");
        Cfg bv;
        bv.meta = json::parse(R"({"x": 1})"); // dropped "y"
        Cfg a_copy = av;
        rfl::merge(a_copy, rfl::diff(av, bv));
        check(rfl::equal(a_copy, bv), "json leaf diff->merge round-trips (removed key really dropped)");
    }

    // ---- a nested-field null resets to the member initializer, not zero ----
    {
        Outer o;
        rfl::merge(o, json::parse(R"({"sub": {"delay": 999}})"));
        check(o.sub.delay == 999, "nested scalar set");
        rfl::merge(o, json::parse(R"({"sub": {"delay": null}})"));
        check(o.sub.delay == 50, "nested scalar null resets to its member initializer (not 0)");
        check(o.sub.scale == 2.0 && o.top == 7, "siblings untouched by the nested reset");
    }

    // ---- valid input still applies cleanly (no false rejection) ----
    {
        Cfg c;
        c.net.host = "10.0.0.1";
        rfl::merge(c, json::parse(R"({"channels": 8, "net": {"port": 5005}, "window": "hamming"})"));
        check(c.channels == 8, "valid uint16 applied");
        check(c.net.port == 5005 && c.net.host == "10.0.0.1", "nested merge preserves siblings");
        check(c.window == Window::hamming, "enum by name applied");
        rfl::merge(c, json::parse(R"({"bandwidth": 2.0e6})"));
        check(c.bandwidth.has_value() && *c.bandwidth == 2.0e6, "optional set");
        rfl::merge(c, json::parse(R"({"bandwidth": null})"));
        check(!c.bandwidth.has_value(), "optional reset by null");
        rfl::merge(c, json::parse(R"({"taps": [1, 2, 3]})"));
        check(c.taps.size() == 3 && c.taps[2] == 3, "vector replace");
    }

    if (g_fails != 0) {
        std::fprintf(stderr, "%d strict-decode check(s) FAILED\n", g_fails);
        return 1;
    }
    std::puts("REFLECT STRICT-DECODE TESTS PASSED");
    return 0;
}
