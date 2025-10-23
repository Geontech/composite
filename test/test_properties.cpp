#define CATCH_CONFIG_MAIN
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <nlohmann/json.hpp>

#include "composite/property_set.hpp"
#include "helpers.hpp"

using namespace composite;

struct channel {
    uint32_t id{};
    std::optional<std::string> label;
    float frequency{};
};

struct config {
    int32_t gain{};
    std::optional<std::string> name;
    channel channel_;
    std::vector<float> thresholds;
    std::vector<channel> channels;
};

auto register_channel_fields(property_set& ps, channel* c) {
    using enum properties::config_type;
    ps.add_property("id", &c->id);
    ps.add_property("label", &c->label);
    ps.add_property("frequency", &c->frequency).configurability(RUNTIME);
}

auto register_config_properties(property_set& set, config* config) {
    using enum properties::config_type;
    set.add_property("gain", &config->gain).configurability(RUNTIME);
    set.add_property("name", &config->name);
    set.add_struct_property("channel", &config->channel_, register_channel_fields);
    set.add_list_property("thresholds", &config->thresholds).configurability(RUNTIME);
    set.add_struct_list_property("channels", &config->channels, register_channel_fields);
}

TEST_CASE("composite::property_set") {
    config cfg;
    property_set ps;
    register_config_properties(ps, &cfg);

    SECTION("scalar property get/set") {
        REQUIRE(cfg.gain == 0);
        REQUIRE(ps.get_property<int32_t>("gain") == 0);
        REQUIRE(ps.set_property("gain", int32_t{42}) == properties::error::OK);
        REQUIRE(cfg.gain == 42);
        REQUIRE(ps.get_property<int32_t>("gain") == 42);
        REQUIRE(ps.set_property("gain", int32_t{}, true) == properties::error::OK);
        REQUIRE(cfg.gain == 0);
        REQUIRE(ps.get_property<int32_t>("gain") == 0);
    }

    SECTION("scalar property get/set using set_properties") {
        REQUIRE(cfg.gain == 0);
        REQUIRE(ps.get_property<int32_t>("gain") == 0);
        ps.set_properties({{"gain", "42"}});
        REQUIRE(cfg.gain == 42);
        REQUIRE(ps.get_property<int32_t>("gain") == 42);
        ps.set_properties({{"gain", std::string{composite::properties::null_prop}}});
        REQUIRE(cfg.gain == 0);
        REQUIRE(ps.get_property<int32_t>("gain") == 0);
    }

    SECTION("optional property get/set") {
        REQUIRE_FALSE(cfg.name.has_value());
        REQUIRE(ps.try_get_property<std::string>("name") == std::nullopt);
        REQUIRE(ps.set_property("name", std::string{"foo"}) == properties::error::OK);
        REQUIRE(cfg.name.has_value());
        REQUIRE(cfg.name.value() == "foo");
        REQUIRE(ps.try_get_property<std::string>("name") == "foo");
        REQUIRE(ps.set_property("name", std::string{}, true) == properties::error::OK);
        REQUIRE_FALSE(cfg.name.has_value());
        REQUIRE(ps.try_get_property<std::string>("name") == std::nullopt);
    }

    SECTION("optional property get/set using set_properties") {
        REQUIRE_FALSE(cfg.name.has_value());
        REQUIRE(ps.try_get_property<std::string>("name") == std::nullopt);
        ps.set_properties({{"name", "foo"}});
        REQUIRE(cfg.name.has_value());
        REQUIRE(cfg.name.value() == "foo");
        REQUIRE(ps.try_get_property<std::string>("name") == "foo");
        ps.set_properties({{"name", std::string{composite::properties::null_prop}}});
        REQUIRE_FALSE(cfg.name.has_value());
        REQUIRE(ps.try_get_property<std::string>("name") == std::nullopt);
    }

    SECTION("struct property get/set") {
        REQUIRE(ps.get_property<uint32_t>("channel.id") == 0);
        REQUIRE(ps.try_get_property<std::string>("channel.label") == std::nullopt);
        REQUIRE(ps.get_property<float>("channel.frequency") == 0.f);
        REQUIRE(ps.set_property("channel.id", uint32_t{1}) == properties::error::OK);
        REQUIRE(ps.set_property("channel.label", std::string{"foo"}) == properties::error::OK);
        REQUIRE(ps.set_property("channel.frequency", float{1}) == properties::error::OK);
        REQUIRE(cfg.channel_.id == 1);
        REQUIRE(cfg.channel_.label.has_value());
        REQUIRE(cfg.channel_.label.value() == "foo");
        REQUIRE_THAT(cfg.channel_.frequency, Catch::Matchers::WithinAbs(1.0, 0.000001));
        REQUIRE(ps.get_property<uint32_t>("channel.id") == 1);
        REQUIRE(ps.try_get_property<std::string>("channel.label") == "foo");
        REQUIRE_THAT(ps.get_property<float>("channel.frequency"), Catch::Matchers::WithinAbs(1.0, 0.000001));
    }

    SECTION("struct property get/set using set_properties") {
        REQUIRE(ps.get_property<uint32_t>("channel.id") == 0);
        REQUIRE(ps.try_get_property<std::string>("channel.label") == std::nullopt);
        REQUIRE(ps.get_property<float>("channel.frequency") == 0.f);
        auto get_channel = ps.get_property<channel>("channel");
        REQUIRE(get_channel.id == 0);
        REQUIRE(get_channel.label == std::nullopt);
        REQUIRE(get_channel.frequency == 0.f);
        std::vector<std::pair<std::string, std::string>> values = {
            {"id", "1"},
            {"label", "foo"},
            {"frequency", "1.0"}
        };
        ps.set_properties({{"channel", values}});
        REQUIRE(cfg.channel_.id == 1);
        REQUIRE(cfg.channel_.label.has_value());
        REQUIRE(cfg.channel_.label.value() == "foo");
        REQUIRE_THAT(cfg.channel_.frequency, Catch::Matchers::WithinAbs(1.0, 0.000001));
        REQUIRE(ps.get_property<uint32_t>("channel.id") == 1);
        REQUIRE(ps.try_get_property<std::string>("channel.label") == "foo");
        REQUIRE_THAT(ps.get_property<float>("channel.frequency"), Catch::Matchers::WithinAbs(1.0, 0.000001));
        get_channel = ps.get_property<channel>("channel");
        REQUIRE(get_channel.id == 1);
        REQUIRE(get_channel.label.has_value());
        REQUIRE(get_channel.label.value() == "foo");
        REQUIRE_THAT(get_channel.frequency, Catch::Matchers::WithinAbs(1.0, 0.000001));
    }

    SECTION("struct property reset using set_properties") {
        cfg.channel_ = {.id=1, .label="foo", .frequency=100.f};
        ps.set_properties({{"channel", std::string{composite::properties::null_prop}}});
        REQUIRE(cfg.channel_.id == 0);
        REQUIRE_FALSE(cfg.channel_.label.has_value());
        REQUIRE_THAT(cfg.channel_.frequency, Catch::Matchers::WithinAbs(0.0, 0.000001));
    }

    SECTION("list property get/set") {
        REQUIRE(cfg.thresholds.empty());
        std::vector<float> new_vals = {1.f, 2.f, 3.f};
        REQUIRE(ps.get_list_property<float>("thresholds").empty());
        REQUIRE(ps.set_list_property("thresholds", new_vals) ==  properties::error::OK);
        REQUIRE(cfg.thresholds.size() == 3);
        REQUIRE(cfg.thresholds == new_vals);
        REQUIRE(ps.get_list_property<float>("thresholds") == new_vals);
    }

    SECTION("list property get/set using set_properties") {
        REQUIRE(cfg.thresholds.empty());
        std::vector<float> new_fvals = {1.0, 2.0, 3.0};
        std::vector<std::string> new_vals = {"1.0", "2.0", "3.0"};
        REQUIRE(ps.get_list_property<float>("thresholds").empty());
        ps.set_properties({{"thresholds", new_vals}});
        REQUIRE(cfg.thresholds.size() == 3);
        REQUIRE(cfg.thresholds == new_fvals);
        REQUIRE(ps.get_list_property<float>("thresholds") == new_fvals);
    }

    SECTION("list property item get/set") {
        cfg.thresholds = {1.f, 2.f, 3.f};
        REQUIRE_THAT(ps.get_list_property_item<float>("thresholds", 0), Catch::Matchers::WithinAbs(1.0, 0.000001));
        REQUIRE_THAT(ps.get_list_property_item<float>("thresholds", 1), Catch::Matchers::WithinAbs(2.0, 0.000001));
        REQUIRE_THAT(ps.get_list_property_item<float>("thresholds", 2), Catch::Matchers::WithinAbs(3.0, 0.000001));
        REQUIRE(ps.set_list_property_item("thresholds", 1, 4.f) == properties::error::OK);
        REQUIRE_THAT(ps.get_list_property_item<float>("thresholds", 1), Catch::Matchers::WithinAbs(4.0, 0.000001));
        REQUIRE_THAT(cfg.thresholds[1], Catch::Matchers::WithinAbs(4.0, 0.000001));
    }

    SECTION("list property item get/set using set_properties") {
        cfg.thresholds = {1.f, 2.f, 3.f};
        ps.set_properties({{"thresholds[1]", "4.0"}});
        REQUIRE_THAT(ps.get_list_property_item<float>("thresholds", 1), Catch::Matchers::WithinAbs(4.0, 0.000001));
        REQUIRE_THAT(cfg.thresholds[1], Catch::Matchers::WithinAbs(4.0, 0.000001));
    }

    SECTION("list property item append") {
        REQUIRE(ps.append_list_property_item("thresholds", float{42}) == properties::error::OK);
        REQUIRE(cfg.thresholds.size() == 1);
        REQUIRE_THAT(cfg.thresholds[0], Catch::Matchers::WithinAbs(42.0, 0.000001));
    }

    SECTION("list property item append using set_properties") {
        ps.set_properties({{"thresholds[]", "42.0"}});
        REQUIRE(cfg.thresholds.size() == 1);
        REQUIRE_THAT(cfg.thresholds[0], Catch::Matchers::WithinAbs(42.0, 0.000001));
    }

    SECTION("list property item erase") {
        cfg.thresholds = {1.f, 2.f, 3.f};
        REQUIRE(ps.erase_list_property_item<float>("thresholds", 1) == properties::error::OK);
        REQUIRE(cfg.thresholds.size() == 2);
    }

    SECTION("list property item erase using set_properties") {
        cfg.thresholds = {1.f, 2.f, 3.f};
        ps.set_properties({{"thresholds[0]", std::string{composite::properties::null_prop}}});
        REQUIRE(cfg.thresholds.size() == 2);
        REQUIRE(cfg.thresholds == std::vector<float>{2.f, 3.f});
    }

    SECTION("update structured list field using set_properties") {
        cfg.channels.emplace_back();
        std::vector<std::pair<std::string, std::string>> values = {
            {"id", "1"},
            {"label", "foo"},
            {"frequency", "1.0"}
        };
        ps.set_properties({{"channels[0]", values}});
        REQUIRE(cfg.channels[0].id == 1);
        REQUIRE(cfg.channels[0].label.has_value());
        REQUIRE(cfg.channels[0].label.value() == "foo");
        REQUIRE_THAT(cfg.channels[0].frequency, Catch::Matchers::WithinAbs(1.0, 0.000001));
    }

    SECTION("append structured list field using set_properties") {
        std::vector<std::pair<std::string, std::string>> values = {
            {"id", "1"},
            {"label", "foo"},
            {"frequency", "1.0"}
        };
        ps.set_properties({{"channels[]", values}});
        REQUIRE(cfg.channels.size() == 1);
        REQUIRE(cfg.channels[0].id == 1);
        REQUIRE(cfg.channels[0].label.has_value());
        REQUIRE(cfg.channels[0].label.value() == "foo");
        REQUIRE_THAT(cfg.channels[0].frequency, Catch::Matchers::WithinAbs(1.0, 0.000001));
    }

    SECTION("erase structured list field using set_properties") {
        cfg.channels.emplace_back();
        ps.set_properties({{"channels[0]", std::string{composite::properties::null_prop}}});
        REQUIRE(cfg.channels.empty());
    }

} // end TEST_CASE("composite::property_set")

