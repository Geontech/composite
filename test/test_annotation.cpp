// annotation_value carries a real type (bool/int64/double/string) and round-trips
// JSON with that type intact — not stringified. Also checks map<string,
// annotation_value> round-trips and to_string() still renders each type.
#include "composite/core/metadata.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>

using namespace composite;
using nlohmann::json;

int main() {
    // Each alternative encodes to its real JSON type and decodes back.
    assert(json(annotation_value{true}).is_boolean());
    assert(json(annotation_value{true}).get<bool>() == true);
    assert(json(annotation_value{std::int64_t{42}}).is_number_integer());
    assert(json(annotation_value{std::int64_t{42}}).get<std::int64_t>() == 42);
    assert(json(annotation_value{2.5}).is_number_float());
    assert(json(annotation_value{2.5}).get<double>() == 2.5);
    assert(json(annotation_value{std::string{"iq"}}).is_string());
    assert(json(annotation_value{std::string{"iq"}}).get<std::string>() == "iq");

    // A map of annotations round-trips with types preserved.
    std::map<std::string, annotation_value> m;
    m["flag"] = true;
    m["count"] = std::int64_t{7};
    m["gain"] = 1.25;
    m["fmt"] = "ci16";
    const json jm = m;
    const auto back = jm.get<std::map<std::string, annotation_value>>();
    assert(back == m);
    assert(back.at("flag").get<bool>() == true);
    assert(back.at("count").get<std::int64_t>() == 7);
    assert(back.at("gain").get<double>() == 1.25);
    assert(back.at("fmt").get<std::string>() == "ci16");

    // to_string() renders each type.
    assert(annotation_value{true}.to_string() == "true");
    assert(annotation_value{std::int64_t{5}}.to_string() == "5");
    assert(annotation_value{std::string{"hi"}}.to_string() == "hi");

    std::printf("ANNOTATION_VALUE JSON OK: typed round-trip (bool/int/double/string) + map + to_string\n");
    return 0;
}
