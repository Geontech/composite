// COMPOSITE_FIELDS: the ADL/in-namespace reflection macro with
// per-field attributes. Proves: reflection works INSIDE a user namespace and on
// private members (hidden friend); bare/attr/mixed field tokens; attribute
// retrieval (range/unit/doc/power_of_two/runtime/one_of); the reflected<T> concept;
// >16 and 64 fields (FE ladder); COMPOSITE_FIELDS_EXTERN; and that COMPOSITE_STRUCT
// still works and the existing encode/decode/merge engine drives both unchanged.
// Own main(); explicit checks (NDEBUG-safe).
#include <composite/properties/reflect.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <tuple>

namespace rfl = composite::reflect;
using rfl::json;

namespace acme { // a USER namespace — the whole point of the ADL form

enum class window { hann, hamming, blackman_harris };

struct filter_cfg {
    double gain{1.0};
    std::uint32_t taps{8};
    window win{window::hann};
    COMPOSITE_FIELDS(filter_cfg, (gain, runtime, range(0.0, 10.0), unit("dB"), doc("post-mix gain")),
                     (taps, runtime, range(1u, 1024u), power_of_two), win);
};

// hidden friend can see private members
class secret {
    int x{5};
    double y{2.5};

public:
    COMPOSITE_FIELDS(secret, x, (y, range(0.0, 100.0)));
    [[nodiscard]] auto xv() const -> int { return x; }
    [[nodiscard]] auto yv() const -> double { return y; }
};

} // namespace acme

COMPOSITE_ENUM(acme::window, hann, hamming, blackman_harris);

// COMPOSITE_FIELDS_EXTERN: for a struct you cannot edit — written in T's namespace.
namespace ext {
struct plain {
    int a{1};
    int b{2};
};
COMPOSITE_FIELDS_EXTERN(plain, a, b);
} // namespace ext

// legacy COMPOSITE_STRUCT (namespace scope) must still work and interop identically.
struct legacy {
    int p{};
    int q{};
};
COMPOSITE_STRUCT(legacy, p, q);

// 64 fields — exercises the full FE ladder ceiling.
struct big64 {
    int f0{0};
    int f1{1};
    int f2{2};
    int f3{3};
    int f4{4};
    int f5{5};
    int f6{6};
    int f7{7};
    int f8{8};
    int f9{9};
    int f10{10};
    int f11{11};
    int f12{12};
    int f13{13};
    int f14{14};
    int f15{15};
    int f16{16};
    int f17{17};
    int f18{18};
    int f19{19};
    int f20{20};
    int f21{21};
    int f22{22};
    int f23{23};
    int f24{24};
    int f25{25};
    int f26{26};
    int f27{27};
    int f28{28};
    int f29{29};
    int f30{30};
    int f31{31};
    int f32{32};
    int f33{33};
    int f34{34};
    int f35{35};
    int f36{36};
    int f37{37};
    int f38{38};
    int f39{39};
    int f40{40};
    int f41{41};
    int f42{42};
    int f43{43};
    int f44{44};
    int f45{45};
    int f46{46};
    int f47{47};
    int f48{48};
    int f49{49};
    int f50{50};
    int f51{51};
    int f52{52};
    int f53{53};
    int f54{54};
    int f55{55};
    int f56{56};
    int f57{57};
    int f58{58};
    int f59{59};
    int f60{60};
    int f61{61};
    int f62{62};
    int f63{63};
    COMPOSITE_FIELDS(big64, f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19,
                     f20, f21, f22, f23, f24, f25, f26, f27, f28, f29, f30, f31, f32, f33, f34, f35, f36, f37, f38, f39,
                     f40, f41, f42, f43, f44, f45, f46, f47, f48, f49, f50, f51, f52, f53, f54, f55, f56, f57, f58, f59,
                     f60, f61, f62, f63);
};

// compile-time: all of these are reflected; a plain struct is not.
static_assert(rfl::reflected<acme::filter_cfg>);
static_assert(rfl::reflected<acme::secret>);
static_assert(rfl::reflected<ext::plain>);
static_assert(rfl::reflected<legacy>);
static_assert(rfl::reflected<big64>);
struct not_reflected {
    int z;
};
static_assert(!rfl::reflected<not_reflected>);
static_assert(std::tuple_size_v<decltype(rfl::descriptor<big64>::fields())> == 64);

