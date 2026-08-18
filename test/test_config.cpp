// config<T> + config_binding: the struct-as-commit-unit facade over a
// single typed_property<T>. Drives the binding directly (property_set integration is
// tested separately). Proves: hot-path operator->; whole-struct validate/commit from
// a single-field patch; on_apply receives the correct prev + scoped changes<T>;
// attribute-synthesized validators (range / power_of_two / one_of); per-field
// configurability (config_violation); and the 2020-12 schema. Own main(); explicit checks.
#include <composite/properties/config.hpp>

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

namespace cp = composite::properties;
using composite::changes;
using composite::config;
using composite::properties::config_type;
using composite::properties::config_violation;
using composite::properties::validation_error;
using cp::json;

enum class window { hann, hamming, blackman_harris };
COMPOSITE_ENUM(window, hann, hamming, blackman_harris);

struct filt {
    double gain{1.0};
    std::uint32_t taps{8};
    window win{window::hann};
    std::string mode{"fir"};
    COMPOSITE_FIELDS(filt, (gain, runtime, range(0.0, 10.0), unit("dB"), doc("post-mix gain")),
                     (taps, runtime, range(1u, 1024u), power_of_two), (win, runtime),
                     (mode, one_of("fir", "iir"))); // mode has NO runtime attr -> INITIALIZE-only
};

// nested reflected struct, to exercise changes<T>::new_value on a partial sub-diff,
// and a MIXED-literal range (double lo, int hi) to prove the two-type range() builder.
struct inner_t {
    int a{0};
    int b{7};
    COMPOSITE_FIELDS(inner_t, a, b);
};
struct outer_t {
    inner_t sub;
    double scale{1.0};
    COMPOSITE_FIELDS(outer_t, (sub, runtime), (scale, runtime, range(0.0, 10)));
};

// optional field, to exercise coalescing a JSON-null value (cleared optional).
struct opt_cfg {
    int a{0};
    std::optional<int> opt{};
    COMPOSITE_FIELDS(opt_cfg, (a, runtime), (opt, runtime));
};

static int g_fails = 0;
static void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_fails;
    }
}

