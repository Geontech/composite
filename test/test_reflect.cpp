// Reflection layer tests: encode/decode round-trip, RFC-7396 merge (incl. null
// reset + nested), diff+apply, enums, optionals, nested structs.
#include <cassert>
#include <composite/properties/reflect.hpp>
#include <cstdio>
#include <optional>
#include <string>

namespace rfl = composite::reflect;
using rfl::json;

enum class Window { hann, hamming, blackman_harris };
COMPOSITE_ENUM(Window, hann, hamming, blackman_harris);

struct Format {
    bool is_complex{};
    std::string type;
    std::uint32_t bit_width{};
};
COMPOSITE_STRUCT(Format, is_complex, type, bit_width);

struct ChannelConfig {
    std::string name;
    double center_freq{};
    double bandwidth{};
    std::uint32_t decimation{1};
    Window window{Window::hann};
    bool enabled{true};
};
COMPOSITE_STRUCT(ChannelConfig, name, center_freq, bandwidth, decimation, window, enabled);

struct SignalOverrides {
    std::optional<double> center_frequency;
    std::optional<double> bandwidth;
    Format data_format;
    std::string transport;
};
COMPOSITE_STRUCT(SignalOverrides, center_frequency, bandwidth, data_format, transport);

int main() {
    // 1. round-trip a channelizer config (enum + scalars)
    ChannelConfig c{"Lband", 1.2e9, 20e6, 4, Window::blackman_harris, true};
    auto j = rfl::encode(c);
    assert(j["window"] == "blackman_harris" && j["name"] == "Lband");
    ChannelConfig c2;
    rfl::decode(j, c2);
    assert(rfl::equal(c, c2));

    // 2. partial 7396 merge: only the specified scalar changes
    rfl::merge(c, json::parse(R"({"center_freq": 2.0e9})"));
    assert(c.center_freq == 2.0e9 && c.bandwidth == 20e6 && c.name == "Lband" && c.window == Window::blackman_harris);

    // 3. nested struct + optionals round-trip
    SignalOverrides so;
    so.center_frequency = 1.0e9;
    so.bandwidth = 5e6;
    so.data_format = {true, "ci16", 16};
    so.transport = "udp";
    SignalOverrides so2;
    rfl::decode(rfl::encode(so), so2);
    assert(rfl::equal(so, so2));

    // 4. 7396 null resets an optional; siblings preserved
    rfl::merge(so, json::parse(R"({"center_frequency": null})"));
    assert(!so.center_frequency.has_value() && so.bandwidth.has_value() && *so.bandwidth == 5e6);

    // 5. nested object merges (only bit_width changes; type/is_complex preserved)
    rfl::merge(so, json::parse(R"({"data_format": {"bit_width": 32}})"));
    assert(so.data_format.bit_width == 32 && so.data_format.type == "ci16" && so.data_format.is_complex == true);

    // 6. diff carries only changed fields, and applying it reproduces the target
    ChannelConfig a{"A", 1e9, 1e6, 2, Window::hann, true};
    ChannelConfig b = a;
    b.name = "B";
    b.window = Window::hamming;
    auto d = rfl::diff(a, b);
    assert(d.size() == 2 && d.contains("name") && d.contains("window") && !d.contains("center_freq"));
    ChannelConfig a_copy = a;
    rfl::merge(a_copy, d);
    assert(rfl::equal(a_copy, b));

    // 7. optional encodes as null when empty
    SignalOverrides empty_so;
    assert(rfl::encode(empty_so)["center_frequency"].is_null());

    std::printf("REFLECT TESTS PASSED (ChannelConfig fields=%zu, SignalOverrides round-trips, 7396 merge+diff OK)\n",
                std::tuple_size_v<decltype(rfl::descriptor<ChannelConfig>::fields())>);
    return 0;
}