static int g_fails = 0;
static void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_fails;
    }
}

// pull the Nth field's attrs tuple type/value out of the descriptor
template <std::size_t I, typename T>
constexpr auto field_attrs() {
    return std::get<I>(rfl::descriptor<T>::fields()).attrs;
}

int main() {
    // ---- reflection works in a user namespace; encode reads the fields ----
    {
        acme::filter_cfg c;
        const json j = rfl::encode(c);
        check(j["gain"] == 1.0 && j["taps"] == 8 && j["win"] == "hann", "filter_cfg encodes via ADL descriptor");
        // the existing strict engine drives a COMPOSITE_FIELDS struct unchanged:
        rfl::merge(c, json::parse(R"({"gain": 2.5, "win": "hamming"})"));
        check(c.gain == 2.5 && c.win == acme::window::hamming && c.taps == 8, "merge round-trips");
        bool threw = false;
        try {
            rfl::merge(c, json::parse(R"({"bogus": 1})"));
        } catch (const rfl::decode_failure&) {
            threw = true;
        }
        check(threw, "strict unknown-field rejection still applies to COMPOSITE_FIELDS structs");
    }

    // ---- attributes are stored on the fields and retrievable ----
    {
        const auto gain_attrs = field_attrs<0, acme::filter_cfg>(); // (gain, runtime, range, unit, doc)
        check(rfl::has_attr<rfl::runtime_attr>(gain_attrs), "gain is runtime");
        const auto* r = rfl::find_attr<rfl::range_attr>(gain_attrs);
        check(r != nullptr && r->lo == 0.0 && r->hi == 10.0, "gain range(0,10)");
        const auto* u = rfl::find_attr<rfl::unit_attr>(gain_attrs);
        check(u != nullptr && u->text == "dB", "gain unit dB");
        const auto* d = rfl::find_attr<rfl::doc_attr>(gain_attrs);
        check(d != nullptr && d->text == "post-mix gain", "gain doc");

        const auto taps_attrs = field_attrs<1, acme::filter_cfg>(); // (taps, runtime, range, power_of_two)
        check(rfl::has_attr<rfl::power_of_two_attr>(taps_attrs), "taps power_of_two");
        const auto* tr = rfl::find_attr<rfl::range_attr>(taps_attrs);
        check(tr != nullptr && tr->lo == 1.0 && tr->hi == 1024.0, "taps range(1,1024)");

        const auto win_attrs = field_attrs<2, acme::filter_cfg>(); // bare field -> no attrs
        check(!rfl::has_attr<rfl::runtime_attr>(win_attrs), "bare field win carries no attrs");
        check(std::tuple_size_v<decltype(win_attrs)> == 0, "bare field has empty attrs tuple");
    }

    // ---- hidden friend reaches private members ----
    {
        acme::secret s;
        const json j = rfl::encode(s);
        check(j["x"] == 5 && j["y"] == 2.5, "private members reflected via hidden friend");
        rfl::merge(s, json::parse(R"({"x": 9})"));
        check(s.xv() == 9, "private member mutated through merge");
    }

    // ---- COMPOSITE_FIELDS_EXTERN + legacy COMPOSITE_STRUCT both interop ----
    {
        ext::plain p;
        check(rfl::encode(p) == json::parse(R"({"a":1,"b":2})"), "EXTERN form reflects");
        legacy l;
        check(rfl::encode(l) == json::parse(R"({"p":0,"q":0})"), "legacy COMPOSITE_STRUCT still works");
    }

    // ---- 64 fields: encode all, round-trip a high-index field ----
    {
        big64 b;
        const json j = rfl::encode(b);
        check(j.size() == 64 && j["f0"] == 0 && j["f63"] == 63, "64-field struct encodes all fields");
        rfl::merge(b, json::parse(R"({"f63": 999, "f17": 17000})"));
        check(b.f63 == 999 && b.f17 == 17000 && b.f0 == 0, "high-index fields merge (FE ladder > 16)");
    }

    if (g_fails != 0) {
        std::fprintf(stderr, "%d fields-macro check(s) FAILED\n", g_fails);
        return 1;
    }
    std::puts("COMPOSITE_FIELDS MACRO TESTS PASSED");
    return 0;
}
