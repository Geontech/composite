/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifdef COMPOSITE_USE_NATS

#include "composite/transports/nats/client.hpp"
#include "composite/transports/nats/transport.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <format>
#include <thread>

using namespace composite;
using namespace std::chrono_literals;
using namespace Catch::Matchers;

// Helper to check if NATS server is available
auto nats_available() -> bool {
    try {
        nats::client test_client("nats://localhost:4222");
        return test_client.is_connected();
    } catch (...) {
        return false;
    }
}

// =============================================================================
// NATS Client Tests
// =============================================================================

TEST_CASE("NATS client connection", "[nats][integration][.]") {
    if (!nats_available()) {
        SKIP("NATS server not available at localhost:4222\n"
             "Start with: docker run -d -p 4222:4222 nats:latest");
    }

    SECTION("successful connection") {
        nats::client client("nats://localhost:4222");
        REQUIRE(client.is_connected());
    }

    SECTION("connection status check") {
        nats::client client("nats://localhost:4222");
        REQUIRE(client.is_connected());
    }

    SECTION("invalid URL throws exception") {
        // Constructor should throw std::runtime_error for invalid URLs
        REQUIRE_THROWS_AS(
            nats::client("nats://nonexistent-invalid-host-12345:9999"),
            std::runtime_error
        );
    }

    SECTION("malformed URL throws exception") {
        REQUIRE_THROWS_AS(
            nats::client("not-a-valid-url"),
            std::runtime_error
        );
    }
}

TEST_CASE("NATS client publish", "[nats][integration][.]") {
    if (!nats_available()) {
        SKIP("NATS server not available");
    }

    nats::client client("nats://localhost:4222");
    REQUIRE(client.is_connected());

    SECTION("publish simple message") {
        std::string test_data = "Hello NATS!";
        auto data_span = std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(test_data.data()),
            test_data.size()
        };

        auto status = client.publish("test.simple", data_span);
        REQUIRE(status == NATS_OK);
    }

    SECTION("publish empty message") {
        std::span<const std::byte> empty_span{};
        auto status = client.publish("test.empty", empty_span);
        REQUIRE(status == NATS_OK);
    }

    SECTION("publish large message (1MB)") {
        std::vector<std::byte> large_data(1024 * 1024, std::byte{0xAA});
        auto status = client.publish("test.large", large_data);
        REQUIRE(status == NATS_OK);
    }

    SECTION("publish with reply subject") {
        std::string test_data = "Request message";
        auto data_span = std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(test_data.data()),
            test_data.size()
        };

        auto status = client.publish("test.request", data_span, "test.reply");
        REQUIRE(status == NATS_OK);
    }
}

TEST_CASE("NATS client subscribe", "[nats][integration][.]") {
    if (!nats_available()) {
        SKIP("NATS server not available");
    }

    nats::client client("nats://localhost:4222");
    REQUIRE(client.is_connected());

    SECTION("subscribe to subject") {
        auto status = client.subscribe("test.subscribe");
        REQUIRE(status == NATS_OK);

        auto* sub = client.subscriber("test.subscribe");
        REQUIRE(sub != nullptr);
    }

    SECTION("subscribe to invalid subject") {
        auto status = client.subscribe("");
        REQUIRE(status == NATS_INVALID_SUBJECT);
    }

    SECTION("get non-existent subscriber") {
        auto* sub = client.subscriber("nonexistent.subject");
        REQUIRE(sub == nullptr);
    }
}

TEST_CASE("NATS client publish-subscribe round trip", "[nats][integration][.]") {
    if (!nats_available()) {
        SKIP("NATS server not available");
    }

    nats::client client("nats://localhost:4222");
    REQUIRE(client.is_connected());

    // Subscribe first
    auto status = client.subscribe("test.roundtrip");
    REQUIRE(status == NATS_OK);

    auto* sub = client.subscriber("test.roundtrip");
    REQUIRE(sub != nullptr);

    // Publish message
    std::string test_data = "Round-trip test message";
    auto data_span = std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(test_data.data()),
        test_data.size()
    };

    status = client.publish("test.roundtrip", data_span);
    REQUIRE(status == NATS_OK);

    // Receive message
    auto msg = sub->next_msg(5000ms);
    REQUIRE(msg != nullptr);

    auto msg_data = msg->data();
    std::string received{reinterpret_cast<const char*>(msg_data.data()), msg_data.size()};

    REQUIRE(received == test_data);
    REQUIRE(msg->size() == test_data.size());
}

TEST_CASE("NATS subscription timeout", "[nats][integration][.]") {
    if (!nats_available()) {
        SKIP("NATS server not available");
    }

    nats::client client("nats://localhost:4222");
    REQUIRE(client.is_connected());

    auto status = client.subscribe("test.timeout");
    REQUIRE(status == NATS_OK);

    auto* sub = client.subscriber("test.timeout");
    REQUIRE(sub != nullptr);

    SECTION("timeout on no message") {
        auto start = std::chrono::steady_clock::now();
        auto msg = sub->next_msg(100ms);
        auto end = std::chrono::steady_clock::now();

        REQUIRE(msg == nullptr);
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        REQUIRE(duration >= 100ms);
        REQUIRE(duration < 200ms);  // Some tolerance
    }
}