TEST_CASE("composite REST server set_properties") {

    SECTION("build_props_lists") {
        using namespace nlohmann::literals;
        auto json_config = R"({
            "test_key": "test_value",
            "test_null_key": null,
            "list_key[0]": "list_item_value",
            "test_list_key": ["1", "2", "3"],
            "test_struct_key": {
                "struct_key_1": "struct_value_1",
                "struct_key_2": "struct_value_2"
            },
            "test_struct_list_key[3]": {
                "struct_key_1": "struct_value_1",
                "struct_key_2": "struct_value_2"
            }
        })"_json;
        const auto& [props, list_props, struct_props] = build_props_lists(json_config);
        auto vec_contains = [](auto& vec, const std::string& key) -> std::optional<std::size_t> {
            auto i = std::size_t{};
            for (const auto& [k, v] : vec) {
                if (k == key) {
                   return i;
                }
                ++i;
            }
            return std::nullopt;
        };
        REQUIRE(props.size() == 3);
        auto prop_idx = vec_contains(props, "test_key");
        REQUIRE(prop_idx.has_value());
        REQUIRE(props[*prop_idx].first == "test_key");
        REQUIRE(props[*prop_idx].second == "test_value");
        prop_idx = vec_contains(props, "test_null_key");
        REQUIRE(prop_idx.has_value());
        REQUIRE(props[*prop_idx].first == "test_null_key");
        REQUIRE(props[*prop_idx].second == std::string{composite::properties::null_prop});
        prop_idx = vec_contains(props, "list_key[0]");
        REQUIRE(prop_idx.has_value());
        REQUIRE(props[*prop_idx].first == "list_key[0]");
        REQUIRE(props[*prop_idx].second == "list_item_value");
        REQUIRE(list_props.size() == 1);
        REQUIRE(list_props.front().first == "test_list_key");
        REQUIRE(list_props.front().second == std::vector<std::string>{"1", "2", "3"});
        REQUIRE(struct_props.size() == 2);
        prop_idx = vec_contains(struct_props, "test_struct_key");
        REQUIRE(prop_idx.has_value());
        REQUIRE(struct_props[*prop_idx].first == "test_struct_key");
        REQUIRE(struct_props[*prop_idx].second.size() == 2);
        auto internal_prop_idx = vec_contains(struct_props[*prop_idx].second, "struct_key_1");
        REQUIRE(internal_prop_idx.has_value());
        REQUIRE(struct_props[*prop_idx].second[*internal_prop_idx].second == "struct_value_1");
        prop_idx = vec_contains(struct_props, "test_struct_list_key[3]");
        REQUIRE(prop_idx.has_value());
        REQUIRE(struct_props[*prop_idx].first == "test_struct_list_key[3]");
        REQUIRE(struct_props[*prop_idx].second.size() == 2);
        internal_prop_idx = vec_contains(struct_props[*prop_idx].second, "struct_key_1");
        REQUIRE(internal_prop_idx.has_value());
        REQUIRE(struct_props[*prop_idx].second[*internal_prop_idx].second == "struct_value_1");
    }

    SECTION("build_props_lists handles nested structs") {
        using namespace nlohmann::literals;
        auto json_config = R"({
            "signal_overrides": {
                "center_frequency": "10e9",
                "sample_rate": "10e6",
                "data_format": {
                    "is_complex": "true"
                }
            }
        })"_json;
        const auto& [props, list_props, struct_props] = build_props_lists(json_config);
        REQUIRE(props.empty());
        REQUIRE(list_props.empty());
        REQUIRE(struct_props.size() == 1);
        REQUIRE(struct_props.front().first == "signal_overrides");
        REQUIRE(struct_props.front().second.size() == 3);
        auto vec_contains = [](const auto& vec, const std::string& key) -> std::optional<std::size_t> {
            auto i = std::size_t{};
            for (const auto& [k, _] : vec) {
                if (k == key) {
                    return i;
                }
                ++i;
            }
            return std::nullopt;
        };
        auto idx = vec_contains(struct_props.front().second, "center_frequency");
        REQUIRE(idx.has_value());
        REQUIRE(struct_props.front().second[*idx].second == "10e9");
        idx = vec_contains(struct_props.front().second, "sample_rate");
        REQUIRE(idx.has_value());
        REQUIRE(struct_props.front().second[*idx].second == "10e6");
        idx = vec_contains(struct_props.front().second, "data_format.is_complex");
        REQUIRE(idx.has_value());
        REQUIRE(struct_props.front().second[*idx].second == "true");
    }

} // TEST_CASE("composite REST server set_properties")