int main() {
    // ---- config<T> hot-path read ----
    {
        config<filt> c;
        check(c->gain == 1.0 && c->taps == 8 && c->win == window::hann, "operator-> reads defaults");
        check((*c).mode == "fir", "operator* reads");
    }

    // ---- commit a single-field patch -> whole struct, scoped on_apply ----
    {
        config<filt> c;
        int fires = 0;
        filt captured_prev{};
        bool gain_ch = false;
        bool taps_ch = false;
        std::optional<double> new_gain;
        c.on_apply([&](const filt& prev, const changes<filt>& ch) {
            ++fires;
            captured_prev = prev;
            gain_ch = ch.changed(&filt::gain);
            taps_ch = ch.changed(&filt::taps);
            new_gain = ch.new_value(&filt::gain);
        });
        cp::config_binding<filt> b{c, config_type::INITIALIZE};

        b.prepare(json{{"gain", 2.5}}, config_type::RUNTIME);
        const json d = b.commit();
        check(c->gain == 2.5, "value swapped synchronously at commit (read-your-writes)");
        b.notify(d);
        check(fires == 0, "notify STAGES the reaction, does not run it");
        b.run_pending(); // worker loop-top (or inline) drains + runs it
        check(fires == 1, "reaction runs on drain");
        check(gain_ch && !taps_ch, "changes<T> scoped: only gain changed");
        check(captured_prev.gain == 1.0, "on_apply prev is the pre-change value");
        check(new_gain.has_value() && *new_gain == 2.5, "changes<T>::new_value");
        b.run_pending();
        check(fires == 1, "draining again does not re-run (mailbox cleared)");

        // no-op commit -> null diff, nothing staged, no reaction
        b.prepare(json{{"gain", 2.5}}, config_type::RUNTIME);
        const json d2 = b.commit();
        check(d2.is_null(), "no-op commit yields null diff");
        b.notify(d2);
        b.run_pending();
        check(fires == 1, "no-op stages/fires no reaction");
    }

    // ---- attribute-synthesized validators reject bad values; struct untouched ----
    {
        config<filt> c;
        cp::config_binding<filt> b{c, config_type::INITIALIZE};
        bool threw = false;
        try {
            b.prepare(json{{"gain", 99.0}}, config_type::RUNTIME);
        } catch (const validation_error& e) {
            threw = true;
            check(std::string(e.what()).find("out of range") != std::string::npos, "range reason surfaced");
        }
        check(threw && c->gain == 1.0, "range validator rejects, struct intact");

        threw = false;
        try {
            b.prepare(json{{"taps", 7}}, config_type::RUNTIME);
        } // 7 is not a power of two
        catch (const validation_error& e) {
            threw = true;
            check(std::string(e.what()).find("power of two") != std::string::npos, "pow2 reason");
        }
        check(threw, "power_of_two validator rejects 7");
        b.prepare(json{{"taps", 16}}, config_type::RUNTIME);
        b.notify(b.commit());
        check(c->taps == 16, "power_of_two accepts 16");

        threw = false;
        try {
            b.prepare(json{{"mode", "wavelet"}}, config_type::INITIALIZE);
        } // not in one_of
        catch (const validation_error&) {
            threw = true;
        }
        check(threw && c->mode == "fir", "one_of rejects a value outside the set");
    }

    // ---- per-field configurability: a RUNTIME patch can't touch an INITIALIZE field ----
    {
        config<filt> c;
        cp::config_binding<filt> b{c, config_type::INITIALIZE}; // default RUNTIME, but mode has no runtime attr
        bool threw = false;
        try {
            b.prepare(json{{"mode", "iir"}}, config_type::RUNTIME);
        } catch (const config_violation& e) {
            threw = true;
            check(e.name == "mode", "violation names the field");
        }
        check(threw && c->mode == "fir", "RUNTIME write to INITIALIZE-only field rejected");
        // INITIALIZE context may set it
        b.prepare(json{{"mode", "iir"}}, config_type::INITIALIZE);
        b.notify(b.commit());
        check(c->mode == "iir", "INITIALIZE context may set an initialize field");
    }

    // ---- nested-field null-reset via the binding default still works (Phase 0 path) ----
    {
        config<filt> c;
        cp::config_binding<filt> b{c, config_type::INITIALIZE};
        b.prepare(json{{"gain", 5.0}}, config_type::RUNTIME);
        b.notify(b.commit());
        check(c->gain == 5.0, "gain set to 5");
        b.prepare(json{{"gain", nullptr}}, config_type::RUNTIME);
        b.notify(b.commit());
        check(c->gain == 1.0, "null resets gain to its registered default (1.0)");
    }

    // ---- 2020-12 schema ----
    {
        config<filt> c;
        const json s = cp::schema_2020_12(c);
        check(s["$schema"] == "https://json-schema.org/draft/2020-12/schema", "schema dialect");
        check(s["type"] == "object" && s["additionalProperties"] == false, "object, closed");
        check(s["properties"]["gain"]["minimum"] == 0.0 && s["properties"]["gain"]["maximum"] == 10.0,
              "gain range -> min/max");
        check(s["properties"]["gain"]["x-composite-unit"] == "dB", "unit annotation");
        check(s["properties"]["taps"]["x-composite-powerOfTwo"] == true, "power_of_two annotation");
        check(s["properties"]["win"]["enum"] == json::parse(R"(["hann","hamming","blackman_harris"])"), "enum choices");
        check(s["properties"]["mode"]["enum"] == json::parse(R"(["fir","iir"])"), "one_of -> enum");
        check(s["properties"]["gain"]["default"] == 1.0, "default captured");
    }

    // ---- changes<T>::new_value on a NESTED struct returns the full committed value ----
    // (a partial sub-diff {sub:{a:5}} must NOT be decoded into a default inner_t, which
    //  would drop sub.b; new_value reads the live committed struct instead).
    {
        config<outer_t> c;
        cp::config_binding<outer_t> b{c, config_type::INITIALIZE};
        b.prepare(json::parse(R"({"sub": {"a": 1, "b": 99}})"), config_type::INITIALIZE);
        b.notify(b.commit());
        check(c->sub.a == 1 && c->sub.b == 99, "nested struct set at init");

        std::optional<inner_t> captured;
        bool sub_changed = false;
        c.on_apply([&](const outer_t& prev, const changes<outer_t>& ch) {
            (void)prev;
            sub_changed = ch.changed(&outer_t::sub);
            captured = ch.new_value(&outer_t::sub);
        });
        b.prepare(json::parse(R"({"sub": {"a": 5}})"), config_type::RUNTIME); // partial sub-patch
        b.notify(b.commit());
        b.run_pending(); // drain the staged reaction (worker loop-top / inline)
        check(c->sub.a == 5 && c->sub.b == 99, "nested partial merge keeps unchanged sub-field b");
        check(sub_changed, "changes<T>::changed true for the nested struct");
        check(captured.has_value() && captured->a == 5 && captured->b == 99,
              "new_value(nested) returns the FULL committed struct (b=99), not a defaulted one");
    }

    // ---- coalescing two un-drained batches preserves a JSON-null (cleared optional) ----
    // The second batch clears `opt` to null. The coalesced diff must still report
    // changed(opt) at drain (a null value is a VALUE, not an RFC-7396 deletion).
    {
        config<opt_cfg> c;
        cp::config_binding<opt_cfg> b{c, config_type::INITIALIZE};
        b.prepare(json{{"opt", 7}}, config_type::INITIALIZE);
        b.notify(b.commit());
        b.run_pending();
        check(c->opt.has_value() && *c->opt == 7, "opt set to 7");

        int fires = 0;
        bool a_changed = false;
        bool opt_changed = false;
        c.on_apply([&](const opt_cfg&, const changes<opt_cfg>& ch) {
            ++fires;
            a_changed = ch.changed(&opt_cfg::a);
            opt_changed = ch.changed(&opt_cfg::opt);
        });
        // two batches WITHOUT a drain between them -> they coalesce
        b.prepare(json{{"a", 5}}, config_type::RUNTIME);
        b.notify(b.commit());
        b.prepare(json{{"opt", nullptr}}, config_type::RUNTIME); // clear opt
        b.notify(b.commit());
        b.run_pending();
        check(fires == 1, "one coalesced reaction");
        check(a_changed, "coalesced: 'a' reports changed");
        check(opt_changed, "coalesced: cleared optional reports changed (null not dropped by merge_patch)");
        check(!c->opt.has_value(), "opt actually cleared (value)");
    }

    if (g_fails != 0) {
        std::fprintf(stderr, "%d config check(s) FAILED\n", g_fails);
        return 1;
    }
    std::puts("CONFIG<T> TESTS PASSED");
    return 0;
}