TEST_CASE("NATS multiple subscriptions", "[nats][integration][.]") {
    if (!nats_available()) {
        SKIP("NATS server not available");
    }

    nats::client client("nats://localhost:4222");
    REQUIRE(client.is_connected());

    // Subscribe to multiple subjects
    REQUIRE(client.subscribe("test.multi.1") == NATS_OK);
    REQUIRE(client.subscribe("test.multi.2") == NATS_OK);
    REQUIRE(client.subscribe("test.multi.3") == NATS_OK);

    // Verify all subscriptions exist
    REQUIRE(client.subscriber("test.multi.1") != nullptr);
    REQUIRE(client.subscriber("test.multi.2") != nullptr);
    REQUIRE(client.subscriber("test.multi.3") != nullptr);

    // Publish to different subjects
    std::string data1 = "Message 1";
    std::string data2 = "Message 2";
    std::string data3 = "Message 3";

    auto span1 = std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(data1.data()), data1.size()
    };
    auto span2 = std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(data2.data()), data2.size()
    };
    auto span3 = std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(data3.data()), data3.size()
    };

    REQUIRE(client.publish("test.multi.1", span1) == NATS_OK);
    REQUIRE(client.publish("test.multi.2", span2) == NATS_OK);
    REQUIRE(client.publish("test.multi.3", span3) == NATS_OK);

    // Receive messages on each subscription
    auto msg1 = client.subscriber("test.multi.1")->next_msg(1000ms);
    auto msg2 = client.subscriber("test.multi.2")->next_msg(1000ms);
    auto msg3 = client.subscriber("test.multi.3")->next_msg(1000ms);

    REQUIRE(msg1 != nullptr);
    REQUIRE(msg2 != nullptr);
    REQUIRE(msg3 != nullptr);
}

// =============================================================================
// NATS Transport Tests
// =============================================================================

TEST_CASE("NATS transport basic operations", "[nats][integration][transport][.]") {
    if (!nats_available()) {
        SKIP("NATS server not available");
    }

    SECTION("transport creation and connection") {
        auto transport = std::make_unique<nats::transport>(
            "nats://localhost:4222",
            "test.transport"
        );

        REQUIRE(transport->is_connected());
        REQUIRE(transport->type() == transport_type::nats);
        REQUIRE_THAT(transport->endpoint(), ContainsSubstring("localhost:4222"));
        REQUIRE_THAT(transport->endpoint(), ContainsSubstring("test.transport"));
    }

    SECTION("transport send data") {
        auto transport = std::make_unique<nats::transport>(
            "nats://localhost:4222",
            "test.transport.send"
        );

        std::vector<std::byte> test_data(100, std::byte{0x42});
        auto ts = timestamp::now();

        bool success = transport->send(test_data, ts);
        REQUIRE(success);

        // Check statistics
        REQUIRE(transport->packets_sent() == 1);
        REQUIRE(transport->bytes_sent() == 100);
        REQUIRE(transport->send_failures() == 0);
    }

    SECTION("transport statistics") {
        auto transport = std::make_unique<nats::transport>(
            "nats://localhost:4222",
            "test.transport.stats"
        );

        // Send multiple messages
        std::vector<std::byte> data(50, std::byte{0xFF});
        for (int i = 0; i < 10; ++i) {
            transport->send(data, timestamp::now());
        }

        REQUIRE(transport->packets_sent() == 10);
        REQUIRE(transport->bytes_sent() == 500);

        // Reset statistics
        transport->reset_stats();
        REQUIRE(transport->packets_sent() == 0);
        REQUIRE(transport->bytes_sent() == 0);
        REQUIRE(transport->send_failures() == 0);
    }
}

TEST_CASE("NATS transport error handling", "[nats][integration][transport][.]") {
    SECTION("invalid URL throws exception") {
        REQUIRE_THROWS_AS(
            nats::transport("nats://nonexistent-host:9999", "test"),
            std::runtime_error
        );
    }
}

// =============================================================================
// Performance Tests
// =============================================================================

TEST_CASE("NATS throughput test", "[nats][integration][performance][.]") {
    if (!nats_available()) {
        SKIP("NATS server not available");
    }

    nats::client client("nats://localhost:4222");
    REQUIRE(client.is_connected());

    SECTION("throughput - 1000 small messages") {
        const size_t num_messages = 1000;
        std::vector<std::byte> payload(100, std::byte{0xAA});  // 100 bytes

        auto start = std::chrono::steady_clock::now();
        for (size_t i = 0; i < num_messages; ++i) {
            client.publish("test.throughput", payload);
        }
        auto end = std::chrono::steady_clock::now();

        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        auto msg_per_sec = (num_messages * 1000.0) / duration.count();

        INFO("Sent " << num_messages << " messages in " << duration.count() << " ms");
        INFO("Throughput: " << msg_per_sec << " msg/s");

        // Basic sanity check - should be able to send >100 msg/s
        REQUIRE(msg_per_sec > 100);
    }

    SECTION("throughput - large messages") {
        const size_t num_messages = 100;
        std::vector<std::byte> payload(10240, std::byte{0xBB});  // 10KB

        auto start = std::chrono::steady_clock::now();
        for (size_t i = 0; i < num_messages; ++i) {
            client.publish("test.throughput.large", payload);
        }
        auto end = std::chrono::steady_clock::now();

        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        auto mbps = (num_messages * payload.size() * 8.0) / (duration.count() / 1000.0) / 1e6;

        INFO("Sent " << num_messages << " x 10KB messages in " << duration.count() << " ms");
        INFO("Throughput: " << mbps << " Mbps");

        // Basic sanity check - should be able to achieve >1 Mbps locally
        REQUIRE(mbps > 1.0);
    }
}

#else  // !COMPOSITE_USE_NATS

#include <catch2/catch_test_macros.hpp>

TEST_CASE("NATS tests disabled", "[nats][integration][.]") {
    SKIP("NATS support not enabled. Reconfigure with -DCOMPOSITE_USE_NATS=ON");
}

#endif  // COMPOSITE_USE_NATS
