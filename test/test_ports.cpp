/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#define CATCH_CONFIG_MAIN
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "composite/buffers/buffer.hpp"
#include "composite/core/application.hpp"
#include "composite/core/component.hpp"
#include "composite/core/metadata.hpp"
#include "composite/ports/input_port.hpp"
#include "composite/ports/output_port.hpp"
#include <array>
#include <deque>
#include <numeric>
#include <vector>

using namespace composite;

// Poll @p pred until it is true or @p timeout elapses; returns its final value. Replaces
// fixed sleep_for() "give the worker time" waits so the started-worker lifecycle tests do
// not flake under sanitizer slowdown / parallel oversubscription — it returns the instant
// the condition holds (so it never over-waits) and only waits as long as actually needed.
template <typename Pred>
static auto wait_until(Pred pred, std::chrono::milliseconds timeout = std::chrono::seconds(2)) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return pred();
}

// ============================================================================
// Test Fixtures and Mock Components
// ============================================================================

/**
 * @brief Simple source component that generates mutable buffers
 */
class TestMutableSource : public component {
public:
    TestMutableSource() : component("TestMutableSource") { add_port(m_output); }

    auto process() -> retval override {
        if (m_sent) {
            return retval::FINISH;
        }

        auto buffer = make_mutable<float>(m_size);
        for (std::size_t i = 0; i < buffer.size(); ++i) {
            buffer[i] = static_cast<float>(i) * m_multiplier;
        }

        m_output.send_data(std::move(buffer), timestamp{});
        m_sent = true;
        return retval::NORMAL;
    }

    std::size_t m_size{10};
    float m_multiplier{1.0f};
    bool m_sent{false};

private:
    output_port<mutable_buffer<float>> m_output{"data_out"};
};

/**
 * @brief Simple source component that generates immutable buffers
 */
class TestImmutableSource : public component {
public:
    TestImmutableSource() : component("TestImmutableSource") { add_port(m_output); }

    auto process() -> retval override {
        if (m_sent) {
            return retval::FINISH;
        }

        // Create mutable buffer, populate it, then freeze to immutable
        auto mutable_buffer = make_mutable<float>(m_size);
        for (std::size_t i = 0; i < mutable_buffer.size(); ++i) {
            mutable_buffer[i] = static_cast<float>(i) * m_multiplier;
        }
        auto buffer = std::move(mutable_buffer).to_immutable();

        m_output.send_data(std::move(buffer), timestamp{});
        m_sent = true;
        return retval::NORMAL;
    }

    std::size_t m_size{10};
    float m_multiplier{1.0f};
    bool m_sent{false};

private:
    output_port<immutable_buffer<float>> m_output{"data_out"};
};

/**
 * @brief Sink component that receives mutable buffers
 */
class TestMutableSink : public component {
public:
    TestMutableSink() : component("TestMutableSink") { add_port(m_input); }

    auto process() -> retval override {
        auto [buffer, ts, metadata] = m_input.get_data();

        if (buffer.size() == 0) {
            return retval::NOOP;
        }

        m_received = true;
        m_size = buffer.size();
        m_values.clear();
        for (std::size_t i = 0; i < buffer.size(); ++i) {
            m_values.push_back(buffer[i]);
        }

        return retval::FINISH;
    }

    bool m_received{false};
    std::size_t m_size{0};
    std::vector<float> m_values;

private:
    input_port<mutable_buffer<float>> m_input{"data_in"};
};

/**
 * @brief Sink component that receives immutable buffers
 */
class TestImmutableSink : public component {
public:
    explicit TestImmutableSink(std::string_view id = "TestImmutableSink") : component(id) { add_port(m_input); }

    auto process() -> retval override {
        auto [buffer, ts, metadata] = m_input.get_data();

        if (buffer.size() == 0) {
            return retval::NOOP;
        }

        m_received = true;
        m_size = buffer.size();
        m_values.clear();
        for (std::size_t i = 0; i < buffer.size(); ++i) {
            m_values.push_back(buffer[i]);
        }

        return retval::FINISH;
    }

    bool m_received{false};
    std::size_t m_size{0};
    std::vector<float> m_values;

private:
    input_port<immutable_buffer<float>> m_input{"data_in"};
};

/**
 * @brief Amplifier component (in-place modification)
 */
class TestAmplifier : public component {
public:
    TestAmplifier() : component("TestAmplifier") {
        add_port(m_input);
        add_port(m_output);
        add_property("gain", m_gain);
    }

    auto process() -> retval override {
        auto [buffer, ts, metadata] = m_input.get_data();

        if (buffer.size() == 0) {
            return retval::NOOP;
        }

        m_processed = true;

        // Modify in place
        for (std::size_t i = 0; i < buffer.size(); ++i) {
            buffer[i] *= m_gain;
        }

        m_output.send_data(std::move(buffer), ts);
        return retval::FINISH;
    }

    bool m_processed{false};
    float m_gain{2.0f};

private:
    input_port<mutable_buffer<float>> m_input{"data_in"};
    output_port<mutable_buffer<float>> m_output{"data_out"};
};

/**
 * @brief Broadcaster component (converts mutable to immutable)
 */
class TestBroadcaster : public component {
public:
    TestBroadcaster() : component("TestBroadcaster") {
        add_port(m_input);
        add_port(m_output1);
        add_port(m_output2);
        add_port(m_output3);
    }

    auto process() -> retval override {
        auto [buffer, ts, metadata] = m_input.get_data();

        if (buffer.size() == 0) {
            return retval::NOOP;
        }

        m_broadcasted = true;

        // Convert to immutable for sharing
        auto immutable = std::move(buffer).to_immutable();

        // Share with all outputs (zero copy)
        m_output1.send_data(immutable.share(), ts);
        m_output2.send_data(immutable.share(), ts);
        m_output3.send_data(immutable.share(), ts);

        return retval::FINISH;
    }

    bool m_broadcasted{false};

private:
    input_port<mutable_buffer<float>> m_input{"data_in"};
    output_port<immutable_buffer<float>> m_output1{"data_out1"};
    output_port<immutable_buffer<float>> m_output2{"data_out2"};
    output_port<immutable_buffer<float>> m_output3{"data_out3"};
};

/**
 * @brief Source that sends data with metadata
 */
class TestMetadataSource : public component {
public:
    TestMetadataSource() : component("TestMetadataSource") { add_port(m_output); }

    auto process() -> retval override {
        if (m_sent) {
            return retval::FINISH;
        }

        // Send metadata first
        metadata md;
        md.format.is_complex = m_metadata_format_complex;
        md.format.type = data_type::floating_point;
        md.format.bit_width = 32;
        md.center_frequency = m_metadata_cf;
        md.bandwidth = m_metadata_bw;
        md.sample_rate = m_metadata_sr;
        md.eos = m_metadata_eos;
        if (!m_annotation_key.empty()) {
            md.annotations[m_annotation_key] = m_annotation_value;
        }

        // Send data, carrying the metadata atomically with the packet
        auto buffer = make_mutable<float>(m_size);
        for (std::size_t i = 0; i < buffer.size(); ++i) {
            buffer[i] = static_cast<float>(i);
        }

        m_output.send_data(std::move(buffer), timestamp{}, md);
        m_sent = true;
        return retval::NORMAL;
    }

    std::size_t m_size{10};
    bool m_sent{false};
    double m_metadata_cf{2.4e9};
    double m_metadata_bw{1e6};
    double m_metadata_sr{1e6};
    bool m_metadata_eos{false};
    bool m_metadata_format_complex{false};
    std::string m_annotation_key{};
    std::string m_annotation_value{};

private:
    output_port<mutable_buffer<float>> m_output{"data_out"};
};

/**
 * @brief Sink that captures data and metadata
 */
class TestMetadataSink : public component {
public:
    TestMetadataSink() : component("TestMetadataSink") { add_port(m_input); }

    auto process() -> retval override {
        auto [buffer, ts, md] = m_input.get_data();

        if (buffer.size() == 0) {
            return retval::NOOP;
        }

        m_received = true;
        m_size = buffer.size();
        m_metadata = md; // Capture metadata
        m_values.clear();
        for (std::size_t i = 0; i < buffer.size(); ++i) {
            m_values.push_back(buffer[i]);
        }

        return retval::FINISH;
    }

    bool m_received{false};
    std::size_t m_size{0};
    std::vector<float> m_values;
    metadata_ptr m_metadata;

private:
    input_port<mutable_buffer<float>> m_input{"data_in"};
};

/**
 * @brief Pass-through component that forwards metadata
 */
class TestMetadataPassthrough : public component {
public:
    TestMetadataPassthrough() : component("TestMetadataPassthrough") {
        add_port(m_input);
        add_port(m_output);
    }

    auto process() -> retval override {
        auto [buffer, ts, md] = m_input.get_data();

        if (buffer.size() == 0) {
            return retval::NOOP;
        }

        m_processed = true;
        m_received_metadata = md;

        // Forward data with its metadata attached
        m_output.send_data(std::move(buffer), ts, md);
        return retval::FINISH;
    }

    bool m_processed{false};
    metadata_ptr m_received_metadata;

private:
    input_port<mutable_buffer<float>> m_input{"data_in"};
    output_port<mutable_buffer<float>> m_output{"data_out"};
};

/**
 * @brief Component that modifies metadata
 */
class TestMetadataModifier : public component {
public:
    TestMetadataModifier() : component("TestMetadataModifier") {
        add_port(m_input);
        add_port(m_output);
        add_property("center_frequency_offset", m_cf_offset);
    }

    auto process() -> retval override {
        auto [buffer, ts, md] = m_input.get_data();

        if (buffer.size() == 0) {
            return retval::NOOP;
        }

        m_processed = true;

        // Modify metadata (if any) and forward it with the data: shared metadata is
        // immutable in flight, so a modifying component copies the value, edits, re-wraps.
        if (md != nullptr) {
            auto modified = *md;
            modified.center_frequency += m_cf_offset;
            modified.annotations["modified_by"] = id();
            md = make_metadata(std::move(modified));
        }
        m_output.send_data(std::move(buffer), ts, md);
        return retval::FINISH;
    }

    bool m_processed{false};
    double m_cf_offset{0.0};

private:
    input_port<mutable_buffer<float>> m_input{"data_in"};
    output_port<mutable_buffer<float>> m_output{"data_out"};
};

// ============================================================================
// Buffer Tests
// ============================================================================

TEST_CASE("mutable_buffer basic operations", "[buffer][mutable]") {
    SECTION("construction and size") {
        auto buffer = make_mutable<float>(100);
        REQUIRE(buffer.size() == 100);
    }

    SECTION("element access and modification") {
        auto buffer = make_mutable<float>(10);

        // Write via operator[]
        for (std::size_t i = 0; i < buffer.size(); ++i) {
            buffer[i] = static_cast<float>(i * 2);
        }

        // Read back
        for (std::size_t i = 0; i < buffer.size(); ++i) {
            REQUIRE_THAT(buffer[i], Catch::Matchers::WithinAbs(i * 2.0f, 0.001f));
        }
    }

    SECTION("data pointer access") {
        auto buffer = make_mutable<float>(10);
        auto* ptr = buffer.data();

        ptr[5] = 42.0f;
        REQUIRE_THAT(buffer[5], Catch::Matchers::WithinAbs(42.0f, 0.001f));
    }

    SECTION("span access") {
        auto buffer = make_mutable<float>(10);
        auto span = buffer.as_span();

        REQUIRE(span.size() == 10);
        span[3] = 99.0f;
        REQUIRE_THAT(buffer[3], Catch::Matchers::WithinAbs(99.0f, 0.001f));
    }

    SECTION("iteration") {
        auto buffer = make_mutable<float>(10);
        std::size_t count = 0;
        for (auto& val : buffer) {
            val = static_cast<float>(count++);
        }

        REQUIRE(count == 10);
        REQUIRE_THAT(buffer[5], Catch::Matchers::WithinAbs(5.0f, 0.001f));
    }

    SECTION("copy operation") {
        auto buffer = make_mutable<float>(10);
        for (std::size_t i = 0; i < buffer.size(); ++i) {
            buffer[i] = static_cast<float>(i);
        }

        auto copy = buffer.copy();
        REQUIRE(copy.size() == buffer.size());

        for (std::size_t i = 0; i < buffer.size(); ++i) {
            REQUIRE_THAT(copy[i], Catch::Matchers::WithinAbs(buffer[i], 0.001f));
        }

        // Modify copy, original should be unchanged
        copy[0] = 999.0f;
        REQUIRE_THAT(buffer[0], Catch::Matchers::WithinAbs(0.0f, 0.001f));
    }

    SECTION("conversion to immutable") {
        auto mutable_buf = make_mutable<float>(10);
        for (std::size_t i = 0; i < mutable_buf.size(); ++i) {
            mutable_buf[i] = static_cast<float>(i);
        }

        auto immutable_buf = std::move(mutable_buf).to_immutable();
        REQUIRE(immutable_buf.size() == 10);
        REQUIRE_THAT(immutable_buf[5], Catch::Matchers::WithinAbs(5.0f, 0.001f));
    }
}

TEST_CASE("immutable_buffer basic operations", "[buffer][immutable]") {
    SECTION("construction and size") {
        auto buffer = make_immutable<float>(100);
        REQUIRE(buffer.size() == 100);
    }

    SECTION("read-only access") {
        auto buffer = make_immutable<float>(10);

        // Can read
        const auto& val = buffer[0];
        (void)val;

        // Cannot write (compile-time check)
        // buffer[0] = 42.0f;  // Won't compile
    }

    SECTION("data pointer is const") {
        auto buffer = make_immutable<float>(10);
        const auto* ptr = buffer.data();

        // Can read
        auto val = ptr[0];
        (void)val;

        // Cannot write (compile-time check)
        // ptr[0] = 42.0f;  // Won't compile
    }

    SECTION("span is const") {
        auto buffer = make_immutable<float>(10);
        auto span = buffer.as_span();

        REQUIRE(span.size() == 10);

        // Type is span<const float>
        static_assert(std::is_const_v<std::remove_reference_t<decltype(span[0])>>);
    }

    SECTION("const iteration") {
        auto buffer = make_immutable<float>(10);
        std::size_t count = 0;
        for (const auto& val : buffer) {
            (void)val;
            count++;
        }
        REQUIRE(count == 10);
    }

    SECTION("sharing creates new reference") {
        auto buffer = make_immutable<float>(10);
        auto shared = buffer.share();

        REQUIRE(shared.size() == buffer.size());
        REQUIRE(!buffer.is_unique()); // Now has multiple references
    }

    SECTION("uniqueness check") {
        auto buffer = make_immutable<float>(10);
        REQUIRE(buffer.is_unique());

        auto shared = buffer.share();
        REQUIRE(!buffer.is_unique());
        REQUIRE(!shared.is_unique());
    }
}

TEST_CASE("wrap custom containers", "[buffer][container]") {
    SECTION("wrap std::vector") {
        auto vec = std::make_unique<std::vector<float>>(20, 3.14f);
        auto buffer = wrap_mutable(std::move(vec));

        REQUIRE(buffer.size() == 20);
        REQUIRE_THAT(buffer[0], Catch::Matchers::WithinAbs(3.14f, 0.001f));
    }

    SECTION("wrap another std::vector") {
        auto vec = std::make_unique<std::vector<float>>(15, 2.71f);
        auto buffer = wrap_mutable(std::move(vec));

        REQUIRE(buffer.size() == 15);
        REQUIRE_THAT(buffer[0], Catch::Matchers::WithinAbs(2.71f, 0.001f));
    }
}

TEST_CASE("buffer empty, has_data, and operator bool semantics", "[buffer][semantics]") {
    SECTION("default constructed immutable buffer") {
        immutable_buffer<float> buf;

        REQUIRE(buf.empty());          // No elements
        REQUIRE_FALSE(buf.has_data()); // No storage
        REQUIRE_FALSE(buf);            // Not usable (operator bool)
        REQUIRE(buf.size() == 0);
    }

    SECTION("zero-size immutable buffer") {
        auto buf = make_immutable<float>(0);

        REQUIRE(buf.empty());    // No elements
        REQUIRE(buf.has_data()); // Has storage allocation
        REQUIRE_FALSE(buf);      // Not usable (no elements)
        REQUIRE(buf.size() == 0);
    }

    SECTION("non-empty immutable buffer") {
        auto buf = make_immutable<float>(10);

        REQUIRE_FALSE(buf.empty()); // Has elements
        REQUIRE(buf.has_data());    // Has storage
        REQUIRE(buf);               // Usable (operator bool)
        REQUIRE(buf.size() == 10);
    }

    SECTION("shared immutable buffer") {
        auto buf1 = make_immutable<float>(5);
        auto buf2 = buf1.share();

        // Both should have same semantics
        REQUIRE_FALSE(buf1.empty());
        REQUIRE(buf1.has_data());
        REQUIRE(buf1);

        REQUIRE_FALSE(buf2.empty());
        REQUIRE(buf2.has_data());
        REQUIRE(buf2);
    }

    SECTION("default constructed mutable buffer") {
        mutable_buffer<float> buf;

        REQUIRE(buf.empty());          // No elements
        REQUIRE_FALSE(buf.has_data()); // No storage
        REQUIRE_FALSE(buf);            // Not usable (operator bool)
        REQUIRE(buf.size() == 0);
    }

    SECTION("zero-size mutable buffer") {
        auto buf = make_mutable<float>(0);

        REQUIRE(buf.empty());    // No elements
        REQUIRE(buf.has_data()); // Has storage allocation
        REQUIRE_FALSE(buf);      // Not usable (no elements)
        REQUIRE(buf.size() == 0);
    }

    SECTION("non-empty mutable buffer") {
        auto buf = make_mutable<float>(10);

        REQUIRE_FALSE(buf.empty()); // Has elements
        REQUIRE(buf.has_data());    // Has storage
        REQUIRE(buf);               // Usable (operator bool)
        REQUIRE(buf.size() == 10);
    }

    SECTION("moved-from mutable buffer") {
        auto buf1 = make_mutable<float>(10);
        auto buf2 = std::move(buf1);

        // buf2 should be valid
        REQUIRE_FALSE(buf2.empty());
        REQUIRE(buf2.has_data());
        REQUIRE(buf2);
        REQUIRE(buf2.size() == 10);

        // buf1 is moved-from - should be in default state
        REQUIRE(buf1.empty());
        REQUIRE_FALSE(buf1.has_data());
        REQUIRE_FALSE(buf1);
        REQUIRE(buf1.size() == 0);
    }

    SECTION("copied mutable buffer") {
        auto buf1 = make_mutable<float>(8);
        for (std::size_t i = 0; i < buf1.size(); ++i) {
            buf1[i] = static_cast<float>(i);
        }

        auto buf2 = buf1.copy();

        // Both should be valid
        REQUIRE_FALSE(buf1.empty());
        REQUIRE(buf1.has_data());
        REQUIRE(buf1);

        REQUIRE_FALSE(buf2.empty());
        REQUIRE(buf2.has_data());
        REQUIRE(buf2);

        // Independent data
        buf2[0] = 999.0f;
        REQUIRE_THAT(buf1[0], Catch::Matchers::WithinAbs(0.0f, 0.001f));
        REQUIRE_THAT(buf2[0], Catch::Matchers::WithinAbs(999.0f, 0.001f));
    }

    SECTION("mutable to immutable conversion") {
        auto mut = make_mutable<float>(5);
        for (std::size_t i = 0; i < mut.size(); ++i) {
            mut[i] = static_cast<float>(i * 2);
        }

        auto imm = std::move(mut).to_immutable();

        // Immutable should be valid
        REQUIRE_FALSE(imm.empty());
        REQUIRE(imm.has_data());
        REQUIRE(imm);
        REQUIRE(imm.size() == 5);

        // Mutable is moved-from
        REQUIRE(mut.empty());
        REQUIRE_FALSE(mut.has_data());
        REQUIRE_FALSE(mut);
    }

    SECTION("conditional usage pattern") {
        auto buf = make_mutable<float>(100);

        // Idiomatic usage with operator bool
        if (buf) {
            // Safe to use buffer
            buf[0] = 42.0f;
            REQUIRE_THAT(buf[0], Catch::Matchers::WithinAbs(42.0f, 0.001f));
        } else {
            FAIL("Buffer should be usable");
        }

        // Move it away
        auto buf2 = std::move(buf);

        // Now buf is not usable
        if (!buf) {
            // This is expected
            REQUIRE(true);
        } else {
            FAIL("Moved-from buffer should not be usable");
        }
    }
}

// ============================================================================
// Port Connection Tests
// ============================================================================

TEST_CASE("mutable to mutable connection", "[port][connection]") {
    auto source = std::make_shared<TestMutableSource>();
    auto sink = std::make_shared<TestMutableSink>();

    source->m_size = 5;
    source->m_multiplier = 2.0f;

    REQUIRE(source->connect("data_out", sink, "data_in"));

    // Run components
    REQUIRE(source->process() == retval::NORMAL);
    REQUIRE(sink->process() == retval::FINISH);

    // Verify data transfer
    REQUIRE(sink->m_received);
    REQUIRE(sink->m_size == 5);
    REQUIRE(sink->m_values.size() == 5);

    for (std::size_t i = 0; i < sink->m_values.size(); ++i) {
        REQUIRE_THAT(sink->m_values[i], Catch::Matchers::WithinAbs(i * 2.0f, 0.001f));
    }
}

TEST_CASE("component connect/disconnect bookkeeping", "[port][connection]") {
    auto source = std::make_shared<TestMutableSource>();
    auto sink = std::make_shared<TestMutableSink>();

    REQUIRE(source->connections().empty());
    REQUIRE(source->connect("data_out", sink, "data_in"));
    REQUIRE(source->connections().size() == 1);
    REQUIRE(source->connections()[0].output.second == "data_out");
    REQUIRE(source->connections()[0].input.first == sink->id());

    // Parked disconnect (no worker running -> with_worker_parked inline path):
    // tears down the port wiring AND removes the bookkeeping record.
    REQUIRE(source->disconnect("data_out", sink, "data_in"));
    REQUIRE(source->connections().empty());
    // already disconnected -> false, bookkeeping unchanged
    REQUIRE_FALSE(source->disconnect("data_out", sink, "data_in"));

    // the input's producer claim was released, so a reconnect succeeds...
    REQUIRE(source->connect("data_out", sink, "data_in"));
    REQUIRE(source->connections().size() == 1);
    // ...and disconnect_all clears the record and releases the claim again.
    REQUIRE(source->disconnect_all("data_out") == 1);
    REQUIRE(source->connections().empty());
    REQUIRE(source->connect("data_out", sink, "data_in")); // re-claimable
    REQUIRE(source->connections().size() == 1);
}

TEST_CASE("immutable to immutable connection", "[port][connection]") {
    auto source = std::make_shared<TestImmutableSource>();
    auto sink = std::make_shared<TestImmutableSink>();

    source->m_size = 7;
    source->m_multiplier = 3.0f;

    REQUIRE(source->connect("data_out", sink, "data_in"));

    // Run components
    REQUIRE(source->process() == retval::NORMAL);
    REQUIRE(sink->process() == retval::FINISH);

    // Verify data transfer
    REQUIRE(sink->m_received);
    REQUIRE(sink->m_size == 7);
    REQUIRE(sink->m_values.size() == 7);

    for (std::size_t i = 0; i < sink->m_values.size(); ++i) {
        REQUIRE_THAT(sink->m_values[i], Catch::Matchers::WithinAbs(i * 3.0f, 0.001f));
    }
}

TEST_CASE("immutable batch moves ownership and aggregates overflow", "[port][batch][immutable]") {
    output_port<immutable_buffer<std::uint8_t>> out{"out"};
    input_port<immutable_buffer<std::uint8_t>> in{"in", 2};
    REQUIRE(out.connect(&in));

    std::size_t callback_calls = 0;
    std::size_t callback_drops = 0;
    in.set_overflow_callback([&](std::size_t count) {
        ++callback_calls;
        callback_drops += count;
    });

    metadata md_value;
    md_value.sample_rate = 2.5e6;
    const auto md = make_metadata(std::move(md_value));
    std::vector<immutable_buffer<std::uint8_t>> buffers;
    for (std::uint8_t value = 1; value <= 4; ++value) {
        buffers.push_back(make_immutable<std::uint8_t>({value}));
    }

    out.send_batch(std::span{buffers}, timestamp{}, md);

    // The complete source span is consumed, including the bounded ring's
    // rejected suffix. Overflow is reported once with the aggregate count.
    REQUIRE(std::ranges::all_of(buffers, [](const auto& b) { return !b.has_data(); }));
    REQUIRE(callback_calls == 1);
    REQUIRE(callback_drops == 2);

    std::array<decltype(in)::queue_type, 4> received;
    const auto count = in.get_batch(std::span{received});
    REQUIRE(count == 2);
    REQUIRE(std::get<0>(received[0])[0] == 1);
    REQUIRE(std::get<0>(received[1])[0] == 2);
    REQUIRE(std::get<2>(received[0]) == md);
    REQUIRE(std::get<2>(received[1]) == md);
}

TEST_CASE("immutable batch consumes every buffer when entirely rejected", "[port][batch][immutable]") {
    output_port<immutable_buffer<std::uint8_t>> out{"out"};
    input_port<immutable_buffer<std::uint8_t>> in{"in", 2};
    REQUIRE(out.connect(&in));

    out.send_data(make_immutable<std::uint8_t>({10}), timestamp{});
    out.send_data(make_immutable<std::uint8_t>({11}), timestamp{});
    REQUIRE(in.is_full());

    std::size_t callback_calls = 0;
    std::size_t callback_drops = 0;
    in.set_overflow_callback([&](std::size_t count) {
        ++callback_calls;
        callback_drops += count;
    });

    std::vector<immutable_buffer<std::uint8_t>> rejected;
    for (std::uint8_t value = 1; value <= 3; ++value) {
        rejected.push_back(make_immutable<std::uint8_t>({value}));
    }
    out.send_batch(std::span{rejected}, timestamp{});

    REQUIRE(std::ranges::all_of(rejected, [](const auto& b) { return !b.has_data(); }));
    REQUIRE(callback_calls == 1);
    REQUIRE(callback_drops == rejected.size());

    std::array<decltype(in)::queue_type, 2> received;
    REQUIRE(in.get_batch(std::span{received}) == 2);
    REQUIRE(std::get<0>(received[0])[0] == 10);
    REQUIRE(std::get<0>(received[1])[0] == 11);
}

TEST_CASE("mutable to immutable connection (promotion)", "[port][connection]") {
    auto source = std::make_shared<TestMutableSource>();
    auto sink = std::make_shared<TestImmutableSink>();

    source->m_size = 6;

    REQUIRE(source->connect("data_out", sink, "data_in"));

    // Run components
    REQUIRE(source->process() == retval::NORMAL);
    REQUIRE(sink->process() == retval::FINISH);

    // Verify data transfer (mutable promoted to immutable)
    REQUIRE(sink->m_received);
    REQUIRE(sink->m_size == 6);
}

TEST_CASE("immutable to mutable connection (copy)", "[port][connection]") {
    auto source = std::make_shared<TestImmutableSource>();
    auto sink = std::make_shared<TestMutableSink>();

    source->m_size = 8;

    REQUIRE(source->connect("data_out", sink, "data_in"));

    // Run components
    REQUIRE(source->process() == retval::NORMAL);
    REQUIRE(sink->process() == retval::FINISH);

    // Verify data transfer (immutable copied to mutable)
    REQUIRE(sink->m_received);
    REQUIRE(sink->m_size == 8);
}

// ============================================================================
// Processing Chain Tests
// ============================================================================

TEST_CASE("mutable processing chain (zero copy)", "[port][chain]") {
    auto source = std::make_shared<TestMutableSource>();
    auto amp1 = std::make_shared<TestAmplifier>();
    auto amp2 = std::make_shared<TestAmplifier>();
    auto sink = std::make_shared<TestMutableSink>();

    source->m_size = 10;
    source->m_multiplier = 1.0f;
    amp1->m_gain = 2.0f;
    amp2->m_gain = 3.0f;

    REQUIRE(source->connect("data_out", amp1, "data_in"));
    REQUIRE(amp1->connect("data_out", amp2, "data_in"));
    REQUIRE(amp2->connect("data_out", sink, "data_in"));

    // Run pipeline
    REQUIRE(source->process() == retval::NORMAL);
    REQUIRE(amp1->process() == retval::FINISH);
    REQUIRE(amp2->process() == retval::FINISH);
    REQUIRE(sink->process() == retval::FINISH);

    // Verify final result (1.0 * 2.0 * 3.0 = 6.0 multiplier)
    REQUIRE(amp1->m_processed);
    REQUIRE(amp2->m_processed);
    REQUIRE(sink->m_received);
    REQUIRE(sink->m_size == 10);

    for (std::size_t i = 0; i < sink->m_values.size(); ++i) {
        REQUIRE_THAT(sink->m_values[i], Catch::Matchers::WithinAbs(i * 6.0f, 0.001f));
    }
}

// ============================================================================
// Fan-out Tests
// ============================================================================

TEST_CASE("immutable fan-out (zero copy)", "[port][fanout]") {
    auto source = std::make_shared<TestImmutableSource>();
    auto sink1 = std::make_shared<TestImmutableSink>();
    auto sink2 = std::make_shared<TestImmutableSink>();
    auto sink3 = std::make_shared<TestImmutableSink>();

    source->m_size = 10;
    source->m_multiplier = 1.0f;

    // Connect to three sinks
    REQUIRE(source->connect("data_out", sink1, "data_in"));
    REQUIRE(source->connect("data_out", sink2, "data_in"));
    REQUIRE(source->connect("data_out", sink3, "data_in"));

    // Run source (synchronous: send_data delivers into each sink's ring immediately)
    REQUIRE(source->process() == retval::NORMAL);

    // Run all sinks
    REQUIRE(sink1->process() == retval::FINISH);
    REQUIRE(sink2->process() == retval::FINISH);
    REQUIRE(sink3->process() == retval::FINISH);

    // Verify all received same data
    REQUIRE(sink1->m_received);
    REQUIRE(sink2->m_received);
    REQUIRE(sink3->m_received);

    REQUIRE(sink1->m_size == 10);
    REQUIRE(sink2->m_size == 10);
    REQUIRE(sink3->m_size == 10);

    // All should have identical values
    for (std::size_t i = 0; i < 10; ++i) {
        auto expected = static_cast<float>(i);
        REQUIRE_THAT(sink1->m_values[i], Catch::Matchers::WithinAbs(expected, 0.001f));
        REQUIRE_THAT(sink2->m_values[i], Catch::Matchers::WithinAbs(expected, 0.001f));
        REQUIRE_THAT(sink3->m_values[i], Catch::Matchers::WithinAbs(expected, 0.001f));
    }
}

TEST_CASE("immutable fan-out delivers metadata to every receiver (moved into the last)", "[port][fanout][metadata]") {
    // send_data moves the metadata into the LAST receiver and copies it into the earlier
    // ones; verify NONE are dropped — all three inputs must see the same metadata.
    output_port<immutable_buffer<float>> out{"out"};
    input_port<immutable_buffer<float>> in1{"in1", 4};
    input_port<immutable_buffer<float>> in2{"in2", 4};
    input_port<immutable_buffer<float>> in3{"in3", 4};
    REQUIRE(out.connect(&in1));
    REQUIRE(out.connect(&in2));
    REQUIRE(out.connect(&in3));

    metadata md;
    md.center_frequency = 2.4e9;
    md.sample_rate = 1e6;
    md.eos = true;
    out.send_data(make_immutable<float>({1.0f, 2.0f, 3.0f}), timestamp{}, md);

    for (auto* in : {&in1, &in2, &in3}) {
        auto [buf, ts, rmd] = in->get_data();
        REQUIRE(buf.size() == 3);
        REQUIRE(rmd != nullptr);
        REQUIRE(rmd->center_frequency == 2.4e9);
        REQUIRE(rmd->sample_rate == 1e6);
        REQUIRE(rmd->eos == true);
    }
}

TEST_CASE("broadcaster pattern (mutable to immutable fan-out)", "[port][broadcast]") {
    auto source = std::make_shared<TestMutableSource>();
    auto broadcaster = std::make_shared<TestBroadcaster>();
    auto sink1 = std::make_shared<TestImmutableSink>();
    auto sink2 = std::make_shared<TestImmutableSink>();
    auto sink3 = std::make_shared<TestImmutableSink>();

    source->m_size = 12;
    source->m_multiplier = 1.0f;

    // Connect pipeline
    REQUIRE(source->connect("data_out", broadcaster, "data_in"));
    REQUIRE(broadcaster->connect("data_out1", sink1, "data_in"));
    REQUIRE(broadcaster->connect("data_out2", sink2, "data_in"));
    REQUIRE(broadcaster->connect("data_out3", sink3, "data_in"));

    // Run pipeline
    REQUIRE(source->process() == retval::NORMAL);
    REQUIRE(broadcaster->process() == retval::FINISH);
    REQUIRE(sink1->process() == retval::FINISH);
    REQUIRE(sink2->process() == retval::FINISH);
    REQUIRE(sink3->process() == retval::FINISH);

    // Verify broadcasting occurred
    REQUIRE(broadcaster->m_broadcasted);
    REQUIRE(sink1->m_received);
    REQUIRE(sink2->m_received);
    REQUIRE(sink3->m_received);

    // All sinks should have identical data
    for (std::size_t i = 0; i < 12; ++i) {
        auto expected = static_cast<float>(i);
        REQUIRE_THAT(sink1->m_values[i], Catch::Matchers::WithinAbs(expected, 0.001f));
        REQUIRE_THAT(sink2->m_values[i], Catch::Matchers::WithinAbs(expected, 0.001f));
        REQUIRE_THAT(sink3->m_values[i], Catch::Matchers::WithinAbs(expected, 0.001f));
    }
}

// ============================================================================
// Port Depth Tests
// ============================================================================

TEST_CASE("input port depth limiting", "[port][depth]") {
    auto source = std::make_shared<TestMutableSource>();
    auto sink = std::make_shared<TestMutableSink>();

    REQUIRE(source->connect("data_out", sink, "data_in"));

    // Set depth to 0 (should drop all data)
    auto* input_port = sink->get_port<input_port_base>("data_in");
    REQUIRE(input_port != nullptr);
    input_port->depth(0);

    // Send data (dropped synchronously at add_data because depth==0)
    REQUIRE(source->process() == retval::NORMAL);

    // Try to receive (nothing was queued)
    REQUIRE(sink->process() == retval::NOOP);
    REQUIRE(!sink->m_received);
}

TEST_CASE("input port depth queuing", "[port][depth]") {
    // This test would need a modified source that sends multiple times
    // Placeholder for future implementation
    REQUIRE(true);
}

// ============================================================================
// Metadata Tests
// ============================================================================

TEST_CASE("metadata is shared by pointer across packets and fan-out", "[port][metadata]") {
    output_port<immutable_buffer<float>> out{"out"};
    input_port<immutable_buffer<float>> in1{"in1"};
    input_port<immutable_buffer<float>> in2{"in2"};
    REQUIRE(out.connect(&in1));
    REQUIRE(out.connect(&in2));

    metadata md;
    md.sample_rate = 1e6;
    md.annotations["protocol"] = "test";
    const auto shared = make_metadata(std::move(md));

    // The producer latches one instance and attaches it to every packet.
    out.send_data(make_immutable<float>({1.0f}), timestamp{}, shared);
    out.send_data(make_immutable<float>({2.0f}), timestamp{}, shared);

    // Every packet on every receiver carries the SAME instance — no copies were made.
    auto [b1, t1, m1] = in1.get_data();
    auto [b2, t2, m2] = in1.get_data();
    auto [c1, u1, n1] = in2.get_data();
    auto [c2, u2, n2] = in2.get_data();
    REQUIRE(m1.get() == shared.get());
    REQUIRE(m2.get() == shared.get());
    REQUIRE(n1.get() == shared.get());
    REQUIRE(n2.get() == shared.get());

    // Consumers detect "unchanged" by pointer identity.
    REQUIRE(m1 == m2);
    REQUIRE(m1->sample_rate == 1e6);
}

TEST_CASE("metadata propagation", "[port][metadata]") {
    SECTION("basic metadata send and receive") {
        auto source = std::make_shared<TestMetadataSource>();
        auto sink = std::make_shared<TestMetadataSink>();

        source->m_size = 100;
        source->m_metadata_cf = 2.4e9;
        source->m_metadata_bw = 20e6;
        source->m_metadata_sr = 10e6;
        source->m_annotation_key = "test_key";
        source->m_annotation_value = "test_value";

        REQUIRE(source->connect("data_out", sink, "data_in"));

        // Run components
        REQUIRE(source->process() == retval::NORMAL);
        REQUIRE(sink->process() == retval::FINISH);

        // Verify data received
        REQUIRE(sink->m_received);
        REQUIRE(sink->m_size == 100);

        // Verify metadata received
        REQUIRE(sink->m_metadata != nullptr);
        const auto& md = *sink->m_metadata;
        REQUIRE_THAT(md.center_frequency, Catch::Matchers::WithinAbs(2.4e9, 1.0));
        REQUIRE_THAT(md.bandwidth, Catch::Matchers::WithinAbs(20e6, 1.0));
        REQUIRE_THAT(md.sample_rate, Catch::Matchers::WithinAbs(10e6, 1.0));
        REQUIRE(md.format.type == data_type::floating_point);
        REQUIRE(md.format.bit_width == 32);
        REQUIRE(md.annotations.contains("test_key"));
        REQUIRE(md.annotations.at("test_key") == "test_value");
    }

    SECTION("metadata without data is discarded") {
        auto source = std::make_shared<TestMetadataSource>();
        auto sink = std::make_shared<TestMetadataSink>();

        source->m_size = 0; // Send no data
        source->m_metadata_cf = 1.0e9;

        REQUIRE(source->connect("data_out", sink, "data_in"));

        REQUIRE(source->process() == retval::NORMAL);
        REQUIRE(sink->process() == retval::NOOP); // No data

        // No metadata should be received without data
        REQUIRE_FALSE(sink->m_received);
        REQUIRE_FALSE(sink->m_metadata != nullptr);
    }

    SECTION("data without metadata") {
        auto source = std::make_shared<TestMutableSource>();
        auto sink = std::make_shared<TestMetadataSink>();

        source->m_size = 50;

        REQUIRE(source->connect("data_out", sink, "data_in"));

        REQUIRE(source->process() == retval::NORMAL);
        REQUIRE(sink->process() == retval::FINISH);

        // Data received but no metadata
        REQUIRE(sink->m_received);
        REQUIRE(sink->m_size == 50);
        REQUIRE_FALSE(sink->m_metadata != nullptr);
    }

    SECTION("metadata propagation through chain") {
        auto source = std::make_shared<TestMetadataSource>();
        auto passthrough = std::make_shared<TestMetadataPassthrough>();
        auto sink = std::make_shared<TestMetadataSink>();

        source->m_size = 75;
        source->m_metadata_cf = 5.8e9;
        source->m_metadata_sr = 50e6;
        source->m_metadata_eos = true;

        REQUIRE(source->connect("data_out", passthrough, "data_in"));
        REQUIRE(passthrough->connect("data_out", sink, "data_in"));

        // Run pipeline
        REQUIRE(source->process() == retval::NORMAL);
        REQUIRE(passthrough->process() == retval::FINISH);
        REQUIRE(sink->process() == retval::FINISH);

        // Verify passthrough received metadata
        REQUIRE(passthrough->m_processed);
        REQUIRE(passthrough->m_received_metadata != nullptr);

        // Verify sink received metadata
        REQUIRE(sink->m_received);
        REQUIRE(sink->m_metadata != nullptr);
        const auto& md = *sink->m_metadata;
        REQUIRE_THAT(md.center_frequency, Catch::Matchers::WithinAbs(5.8e9, 1.0));
        REQUIRE_THAT(md.sample_rate, Catch::Matchers::WithinAbs(50e6, 1.0));
        REQUIRE(md.eos == true);
    }

    SECTION("metadata modification in chain") {
        auto source = std::make_shared<TestMetadataSource>();
        auto modifier = std::make_shared<TestMetadataModifier>();
        auto sink = std::make_shared<TestMetadataSink>();

        source->m_size = 50;
        source->m_metadata_cf = 1.0e9;
        modifier->m_cf_offset = 100e6; // Add 100 MHz

        REQUIRE(source->connect("data_out", modifier, "data_in"));
        REQUIRE(modifier->connect("data_out", sink, "data_in"));

        // Run pipeline
        REQUIRE(source->process() == retval::NORMAL);
        REQUIRE(modifier->process() == retval::FINISH);
        REQUIRE(sink->process() == retval::FINISH);

        // Verify metadata was modified
        REQUIRE(sink->m_metadata != nullptr);
        const auto& md = *sink->m_metadata;
        REQUIRE_THAT(md.center_frequency, Catch::Matchers::WithinAbs(1.1e9, 1.0));
        REQUIRE(md.annotations.contains("modified_by"));
        REQUIRE(md.annotations.at("modified_by") == "TestMetadataModifier");
    }

    SECTION("metadata with eos flag") {
        auto source = std::make_shared<TestMetadataSource>();
        auto sink = std::make_shared<TestMetadataSink>();

        source->m_size = 10;
        source->m_metadata_eos = true; // End of stream

        REQUIRE(source->connect("data_out", sink, "data_in"));

        REQUIRE(source->process() == retval::NORMAL);
        REQUIRE(sink->process() == retval::FINISH);

        REQUIRE(sink->m_metadata != nullptr);
        REQUIRE(sink->m_metadata->eos == true);
    }

    SECTION("complex vs real data format") {
        auto source = std::make_shared<TestMetadataSource>();
        auto sink = std::make_shared<TestMetadataSink>();

        source->m_size = 20;
        source->m_metadata_format_complex = true;

        REQUIRE(source->connect("data_out", sink, "data_in"));

        REQUIRE(source->process() == retval::NORMAL);
        REQUIRE(sink->process() == retval::FINISH);

        REQUIRE(sink->m_metadata != nullptr);
        REQUIRE(sink->m_metadata->format.is_complex == true);
    }

    SECTION("multiple annotations") {
        auto source = std::make_shared<TestMetadataSource>();
        auto modifier = std::make_shared<TestMetadataModifier>();
        auto sink = std::make_shared<TestMetadataSink>();

        source->m_size = 30;
        source->m_annotation_key = "source_info";
        source->m_annotation_value = "test_source";
        modifier->m_cf_offset = 0.0; // No frequency change

        REQUIRE(source->connect("data_out", modifier, "data_in"));
        REQUIRE(modifier->connect("data_out", sink, "data_in"));

        REQUIRE(source->process() == retval::NORMAL);
        REQUIRE(modifier->process() == retval::FINISH);
        REQUIRE(sink->process() == retval::FINISH);

        // Should have both source and modifier annotations
        REQUIRE(sink->m_metadata != nullptr);
        const auto& md = *sink->m_metadata;
        REQUIRE(md.annotations.size() == 2);
        REQUIRE(md.annotations.contains("source_info"));
        REQUIRE(md.annotations.contains("modified_by"));
    }
}

// ============================================================================
// Error Handling Tests
// ============================================================================

TEST_CASE("connection with invalid port names", "[port][error]") {
    auto source = std::make_shared<TestMutableSource>();
    auto sink = std::make_shared<TestMutableSink>();

    REQUIRE(!source->connect("invalid_port", sink, "data_in"));
    REQUIRE(!source->connect("data_out", sink, "invalid_port"));
}

TEST_CASE("empty buffer handling", "[buffer][error]") {
    auto buffer = make_mutable<float>(0);
    REQUIRE(buffer.size() == 0);

    // Should be safe to use
    auto span = buffer.as_span();
    REQUIRE(span.size() == 0);
}

TEST_CASE("to_immutable error handling", "[buffer][error]") {
    SECTION("empty buffer converts safely") {
        auto buf = make_mutable<float>(0);
        auto imm = std::move(buf).to_immutable();

        REQUIRE(imm.empty());
        REQUIRE_FALSE(imm.has_data());
    }

    SECTION("default constructed buffer converts safely") {
        mutable_buffer<float> buf;
        auto imm = std::move(buf).to_immutable();

        REQUIRE(imm.empty());
        REQUIRE_FALSE(imm.has_data());
    }

    SECTION("moved-from buffer converts safely") {
        auto buf1 = make_mutable<float>(10);
        auto buf2 = std::move(buf1);

        // buf1 is moved-from, should convert to empty immutable
        auto imm = std::move(buf1).to_immutable();

        REQUIRE(imm.empty());
        REQUIRE_FALSE(imm.has_data());
    }

    SECTION("normal conversion works") {
        auto mut = make_mutable<float>(100);
        for (std::size_t i = 0; i < mut.size(); ++i) {
            mut[i] = static_cast<float>(i);
        }

        // Should not throw
        REQUIRE_NOTHROW([&]() {
            auto imm = std::move(mut).to_immutable();
            REQUIRE(imm.size() == 100);
            REQUIRE(imm.has_data());
            REQUIRE_THAT(imm[50], Catch::Matchers::WithinAbs(50.0f, 0.001f));
        }());
    }

    SECTION("large buffer converts safely") {
        // Test with large but reasonable size
        auto mut = make_mutable<float>(1000000); // 1 million floats

        // Should not throw - no overflow
        REQUIRE_NOTHROW([&]() {
            auto imm = std::move(mut).to_immutable();
            REQUIRE(imm.size() == 1000000);
            REQUIRE(imm.has_data());
        }());
    }
}

TEST_CASE("buffer out_of_range handling", "[buffer][error]") {
    SECTION("mutable buffer at() bounds checking") {
        auto buf = make_mutable<float>(10);

        REQUIRE_NOTHROW(buf.at(0));
        REQUIRE_NOTHROW(buf.at(9));
        REQUIRE_THROWS_AS(buf.at(10), std::out_of_range);
        REQUIRE_THROWS_AS(buf.at(100), std::out_of_range);
    }

    SECTION("immutable buffer at() bounds checking") {
        auto buf = make_immutable<float>(10);

        REQUIRE_NOTHROW(buf.at(0));
        REQUIRE_NOTHROW(buf.at(9));
        REQUIRE_THROWS_AS(buf.at(10), std::out_of_range);
        REQUIRE_THROWS_AS(buf.at(100), std::out_of_range);
    }
}

// ============================================================================
// Performance Tests
// ============================================================================

TEST_CASE("large buffer transfer", "[port][performance]") {
    auto source = std::make_shared<TestMutableSource>();
    auto sink = std::make_shared<TestMutableSink>();

    source->m_size = 1000000; // 1 million elements

    REQUIRE(source->connect("data_out", sink, "data_in"));

    REQUIRE(source->process() == retval::NORMAL);
    REQUIRE(sink->process() == retval::FINISH);

    // Correctness only: a large buffer transfers intact. The wall-clock threshold that used
    // to gate this (duration < 1s) was removed — a fixed ms bound flakes under sanitizers /
    // parallel CI oversubscription; throughput is measured in bench_datapath, not asserted
    // in the correctness suite.
    REQUIRE(sink->m_received);
    REQUIRE(sink->m_size == 1000000);
}

TEST_CASE("repeated buffer creation and destruction", "[buffer][performance]") {
    constexpr std::size_t iterations = 1000;

    // Exercise repeated allocate / fill / free — a no-leak, no-corruption smoke (meaningful
    // under ASan). Assert correctness of the produced data rather than a wall-clock bound:
    // the old `duration < 1s` threshold flaked under sanitizers / CI oversubscription.
    float last = 0.0F;
    for (std::size_t i = 0; i < iterations; ++i) {
        auto buffer = make_mutable<float>(1000);
        for (std::size_t j = 0; j < buffer.size(); ++j) {
            buffer[j] = static_cast<float>(j);
        }
        last = buffer[buffer.size() - 1];
        // Buffer destroyed here
    }
    REQUIRE(last == static_cast<float>(999));
}

// ============================================================================
// Integration Tests
// ============================================================================

TEST_CASE("complete pipeline integration", "[integration]") {
    application app{"test_app"};

    auto source = std::make_shared<TestMutableSource>();
    auto amp = std::make_shared<TestAmplifier>();
    auto broadcaster = std::make_shared<TestBroadcaster>();
    auto sink1 = std::make_shared<TestImmutableSink>("sink1");
    auto sink2 = std::make_shared<TestImmutableSink>("sink2");
    auto sink3 = std::make_shared<TestImmutableSink>("sink3");

    source->m_size = 20;
    amp->m_gain = 5.0f;

    app.add_component(source);
    app.add_component(amp);
    app.add_component(broadcaster);
    app.add_component(sink1);
    app.add_component(sink2);
    app.add_component(sink3);

    REQUIRE(source->connect("data_out", amp, "data_in"));
    REQUIRE(amp->connect("data_out", broadcaster, "data_in"));
    REQUIRE(broadcaster->connect("data_out1", sink1, "data_in"));
    REQUIRE(broadcaster->connect("data_out2", sink2, "data_in"));
    REQUIRE(broadcaster->connect("data_out3", sink3, "data_in"));

    // Initialize and start
    app.initialize();
    app.start();

    // Give time for processing
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Stop
    app.stop();

    // Verify all components processed
    REQUIRE(amp->m_processed);
    REQUIRE(broadcaster->m_broadcasted);
    REQUIRE(sink1->m_received);
    REQUIRE(sink2->m_received);
    REQUIRE(sink3->m_received);

    // Verify data (amplified by 5.0)
    for (std::size_t i = 0; i < 20; ++i) {
        auto expected = static_cast<float>(i) * 5.0f;
        REQUIRE_THAT(sink1->m_values[i], Catch::Matchers::WithinAbs(expected, 0.001f));
        REQUIRE_THAT(sink2->m_values[i], Catch::Matchers::WithinAbs(expected, 0.001f));
        REQUIRE_THAT(sink3->m_values[i], Catch::Matchers::WithinAbs(expected, 0.001f));
    }

    app.clear();
}

// ============================================================================
// Type Safety Tests
// ============================================================================

/**
 * @brief Test component with different element type
 */
class TestIntSource : public component {
public:
    TestIntSource() : component("TestIntSource") { add_port(m_output); }

    auto process() -> retval override {
        auto buffer = make_mutable<int32_t>(10);
        for (std::size_t i = 0; i < buffer.size(); ++i) {
            buffer[i] = static_cast<int32_t>(i);
        }
        m_output.send_data(std::move(buffer), timestamp{});
        return retval::FINISH;
    }

private:
    output_port<mutable_buffer<int32_t>> m_output{"data_out"};
};

TEST_CASE("type mismatch detection", "[port][type_safety]") {
    auto int_source = std::make_shared<TestIntSource>();
    auto float_sink = std::make_shared<TestMutableSink>();

    // Should fail: int32_t output cannot connect to float input
    REQUIRE(!int_source->connect("data_out", float_sink, "data_in"));
}

TEST_CASE("same type compatibility", "[port][type_safety]") {
    auto float_source = std::make_shared<TestMutableSource>();
    auto float_sink = std::make_shared<TestMutableSink>();

    // Should succeed: both are float
    REQUIRE(float_source->connect("data_out", float_sink, "data_in"));
}

// ============================================================================
// Concurrent Access Tests
// ============================================================================

TEST_CASE("concurrent port access", "[port][concurrent]") {
    auto source = std::make_shared<TestMutableSource>();
    auto sink1 = std::make_shared<TestMutableSink>();
    auto sink2 = std::make_shared<TestMutableSink>();

    source->m_size = 100;

    REQUIRE(source->connect("data_out", sink1, "data_in"));
    REQUIRE(source->connect("data_out", sink2, "data_in"));

    // Start components in separate threads
    std::thread source_thread([&]() { source->process(); });

    // Poll process() until the source (running on its own thread) has delivered — avoids a
    // fixed-sleep race where a sink could run process() before the source sends.
    std::thread sink1_thread([&]() { wait_until([&] { return sink1->process() != retval::NOOP; }); });

    std::thread sink2_thread([&]() { wait_until([&] { return sink2->process() != retval::NOOP; }); });

    source_thread.join();
    sink1_thread.join();
    sink2_thread.join();

    // Both should receive data (copies made for fan-out)
    REQUIRE(sink1->m_received);
    REQUIRE(sink2->m_received);
}

// ============================================================================
// Buffer Lifetime Tests
// ============================================================================

TEST_CASE("mutable buffer move semantics", "[buffer][lifetime]") {
    auto buffer1 = make_mutable<float>(10);
    buffer1[0] = 42.0f;

    // Move to buffer2
    auto buffer2 = std::move(buffer1);

    REQUIRE(buffer2.size() == 10);
    REQUIRE_THAT(buffer2[0], Catch::Matchers::WithinAbs(42.0f, 0.001f));

    // buffer1 is now moved-from (implementation-defined state, but should be safe)
    // Just verify it doesn't crash to access
    (void)buffer1.size();
}

TEST_CASE("immutable buffer copy semantics", "[buffer][lifetime]") {
    auto buffer1 = make_immutable<float>(10);
    auto* ptr1 = buffer1.data();

    // Copy to buffer2 (shallow copy - shares data)
    auto buffer2 = buffer1;
    auto* ptr2 = buffer2.data();

    REQUIRE(buffer1.size() == buffer2.size());
    REQUIRE(ptr1 == ptr2); // Same underlying data
    REQUIRE(!buffer1.is_unique());
    REQUIRE(!buffer2.is_unique());
}

TEST_CASE("buffer scope and destruction", "[buffer][lifetime]") {
    std::vector<float> reference;

    {
        auto buffer = make_mutable<float>(10);
        for (std::size_t i = 0; i < buffer.size(); ++i) {
            buffer[i] = static_cast<float>(i);
            reference.push_back(buffer[i]);
        }
        // buffer destroyed here
    }

    // Reference should still be valid
    REQUIRE(reference.size() == 10);
    REQUIRE_THAT(reference[5], Catch::Matchers::WithinAbs(5.0f, 0.001f));
}

// ============================================================================
// Custom Container Tests
// ============================================================================

TEST_CASE("std::vector as underlying container", "[buffer][container]") {
    auto vec = std::make_unique<std::vector<float>>(20);
    for (std::size_t i = 0; i < vec->size(); ++i) {
        (*vec)[i] = static_cast<float>(i * 2);
    }

    auto buffer = wrap_mutable(std::move(vec));

    REQUIRE(buffer.size() == 20);
    REQUIRE_THAT(buffer[10], Catch::Matchers::WithinAbs(20.0f, 0.001f));

    // Modify through buffer
    buffer[10] = 99.0f;
    REQUIRE_THAT(buffer[10], Catch::Matchers::WithinAbs(99.0f, 0.001f));
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE("zero-size buffer handling", "[buffer][edge_case]") {
    auto buffer = make_mutable<float>(0);

    REQUIRE(buffer.size() == 0);
    // REQUIRE(buffer.data() != nullptr);  // Valid pointer even for empty

    auto span = buffer.as_span();
    REQUIRE(span.size() == 0);
}

TEST_CASE("single element buffer", "[buffer][edge_case]") {
    auto buffer = make_mutable<float>(1);
    buffer[0] = 3.14f;

    REQUIRE(buffer.size() == 1);
    REQUIRE_THAT(buffer[0], Catch::Matchers::WithinAbs(3.14f, 0.001f));
}

TEST_CASE("very large buffer", "[buffer][edge_case]") {
    constexpr std::size_t large_size = 10000000; // 10 million elements

    auto buffer = make_mutable<float>(large_size);
    REQUIRE(buffer.size() == large_size);

    // Spot check some values
    buffer[0] = 1.0f;
    buffer[large_size - 1] = 2.0f;

    REQUIRE_THAT(buffer[0], Catch::Matchers::WithinAbs(1.0f, 0.001f));
    REQUIRE_THAT(buffer[large_size - 1], Catch::Matchers::WithinAbs(2.0f, 0.001f));
}

TEST_CASE("multiple conversions", "[buffer][conversion]") {
    // Start with mutable
    auto mutable1 = make_mutable<float>(10);
    for (std::size_t i = 0; i < mutable1.size(); ++i) {
        mutable1[i] = static_cast<float>(i);
    }

    // Convert to immutable
    auto immutable = std::move(mutable1).to_immutable();
    REQUIRE(immutable.size() == 10);
    REQUIRE_THAT(immutable[5], Catch::Matchers::WithinAbs(5.0f, 0.001f));

    // Immutable can be shared multiple times
    auto shared1 = immutable.share();
    auto shared2 = immutable.share();

    REQUIRE(shared1.size() == 10);
    REQUIRE(shared2.size() == 10);
}

// ============================================================================
// Container Concept Validation Tests
// ============================================================================

TEST_CASE("ValidBufferContainer concept validation", "[buffer][concept]") {
    SECTION("std::vector satisfies ValidBufferContainer") {
        static_assert(ValidBufferContainer<std::vector<float>, float>);
        static_assert(ValidBufferContainer<std::vector<int>, int>);
        static_assert(ValidBufferContainer<std::vector<double>, double>);

        // Runtime test: can create buffers with std::vector
        auto vec_mutable = make_mutable<float>(10);
        REQUIRE(vec_mutable.size() == 10);

        auto vec_immutable = make_immutable<float>(10);
        REQUIRE(vec_immutable.size() == 10);
    }

    SECTION("std::array satisfies ValidBufferContainer") {
        static_assert(ValidBufferContainer<std::array<float, 5>, float>);
        static_assert(ValidBufferContainer<std::array<int, 10>, int>);

        // Runtime test: can create buffers with std::array
        auto arr = std::make_unique<std::array<float, 5>>();
        for (std::size_t i = 0; i < 5; ++i) {
            (*arr)[i] = static_cast<float>(i);
        }

        auto buf = mutable_buffer<float>(std::move(arr));
        REQUIRE(buf.size() == 5);
        REQUIRE_THAT(buf[2], Catch::Matchers::WithinAbs(2.0f, 0.001f));
    }

    SECTION("type mismatch fails ValidBufferContainer") {
        // std::vector<float> does not satisfy ValidBufferContainer<..., int>
        static_assert(!ValidBufferContainer<std::vector<float>, int>);
        static_assert(!ValidBufferContainer<std::vector<int>, float>);
    }
}

TEST_CASE("concept rejects invalid containers", "[buffer][concept][negative]") {
    // These tests document what SHOULD fail to compile
    // They use static_assert to verify the concept rejects invalid types

    SECTION("container without data() method fails") {
        struct NoDataMethod {
            using value_type [[maybe_unused]] = float;
            auto size() const -> std::size_t { return 10; }
            // Missing data() method
        };

        static_assert(!ValidBufferContainer<NoDataMethod, float>);
    }

    SECTION("container with wrong data() return type fails") {
        struct WrongDataReturn {
            using value_type [[maybe_unused]] = float;
            auto data() -> void* { return nullptr; } // Should return float*
            auto size() const -> std::size_t { return 10; }
        };

        static_assert(!ValidBufferContainer<WrongDataReturn, float>);
    }

    SECTION("container without size() method fails") {
        struct NoSizeMethod {
            using value_type [[maybe_unused]] = float;
            auto data() -> float* { return nullptr; }
            // Missing size() method
        };

        static_assert(!ValidBufferContainer<NoSizeMethod, float>);
    }

    SECTION("non-copyable container fails") {
        struct NonCopyable {
            using value_type [[maybe_unused]] = float;
            NonCopyable() = default;
            NonCopyable(const NonCopyable&) = delete;
            NonCopyable& operator=(const NonCopyable&) = delete;
            auto data() -> float* { return nullptr; }
            auto size() const -> std::size_t { return 10; }
        };

        static_assert(!ValidBufferContainer<NonCopyable, float>);
    }

    SECTION("container without value_type fails") {
        struct NoValueType {
            // Missing value_type
            auto data() -> float* { return nullptr; }
            auto size() const -> std::size_t { return 10; }
        };

        static_assert(!ValidBufferContainer<NoValueType, float>);
    }

    SECTION("non-contiguous container fails") {
        // std::deque is not contiguous
        static_assert(!ValidBufferContainer<std::deque<float>, float>);
    }
}

TEST_CASE("wrap functions enforce ValidBufferContainer", "[buffer][concept][factory]") {
    SECTION("wrap_mutable with valid container") {
        auto vec = std::make_unique<std::vector<float>>(10);
        for (std::size_t i = 0; i < 10; ++i) {
            (*vec)[i] = static_cast<float>(i);
        }

        auto buf = wrap_mutable(std::move(vec));
        REQUIRE(buf.size() == 10);
        REQUIRE_THAT(buf[5], Catch::Matchers::WithinAbs(5.0f, 0.001f));
    }

    SECTION("wrap_immutable with valid container") {
        auto vec = std::make_shared<std::vector<float>>(10);
        for (std::size_t i = 0; i < 10; ++i) {
            (*vec)[i] = static_cast<float>(i);
        }

        auto buf = wrap_immutable(vec);
        REQUIRE(buf.size() == 10);
        REQUIRE_THAT(buf[5], Catch::Matchers::WithinAbs(5.0f, 0.001f));
    }

    SECTION("wrap functions with std::array") {
        auto arr = std::make_unique<std::array<int, 5>>();
        std::iota(arr->begin(), arr->end(), 0);

        auto buf = wrap_mutable(std::move(arr));
        REQUIRE(buf.size() == 5);
        REQUIRE(buf[0] == 0);
        REQUIRE(buf[4] == 4);
    }
}

// ============================================================================
// Buffer Slicing Tests
// ============================================================================

TEST_CASE("immutable_buffer slice basic operations", "[buffer][slice]") {
    SECTION("slice full range") {
        auto buf = make_immutable<float>(10);
        auto slice = buf.slice(0, 10);

        REQUIRE(slice.size() == 10);
        REQUIRE(slice.data() == buf.data()); // Same data pointer
        REQUIRE(slice.is_unique() == buf.is_unique());
    }

    SECTION("slice first half") {
        auto buf = make_immutable<int>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
        auto slice = buf.slice(0, 5);

        REQUIRE(slice.size() == 5);
        REQUIRE(slice[0] == 0);
        REQUIRE(slice[4] == 4);
    }

    SECTION("slice second half") {
        auto buf = make_immutable<int>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
        auto slice = buf.slice(5, 5);

        REQUIRE(slice.size() == 5);
        REQUIRE(slice[0] == 5);
        REQUIRE(slice[4] == 9);
    }

    SECTION("slice middle section") {
        auto buf = make_immutable<int>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
        auto slice = buf.slice(3, 4);

        REQUIRE(slice.size() == 4);
        REQUIRE(slice[0] == 3);
        REQUIRE(slice[1] == 4);
        REQUIRE(slice[2] == 5);
        REQUIRE(slice[3] == 6);
    }

    SECTION("slice with npos (to end)") {
        auto buf = make_immutable<int>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
        auto slice = buf.slice(7); // From 7 to end

        REQUIRE(slice.size() == 3);
        REQUIRE(slice[0] == 7);
        REQUIRE(slice[1] == 8);
        REQUIRE(slice[2] == 9);
    }

    SECTION("slice_from convenience method") {
        auto buf = make_immutable<int>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
        auto slice = buf.slice_from(5);

        REQUIRE(slice.size() == 5);
        REQUIRE(slice[0] == 5);
        REQUIRE(slice[4] == 9);
    }

    SECTION("single element slice") {
        auto buf = make_immutable<int>({0, 1, 2, 3, 4});
        auto slice = buf.slice(2, 1);

        REQUIRE(slice.size() == 1);
        REQUIRE(slice[0] == 2);
    }

    SECTION("zero-sized slice at end") {
        auto buf = make_immutable<int>({0, 1, 2, 3, 4});
        auto slice = buf.slice(5, 0);

        REQUIRE(slice.size() == 0);
        REQUIRE(slice.empty());
    }
}

TEST_CASE("immutable_buffer slice sharing and lifetime", "[buffer][slice][lifetime]") {
    SECTION("slice shares underlying data") {
        auto buf = make_immutable<float>(100);
        auto slice1 = buf.slice(0, 50);
        auto slice2 = buf.slice(50, 50);

        // All should share the same underlying allocation
        REQUIRE(buf.is_unique() == false);
        REQUIRE(slice1.is_unique() == false);
        REQUIRE(slice2.is_unique() == false);
    }

    SECTION("original buffer keeps data alive") {
        auto buf = make_immutable<int>({1, 2, 3, 4, 5});
        auto slice = buf.slice(1, 3);

        // Original data should be accessible through slice
        REQUIRE(slice[0] == 2);
        REQUIRE(slice[1] == 3);
        REQUIRE(slice[2] == 4);
    }

    SECTION("slice keeps data alive after original destroyed") {
        auto slice = [&]() {
            auto buf = make_immutable<int>({10, 20, 30, 40, 50});
            return buf.slice(2, 2); // Returns slice, buf goes out of scope
        }();

        // Slice should still have valid data
        REQUIRE(slice.size() == 2);
        REQUIRE(slice[0] == 30);
        REQUIRE(slice[1] == 40);
    }

    SECTION("multiple slices of same region share data") {
        auto buf = make_immutable<float>(100);
        auto slice1 = buf.slice(10, 20);
        auto slice2 = buf.slice(10, 20);

        REQUIRE(slice1.data() == slice2.data());
        REQUIRE(slice1.size() == slice2.size());
    }
}

TEST_CASE("immutable_buffer slice chaining", "[buffer][slice]") {
    SECTION("slice of a slice") {
        auto buf = make_immutable<int>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
        auto slice1 = buf.slice(2, 6);    // {2, 3, 4, 5, 6, 7}
        auto slice2 = slice1.slice(1, 4); // {3, 4, 5, 6}

        REQUIRE(slice2.size() == 4);
        REQUIRE(slice2[0] == 3);
        REQUIRE(slice2[3] == 6);
    }

    SECTION("multiple levels of slicing") {
        auto buf = make_immutable<int>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
        auto slice1 = buf.slice(1, 8);    // {1, 2, 3, 4, 5, 6, 7, 8}
        auto slice2 = slice1.slice(2, 4); // {3, 4, 5, 6}
        auto slice3 = slice2.slice(1, 2); // {4, 5}

        REQUIRE(slice3.size() == 2);
        REQUIRE(slice3[0] == 4);
        REQUIRE(slice3[1] == 5);
    }
}

TEST_CASE("immutable_buffer slice error handling", "[buffer][slice][error]") {
    SECTION("offset beyond size throws") {
        auto buf = make_immutable<int>({0, 1, 2, 3, 4});
        REQUIRE_THROWS_AS(buf.slice(6, 1), std::out_of_range);
    }

    SECTION("count beyond end throws") {
        auto buf = make_immutable<int>({0, 1, 2, 3, 4});
        REQUIRE_THROWS_AS(buf.slice(3, 5), std::out_of_range);
    }

    SECTION("offset + count beyond size throws") {
        auto buf = make_immutable<int>({0, 1, 2, 3, 4});
        REQUIRE_THROWS_AS(buf.slice(4, 2), std::out_of_range);
    }

    SECTION("slice empty buffer with non-zero offset throws") {
        immutable_buffer<int> buf;
        REQUIRE_THROWS_AS(buf.slice(1, 0), std::out_of_range);
    }

    SECTION("slice empty buffer at offset 0 returns empty") {
        immutable_buffer<int> buf;
        auto slice = buf.slice(0, 0);
        REQUIRE(slice.empty());
    }

    SECTION("npos on empty buffer works") {
        immutable_buffer<int> buf;
        auto slice = buf.slice(0); // npos
        REQUIRE(slice.empty());
    }
}

TEST_CASE("mutable_buffer slice operations", "[buffer][slice][mutable]") {
    SECTION("slice from mutable returns immutable") {
        auto mut = make_mutable<int>({0, 1, 2, 3, 4, 5});
        auto slice = mut.slice(2, 3);

        // Result should be immutable_buffer
        REQUIRE(slice.size() == 3);
        REQUIRE(slice[0] == 2);
        REQUIRE(slice[1] == 3);
        REQUIRE(slice[2] == 4);
    }

    SECTION("slice_from mutable buffer") {
        auto mut = make_mutable<int>({10, 20, 30, 40, 50});
        auto slice = mut.slice_from(3);

        REQUIRE(slice.size() == 2);
        REQUIRE(slice[0] == 40);
        REQUIRE(slice[1] == 50);
    }

    SECTION("mutable slice preserves data after original destroyed") {
        auto slice = [&]() {
            auto mut = make_mutable<int>({100, 200, 300});
            return mut.slice(1, 2);
        }();

        REQUIRE(slice.size() == 2);
        REQUIRE(slice[0] == 200);
        REQUIRE(slice[1] == 300);
    }
}

TEST_CASE("buffer slice use cases", "[buffer][slice][use_case]") {
    SECTION("FFT windowing") {
        // Simulate 1024-sample buffer split into 512-sample windows
        auto samples = make_immutable<float>(1024);

        auto window1 = samples.slice(0, 512);
        auto window2 = samples.slice(512, 512);

        REQUIRE(window1.size() == 512);
        REQUIRE(window2.size() == 512);
        REQUIRE(window1.data() + 512 == window2.data());
    }

    SECTION("header/payload separation") {
        // Simulate network packet with 20-byte header
        auto packet = make_immutable<uint8_t>(1500);

        auto header = packet.slice(0, 20);
        auto payload = packet.slice_from(20);

        REQUIRE(header.size() == 20);
        REQUIRE(payload.size() == 1480);
    }

    SECTION("sliding window") {
        auto data = make_immutable<int>({0, 1, 2, 3, 4, 5, 6, 7, 8, 9});
        const std::size_t window_size = 3;

        std::vector<immutable_buffer<int>> windows;
        for (std::size_t i = 0; i <= data.size() - window_size; ++i) {
            windows.push_back(data.slice(i, window_size));
        }

        REQUIRE(windows.size() == 8);
        REQUIRE(windows[0][0] == 0);
        REQUIRE(windows[7][0] == 7);
        REQUIRE(windows[7][2] == 9);
    }
}

// ============================================================================
// Port Disconnect Tests
// ============================================================================

TEST_CASE("port disconnect API basic functionality", "[port][disconnect]") {
    // Create simple test components with public port access
    class SimpleSource : public component {
    public:
        SimpleSource() : component("SimpleSource") { add_port(output); }
        auto process() -> retval override { return retval::NOOP; }
        output_port<mutable_buffer<float>> output{"out"};
    };

    class SimpleSink : public component {
    public:
        SimpleSink() : component("SimpleSink") { add_port(input); }
        auto process() -> retval override { return retval::NOOP; }
        input_port<mutable_buffer<float>> input{"in"};
    };

    auto source = std::make_shared<SimpleSource>();
    auto sink1 = std::make_shared<SimpleSink>();
    auto sink2 = std::make_shared<SimpleSink>();

    SECTION("connection_count tracks connections") {
        REQUIRE(source->output.connection_count() == 0);
        REQUIRE_FALSE(source->output.is_connected());

        source->output.connect(&sink1->input);
        REQUIRE(source->output.connection_count() == 1);
        REQUIRE(source->output.is_connected());

        source->output.connect(&sink2->input);
        REQUIRE(source->output.connection_count() == 2);
    }

    SECTION("disconnect removes specific connection") {
        source->output.connect(&sink1->input);
        source->output.connect(&sink2->input);

        bool result = source->output.disconnect(&sink1->input);
        REQUIRE(result == true);
        REQUIRE(source->output.connection_count() == 1);
        REQUIRE_FALSE(source->output.is_connected_to(&sink1->input));
        REQUIRE(source->output.is_connected_to(&sink2->input));
    }

    SECTION("disconnect returns false for non-connected port") {
        source->output.connect(&sink1->input);

        bool result = source->output.disconnect(&sink2->input);
        REQUIRE(result == false);
        REQUIRE(source->output.connection_count() == 1);
    }

    SECTION("disconnect removes all connections") {
        source->output.connect(&sink1->input);
        source->output.connect(&sink2->input);

        auto count = source->output.disconnect();
        REQUIRE(count == 2);
        REQUIRE(source->output.connection_count() == 0);
        REQUIRE_FALSE(source->output.is_connected());
    }

    SECTION("connected_ports returns names") {
        source->output.connect(&sink1->input);
        source->output.connect(&sink2->input);

        auto names = source->output.connected_ports();
        REQUIRE(names.size() == 2);
    }
}

TEST_CASE("port disconnect with data transmission", "[port][disconnect]") {
    class SimpleSource : public component {
    public:
        SimpleSource() : component("SimpleSource") { add_port(output); }
        auto process() -> retval override { return retval::NOOP; }
        output_port<mutable_buffer<float>> output{"out"};
    };

    class SimpleSink : public component {
    public:
        SimpleSink() : component("SimpleSink") { add_port(input); }
        auto process() -> retval override { return retval::NOOP; }
        input_port<mutable_buffer<float>> input{"in"};
    };

    SECTION("disconnect during active data transmission") {
        auto source = std::make_shared<SimpleSource>();
        auto sink1 = std::make_shared<SimpleSink>();
        auto sink2 = std::make_shared<SimpleSink>();

        source->output.connect(&sink1->input);
        source->output.connect(&sink2->input);

        // Send data to both
        auto buffer1 = make_mutable<float>(100);
        for (std::size_t i = 0; i < buffer1.size(); ++i) {
            buffer1[i] = static_cast<float>(i);
        }
        source->output.send_data(std::move(buffer1), timestamp{});

        REQUIRE(sink1->input.size() == 1);
        REQUIRE(sink2->input.size() == 1);

        // Disconnect one sink
        source->output.disconnect(&sink1->input);

        // Send more data - should only go to sink2
        auto buffer2 = make_mutable<float>(50);
        for (std::size_t i = 0; i < buffer2.size(); ++i) {
            buffer2[i] = static_cast<float>(i + 1000);
        }
        source->output.send_data(std::move(buffer2), timestamp{});

        REQUIRE(sink1->input.size() == 1); // Still has first packet
        REQUIRE(sink2->input.size() == 2); // Has both packets

        // Verify data integrity in sink2
        auto [data1, ts1, md1] = sink2->input.get_data();
        REQUIRE(data1.size() == 100);
        REQUIRE(data1[0] == 0.0f);
        REQUIRE(data1[99] == 99.0f);

        auto [data2, ts2, md2] = sink2->input.get_data();
        REQUIRE(data2.size() == 50);
        REQUIRE(data2[0] == 1000.0f);
        REQUIRE(data2[49] == 1049.0f);
    }

    SECTION("disconnect stops all data flow") {
        auto source = std::make_shared<SimpleSource>();
        auto sink1 = std::make_shared<SimpleSink>();
        auto sink2 = std::make_shared<SimpleSink>();

        source->output.connect(&sink1->input);
        source->output.connect(&sink2->input);

        // Send initial data
        auto buffer1 = make_mutable<float>(10);
        source->output.send_data(std::move(buffer1), timestamp{});

        REQUIRE(sink1->input.size() == 1);
        REQUIRE(sink2->input.size() == 1);

        // Disconnect all
        source->output.disconnect();

        // Send more data - should go nowhere
        auto buffer2 = make_mutable<float>(10);
        source->output.send_data(std::move(buffer2), timestamp{});

        REQUIRE(sink1->input.size() == 1); // No new data
        REQUIRE(sink2->input.size() == 1); // No new data
    }

    SECTION("reconnect after disconnect works correctly") {
        auto source = std::make_shared<SimpleSource>();
        auto sink = std::make_shared<SimpleSink>();

        // Connect -> send -> disconnect -> reconnect -> send
        source->output.connect(&sink->input);

        auto buffer1 = make_mutable<float>(5);
        for (std::size_t i = 0; i < 5; ++i) {
            buffer1[i] = static_cast<float>(i);
        }
        source->output.send_data(std::move(buffer1), timestamp{});
        REQUIRE(sink->input.size() == 1);

        source->output.disconnect(&sink->input);

        auto buffer2 = make_mutable<float>(5);
        source->output.send_data(std::move(buffer2), timestamp{});
        REQUIRE(sink->input.size() == 1); // No new data

        source->output.connect(&sink->input);

        auto buffer3 = make_mutable<float>(5);
        for (std::size_t i = 0; i < 5; ++i) {
            buffer3[i] = static_cast<float>(i + 100);
        }
        source->output.send_data(std::move(buffer3), timestamp{});
        REQUIRE(sink->input.size() == 2);

        // Verify data integrity
        auto [data1, ts1, md1] = sink->input.get_data();
        REQUIRE(data1[0] == 0.0f);

        auto [data3, ts3, md3] = sink->input.get_data();
        REQUIRE(data3[0] == 100.0f);
    }
}

TEST_CASE("port disconnect fanout scenarios", "[port][disconnect][fanout]") {
    class SimpleSource : public component {
    public:
        SimpleSource() : component("SimpleSource") { add_port(output); }
        auto process() -> retval override { return retval::NOOP; }
        output_port<mutable_buffer<float>> output{"out"};
    };

    class SimpleSink : public component {
    public:
        SimpleSink() : component("SimpleSink") { add_port(input); }
        auto process() -> retval override { return retval::NOOP; }
        input_port<mutable_buffer<float>> input{"in"};
    };

    SECTION("selective disconnect in large fanout") {
        auto source = std::make_shared<SimpleSource>();
        std::vector<std::shared_ptr<SimpleSink>> sinks;

        // Connect to 5 sinks
        for (int i = 0; i < 5; ++i) {
            auto sink = std::make_shared<SimpleSink>();
            source->output.connect(&sink->input);
            sinks.push_back(sink);
        }

        REQUIRE(source->output.connection_count() == 5);

        // Disconnect middle sink (index 2)
        source->output.disconnect(&sinks[2]->input);

        REQUIRE(source->output.connection_count() == 4);
        REQUIRE_FALSE(source->output.is_connected_to(&sinks[2]->input));

        // Send data - should reach 4 sinks
        auto buffer = make_mutable<float>(10);
        source->output.send_data(std::move(buffer), timestamp{});

        for (std::size_t i = 0; i < sinks.size(); ++i) {
            if (i == 2) {
                REQUIRE(sinks[i]->input.size() == 0);
            } else {
                REQUIRE(sinks[i]->input.size() == 1);
            }
        }
    }

    SECTION("disconnect multiple in sequence") {
        auto source = std::make_shared<SimpleSource>();
        std::vector<std::shared_ptr<SimpleSink>> sinks;

        for (int i = 0; i < 4; ++i) {
            auto sink = std::make_shared<SimpleSink>();
            source->output.connect(&sink->input);
            sinks.push_back(sink);
        }

        // Disconnect sinks 0 and 2
        source->output.disconnect(&sinks[0]->input);
        source->output.disconnect(&sinks[2]->input);

        REQUIRE(source->output.connection_count() == 2);

        auto buffer = make_mutable<float>(10);
        source->output.send_data(std::move(buffer), timestamp{});

        REQUIRE(sinks[0]->input.size() == 0);
        REQUIRE(sinks[1]->input.size() == 1);
        REQUIRE(sinks[2]->input.size() == 0);
        REQUIRE(sinks[3]->input.size() == 1);
    }

    SECTION("reconnect to create new fanout pattern") {
        auto source = std::make_shared<SimpleSource>();
        auto sink1 = std::make_shared<SimpleSink>();
        auto sink2 = std::make_shared<SimpleSink>();
        auto sink3 = std::make_shared<SimpleSink>();

        // Initial pattern: source -> {sink1, sink2}
        source->output.connect(&sink1->input);
        source->output.connect(&sink2->input);

        auto buffer1 = make_mutable<float>(5);
        source->output.send_data(std::move(buffer1), timestamp{});

        REQUIRE(sink1->input.size() == 1);
        REQUIRE(sink2->input.size() == 1);
        REQUIRE(sink3->input.size() == 0);

        // Reconfigure: source -> {sink2, sink3}
        source->output.disconnect(&sink1->input);
        source->output.connect(&sink3->input);

        auto buffer2 = make_mutable<float>(5);
        source->output.send_data(std::move(buffer2), timestamp{});

        REQUIRE(sink1->input.size() == 1); // No new data
        REQUIRE(sink2->input.size() == 2); // Got both
        REQUIRE(sink3->input.size() == 1); // Got second
    }
}

TEST_CASE("port disconnect thread safety", "[port][disconnect][thread]") {
    class SimpleSource : public component {
    public:
        SimpleSource() : component("SimpleSource") { add_port(output); }
        auto process() -> retval override { return retval::NOOP; }
        output_port<mutable_buffer<float>> output{"out"};
    };

    class SimpleSink : public component {
    public:
        SimpleSink() : component("SimpleSink") { add_port(input); }
        auto process() -> retval override { return retval::NOOP; }
        input_port<mutable_buffer<float>> input{"in"};
    };

    SECTION("concurrent send and disconnect operations") {
        auto source = std::make_shared<SimpleSource>();
        auto sink1 = std::make_shared<SimpleSink>();
        auto sink2 = std::make_shared<SimpleSink>();

        source->output.connect(&sink1->input);
        source->output.connect(&sink2->input);

        std::atomic<bool> stop{false};
        std::atomic<int> send_count{0};

        // Thread 1: Continuously send data
        auto sender = std::thread([&]() {
            while (!stop.load()) {
                auto buffer = make_mutable<float>(10);
                for (std::size_t i = 0; i < 10; ++i) {
                    buffer[i] = static_cast<float>(send_count.load() * 10 + i);
                }
                source->output.send_data(std::move(buffer), timestamp{});
                send_count.fetch_add(1);
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });

        // Thread 2: Disconnect and reconnect
        auto disconnector = std::thread([&]() {
            for (int i = 0; i < 10; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                source->output.disconnect(&sink1->input);
                std::this_thread::sleep_for(std::chrono::microseconds(500));
                source->output.connect(&sink1->input);
            }
        });

        // Let them run briefly
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        stop.store(true);

        sender.join();
        disconnector.join();

        // Should have received some data without crashes
        // sink2 should have all packets since it was never disconnected
        REQUIRE(sink2->input.size() > 0);
        REQUIRE(sink2->input.size() == static_cast<std::size_t>(send_count.load()));
    }

    SECTION("concurrent disconnect calls") {
        auto source = std::make_shared<SimpleSource>();
        std::vector<std::shared_ptr<SimpleSink>> sinks;

        for (int i = 0; i < 10; ++i) {
            auto sink = std::make_shared<SimpleSink>();
            source->output.connect(&sink->input);
            sinks.push_back(sink);
        }

        // Multiple threads trying to disconnect different sinks
        std::vector<std::thread> threads;
        for (std::size_t i = 0; i < sinks.size(); ++i) {
            threads.emplace_back([&, i]() { source->output.disconnect(&sinks[i]->input); });
        }

        for (auto& t : threads) {
            t.join();
        }

        // All should be disconnected
        REQUIRE(source->output.connection_count() == 0);
        REQUIRE_FALSE(source->output.is_connected());
    }
}

TEST_CASE("port disconnect with metadata", "[port][disconnect][metadata]") {
    class SimpleSource : public component {
    public:
        SimpleSource() : component("SimpleSource") { add_port(output); }
        auto process() -> retval override { return retval::NOOP; }
        output_port<mutable_buffer<float>> output{"out"};
    };

    class SimpleSink : public component {
    public:
        SimpleSink() : component("SimpleSink") { add_port(input); }
        auto process() -> retval override { return retval::NOOP; }
        input_port<mutable_buffer<float>> input{"in"};
    };

    SECTION("metadata not sent to disconnected port") {
        auto source = std::make_shared<SimpleSource>();
        auto sink1 = std::make_shared<SimpleSink>();
        auto sink2 = std::make_shared<SimpleSink>();

        source->output.connect(&sink1->input);
        source->output.connect(&sink2->input);

        // Send metadata and data to both
        metadata md1;
        md1.sample_rate = 1e6;
        auto buffer1 = make_mutable<float>(10);
        source->output.send_data(std::move(buffer1), timestamp{}, md1);

        auto [data1a, ts1a, md1a] = sink1->input.get_data();
        REQUIRE(md1a != nullptr);
        REQUIRE(md1a->sample_rate == 1e6);

        auto [data1b, ts1b, md1b] = sink2->input.get_data();
        REQUIRE(md1b != nullptr);
        REQUIRE(md1b->sample_rate == 1e6);

        // Disconnect sink1
        source->output.disconnect(&sink1->input);

        // Send new metadata and data
        metadata md2;
        md2.sample_rate = 2e6;
        auto buffer2 = make_mutable<float>(10);
        source->output.send_data(std::move(buffer2), timestamp{}, md2);

        // sink1 should have no new data
        REQUIRE(sink1->input.size() == 0);

        // sink2 should have new data with new metadata
        auto [data2b, ts2b, md2b] = sink2->input.get_data();
        REQUIRE(md2b != nullptr);
        REQUIRE(md2b->sample_rate == 2e6);
    }
}

// ============================================================================
// Buffer Capacity Management Tests
// ============================================================================

TEST_CASE("buffer capacity management basic operations", "[buffer][capacity]") {
    SECTION("resize grows buffer") {
        auto buffer = make_mutable<float>(10);
        for (std::size_t i = 0; i < 10; ++i) {
            buffer[i] = static_cast<float>(i);
        }

        REQUIRE(buffer.size() == 10);
        REQUIRE(buffer[9] == 9.0f);

        buffer.resize(20);
        REQUIRE(buffer.size() == 20);
        REQUIRE(buffer[9] == 9.0f); // Old data preserved
        // New elements are default-initialized (0.0f)
        REQUIRE(buffer[19] == 0.0f);
    }

    SECTION("resize shrinks buffer") {
        auto buffer = make_mutable<float>(20);
        for (std::size_t i = 0; i < 20; ++i) {
            buffer[i] = static_cast<float>(i);
        }

        buffer.resize(5);
        REQUIRE(buffer.size() == 5);
        REQUIRE(buffer[0] == 0.0f);
        REQUIRE(buffer[4] == 4.0f);
    }

    SECTION("reserve increases capacity without changing size") {
        auto buffer = make_mutable<float>(10);
        for (std::size_t i = 0; i < 10; ++i) {
            buffer[i] = static_cast<float>(i);
        }

        auto initial_capacity = buffer.capacity();
        buffer.reserve(1000);

        REQUIRE(buffer.size() == 10);       // Size unchanged
        REQUIRE(buffer.capacity() >= 1000); // Capacity increased
        REQUIRE(buffer.capacity() >= initial_capacity);

        // Data preserved
        for (std::size_t i = 0; i < 10; ++i) {
            REQUIRE(buffer[i] == static_cast<float>(i));
        }
    }

    SECTION("capacity returns current capacity") {
        auto buffer = make_mutable<float>(10);
        auto cap = buffer.capacity();
        REQUIRE(cap >= 10); // At least the size
    }

    SECTION("shrink_to_fit reduces capacity") {
        auto buffer = make_mutable<float>(10);
        buffer.reserve(1000);
        REQUIRE(buffer.capacity() >= 1000);

        buffer.shrink_to_fit();
        // Note: shrink_to_fit is non-binding, but typically reduces capacity
        REQUIRE(buffer.size() == 10);
        REQUIRE(buffer.capacity() >= 10); // Still at least size
    }

    SECTION("clear removes all elements but preserves capacity") {
        auto buffer = make_mutable<float>(100);
        for (std::size_t i = 0; i < 100; ++i) {
            buffer[i] = static_cast<float>(i);
        }

        auto cap_before = buffer.capacity();
        buffer.clear();

        REQUIRE(buffer.size() == 0);
        REQUIRE(buffer.empty());
        REQUIRE(buffer.capacity() == cap_before); // Capacity preserved
    }
}

TEST_CASE("buffer capacity error handling", "[buffer][capacity]") {
    SECTION("resize on empty buffer throws") {
        mutable_buffer<float> empty_buffer;
        REQUIRE_THROWS_AS(empty_buffer.resize(10), std::runtime_error);
    }

    SECTION("reserve on empty buffer throws") {
        mutable_buffer<float> empty_buffer;
        REQUIRE_THROWS_AS(empty_buffer.reserve(10), std::runtime_error);
    }

    SECTION("capacity on empty buffer returns 0") {
        mutable_buffer<float> empty_buffer;
        REQUIRE(empty_buffer.capacity() == 0);
    }

    SECTION("shrink_to_fit on empty buffer is no-op") {
        mutable_buffer<float> empty_buffer;
        REQUIRE_NOTHROW(empty_buffer.shrink_to_fit());
    }

    SECTION("clear on empty buffer is no-op") {
        mutable_buffer<float> empty_buffer;
        REQUIRE_NOTHROW(empty_buffer.clear());
    }
}

TEST_CASE("buffer capacity with data transfer", "[buffer][capacity][integration]") {
    SECTION("resize before sending through port") {
        class SimpleSource : public component {
        public:
            SimpleSource() : component("SimpleSource") { add_port(output); }
            auto process() -> retval override { return retval::NOOP; }
            output_port<mutable_buffer<float>> output{"out"};
        };

        class SimpleSink : public component {
        public:
            SimpleSink() : component("SimpleSink") { add_port(input); }
            auto process() -> retval override { return retval::NOOP; }
            input_port<mutable_buffer<float>> input{"in"};
        };

        auto source = std::make_shared<SimpleSource>();
        auto sink = std::make_shared<SimpleSink>();
        source->output.connect(&sink->input);

        // Create buffer, populate, resize, then send
        auto buffer = make_mutable<float>(10);
        for (std::size_t i = 0; i < 10; ++i) {
            buffer[i] = static_cast<float>(i);
        }

        buffer.resize(15);
        // Fill new elements
        for (std::size_t i = 10; i < 15; ++i) {
            buffer[i] = static_cast<float>(i * 10);
        }

        source->output.send_data(std::move(buffer), timestamp{});
        REQUIRE(sink->input.size() == 1);

        auto [data, ts, md] = sink->input.get_data();
        REQUIRE(data.size() == 15);
        REQUIRE(data[0] == 0.0f);
        REQUIRE(data[9] == 9.0f);
        REQUIRE(data[10] == 100.0f);
        REQUIRE(data[14] == 140.0f);
    }

    SECTION("reserve before growing buffer incrementally") {
        auto buffer = make_mutable<float>(0);
        buffer.resize(0); // Ensure size is 0

        // Pre-allocate space
        buffer.reserve(1000);
        auto reserved_capacity = buffer.capacity();
        REQUIRE(reserved_capacity >= 1000);

        // Grow incrementally - should not reallocate if reserve worked
        for (int i = 0; i < 10; ++i) {
            buffer.resize(buffer.size() + 100);
            // Capacity should remain stable (no reallocation)
            REQUIRE(buffer.capacity() == reserved_capacity);
        }

        REQUIRE(buffer.size() == 1000);
    }

    SECTION("clear and reuse buffer") {
        auto buffer = make_mutable<float>(100);
        for (std::size_t i = 0; i < 100; ++i) {
            buffer[i] = static_cast<float>(i);
        }

        auto cap = buffer.capacity();

        // Clear and refill
        buffer.clear();
        REQUIRE(buffer.empty());

        buffer.resize(50);
        for (std::size_t i = 0; i < 50; ++i) {
            buffer[i] = static_cast<float>(i * 2);
        }

        REQUIRE(buffer.size() == 50);
        REQUIRE(buffer[0] == 0.0f);
        REQUIRE(buffer[49] == 98.0f);
        REQUIRE(buffer.capacity() >= cap); // Should not have shrunk
    }
}

TEST_CASE("buffer capacity with copy operations", "[buffer][capacity]") {
    SECTION("copied buffer has independent capacity") {
        auto buffer1 = make_mutable<float>(10);
        buffer1.reserve(100);

        auto buffer2 = buffer1.copy();

        // Both have same size
        REQUIRE(buffer1.size() == buffer2.size());
        REQUIRE(buffer1.size() == 10);

        // Resize one doesn't affect the other
        buffer1.resize(20);
        REQUIRE(buffer1.size() == 20);
        REQUIRE(buffer2.size() == 10);
    }

    SECTION("capacity operations work on copied buffer") {
        auto buffer1 = make_mutable<float>(5);
        for (std::size_t i = 0; i < 5; ++i) {
            buffer1[i] = static_cast<float>(i);
        }

        auto buffer2 = buffer1.copy();

        // Modify capacity of copy
        buffer2.reserve(1000);
        REQUIRE(buffer2.capacity() >= 1000);

        buffer2.resize(10);
        REQUIRE(buffer2.size() == 10);

        // Original unaffected
        REQUIRE(buffer1.size() == 5);
        REQUIRE(buffer1[4] == 4.0f);
    }
}

TEST_CASE("buffer capacity std::array compatibility", "[buffer][capacity]") {
    SECTION("std::array buffers don't support capacity operations") {
        // This should compile - std::array satisfies ValidBufferContainer
        auto arr = std::make_unique<std::array<float, 10>>();
        mutable_buffer<float> buffer(std::move(arr));

        REQUIRE(buffer.size() == 10);
        REQUIRE_FALSE(buffer.empty());

        // Capacity operations should throw on std::array-based buffers
        REQUIRE_THROWS_AS(buffer.resize(5), std::runtime_error);
        REQUIRE_THROWS_AS(buffer.reserve(20), std::runtime_error);
        REQUIRE(buffer.capacity() == 0); // Returns 0 for non-dynamic containers

        // These should be no-ops
        REQUIRE_NOTHROW(buffer.shrink_to_fit());
        REQUIRE_NOTHROW(buffer.clear());
    }
}

// ============================================================================
// Component State Tests
// ============================================================================

// TEST_CASE("component process multiple times", "[component][state]") {
//     // Create a source that can send multiple times
//     class MultiSendSource : public component {
//     public:
//         MultiSendSource() : component("MultiSendSource") {
//             add_port(&m_output);
//         }

//         auto process() -> retval override {
//             if (m_send_count >= max_sends) {
//                 return retval::FINISH;
//             }

//             auto buffer = make_mutable<float>(5);
//             for (std::size_t i = 0; i < buffer.size(); ++i) {
//                 buffer[i] = static_cast<float>(m_send_count * 100 + i);
//             }

//             m_output.send_data(std::move(buffer), timestamp{});
//             m_send_count++;

//             return retval::NORMAL;
//         }

//         std::size_t m_send_count{0};
//         std::size_t max_sends{3};

//     private:
//         output_port<mutable_buffer<float>> m_output{"data_out"};
//     };

//     class MultiReceiveSink : public component {
//     public:
//         MultiReceiveSink() : component("MultiReceiveSink") {
//             add_port(&m_input);
//         }

//         auto process() -> retval override {
//             auto [buffer, ts, metadata] = m_input.get_data();

//             if (buffer.size() == 0) {
//                 return retval::NOOP;
//             }

//             m_receive_count++;

//             // Store first value of each buffer
//             if (buffer.size() > 0) {
//                 m_first_values.push_back(buffer[0]);
//             }

//             if (m_receive_count >= 3) {
//                 return retval::FINISH;
//             }

//             return retval::NORMAL;
//         }

//         std::size_t m_receive_count{0};
//         std::vector<float> m_first_values;

//     private:
//         input_port<mutable_buffer<float>> m_input{"data_in"};
//     };

//     auto source = std::make_shared<MultiSendSource>();
//     auto sink = std::make_shared<MultiReceiveSink>();

//     REQUIRE(source->connect("data_out", sink, "data_in"));

//     // Process multiple times
//     for (int i = 0; i < 5; ++i) {
//         source->process();
//         std::this_thread::sleep_for(std::chrono::milliseconds(20));
//         sink->process();
//         std::this_thread::sleep_for(std::chrono::milliseconds(20));
//     }

//     // Verify multiple sends and receives
//     REQUIRE(source->m_send_count == 3);
//     REQUIRE(sink->m_receive_count == 3);
//     REQUIRE(sink->m_first_values.size() == 3);

//     // Check values
//     REQUIRE_THAT(sink->m_first_values[0], Catch::Matchers::WithinAbs(0.0f, 0.001f));
//     REQUIRE_THAT(sink->m_first_values[1], Catch::Matchers::WithinAbs(100.0f, 0.001f));
//     REQUIRE_THAT(sink->m_first_values[2], Catch::Matchers::WithinAbs(200.0f, 0.001f));
// }

// ============================================================================
// Port Query Tests
// ============================================================================

TEST_CASE("port metadata queries", "[port][query]") {
    auto source = std::make_shared<TestMutableSource>();
    auto sink = std::make_shared<TestImmutableSink>();

    // Query output port
    auto* out_port = source->get_port<output_port_base>("data_out");
    REQUIRE(out_port != nullptr);
    REQUIRE(out_port->name() == "data_out");
    REQUIRE(out_port->is_mutable() == true);
    REQUIRE(out_port->element_type_id() == typeid(float).hash_code());

    // Query input port
    auto* in_port = sink->get_port<input_port_base>("data_in");
    REQUIRE(in_port != nullptr);
    REQUIRE(in_port->name() == "data_in");
    REQUIRE(in_port->is_mutable() == false);
    REQUIRE(in_port->element_type_id() == typeid(float).hash_code());
}

TEST_CASE("port connection status", "[port][query]") {
    auto source = std::make_shared<TestMutableSource>();
    auto sink = std::make_shared<TestMutableSink>();

    auto* out_port = source->get_port<output_port_base>("data_out");
    REQUIRE(out_port != nullptr);

    // Initially not connected
    REQUIRE(!out_port->is_connected());

    // Connect
    REQUIRE(source->connect("data_out", sink, "data_in"));

    // Now connected
    REQUIRE(out_port->is_connected());
}

// ============================================================================
// Port Statistics Tests
// ============================================================================

TEST_CASE("input port statistics tracking", "[port][stats]") {
    // Clear metrics from previous tests
    metrics::registry::instance().clear();

    auto source = std::make_shared<TestMutableSource>();
    auto sink = std::make_shared<TestMutableSink>();

    source->m_size = 100;
    REQUIRE(source->connect("data_out", sink, "data_in"));

    // Get port references
    auto* input_port = sink->get_port<input_port_base>("data_in");
    REQUIRE(input_port != nullptr);

    // Initial statistics should be zero
    auto& stats = input_port->stats();
    REQUIRE(stats.packets_transferred() == 0);
    REQUIRE(stats.packets_dropped() == 0);
    REQUIRE(stats.bytes_transferred() == 0);

    // Send data
    REQUIRE(source->process() == retval::NORMAL);
    REQUIRE(sink->process() == retval::FINISH);

    // Verify statistics updated
    REQUIRE(stats.packets_transferred() == 1);
    REQUIRE(stats.bytes_transferred() == 100 * sizeof(float));
    REQUIRE(stats.packets_dropped() == 0);
}

TEST_CASE("output port statistics tracking", "[port][stats]") {
    // Clear metrics from previous tests
    metrics::registry::instance().clear();

    auto source = std::make_shared<TestMutableSource>();
    auto sink = std::make_shared<TestMutableSink>();

    source->m_size = 50;
    REQUIRE(source->connect("data_out", sink, "data_in"));

    // Get output port
    auto* output_port = source->get_port<output_port_base>("data_out");
    REQUIRE(output_port != nullptr);

    // Initial state (fresh metrics)
    auto& stats = output_port->stats();
    REQUIRE(stats.packets_transferred() == 0);

    // Send data (record_transfer updates output stats synchronously inside send_data)
    REQUIRE(source->process() == retval::NORMAL);

    // Verify statistics
    REQUIRE(stats.packets_transferred() == 1);
    REQUIRE(stats.bytes_transferred() == 50 * sizeof(float));
}

TEST_CASE("statistics reset", "[port][stats]") {
    // Clear metrics from previous tests
    metrics::registry::instance().clear();

    auto source = std::make_shared<TestMutableSource>();
    auto sink = std::make_shared<TestMutableSink>();

    source->m_size = 10;
    REQUIRE(source->connect("data_out", sink, "data_in"));

    auto* input_port = sink->get_port<input_port_base>("data_in");

    // Send some data (synchronous: source->process() fills the ring, sink->process() drains)
    source->process();
    sink->process();

    // Verify stats exist
    REQUIRE(input_port->stats().packets_transferred() > 0);
    auto packets_before_reset = input_port->stats().packets_transferred();

    // Reset - note: counters are monotonic (metrics design), only timestamps reset
    input_port->reset_stats();

    // Counters remain unchanged (monotonic by design for metrics compatibility)
    REQUIRE(input_port->stats().packets_transferred() == packets_before_reset);
}

TEST_CASE("queue depth and capacity metrics", "[port][stats]") {
    // Clear metrics from previous tests
    metrics::registry::instance().clear();

    auto source = std::make_shared<TestMutableSource>();
    auto sink = std::make_shared<TestMutableSink>();

    source->m_size = 10;
    REQUIRE(source->connect("data_out", sink, "data_in"));

    auto* input_port = sink->get_port<input_port_base>("data_in");
    REQUIRE(input_port != nullptr);

    // Set a specific queue depth
    input_port->depth(20);

    // Verify queue_capacity metric reflects the configured depth
    auto& registry = metrics::registry::instance();

    metrics::labels_t labels = {{"component_id", "TestMutableSink"}, {"port_name", "data_in"}, {"port_type", "input"}};

    // Get the queue_capacity gauge (get_or_create returns existing one)
    auto& capacity_gauge = registry.get_or_create_gauge("composite.port.queue_capacity", "", "1", labels);
    REQUIRE(capacity_gauge.value() == 20.0);

    // Get the queue_depth gauge
    auto& depth_gauge = registry.get_or_create_gauge("composite.port.queue_depth", "", "1", labels);

    // Initially queue should be empty
    REQUIRE(depth_gauge.value() == 0.0);

    // Send some packets (without consuming)
    for (int i = 0; i < 3; ++i) {
        source->m_sent = false;
        source->process();
    }

    // Queue depth should reflect 3 packets
    REQUIRE(depth_gauge.value() == 3.0);

    // Consume one packet
    sink->process();

    // Queue depth should now be 2
    REQUIRE(depth_gauge.value() == 2.0);

    // Change capacity at runtime
    input_port->depth(50);
    REQUIRE(capacity_gauge.value() == 50.0);
}

TEST_CASE("statistics throughput calculation", "[port][stats]") {
    auto source = std::make_shared<TestMutableSource>();
    auto sink = std::make_shared<TestMutableSink>();

    source->m_size = 1000;
    REQUIRE(source->connect("data_out", sink, "data_in"));

    auto* output_port = source->get_port<output_port_base>("data_out");

    // Send data
    source->process();

    // Calculate throughput
    auto throughput = output_port->stats().throughput_mbps();

    // Should be non-zero
    REQUIRE(throughput > 0.0);
}

// ============================================================================
// Backpressure Tests
// ============================================================================

TEST_CASE("port is_full() check", "[port][backpressure]") {
    auto source = std::make_shared<TestMutableSource>();
    auto sink = std::make_shared<TestMutableSink>();

    REQUIRE(source->connect("data_out", sink, "data_in"));

    auto* input_port = sink->get_port<input_port_base>("data_in");
    REQUIRE(input_port != nullptr);

    // Initially not full
    REQUIRE(!input_port->is_full());

    // Set small depth
    input_port->depth(2);

    // Send data to fill queue - need to reset m_sent flag to send multiple times
    source->m_size = 10;
    source->m_sent = false;
    source->process();

    source->m_sent = false;
    source->process();

    // Should be full now
    REQUIRE(input_port->is_full());
}

TEST_CASE("available_capacity() tracking", "[port][backpressure]") {
    auto source = std::make_shared<TestMutableSource>();
    auto sink = std::make_shared<TestMutableSink>();

    REQUIRE(source->connect("data_out", sink, "data_in"));

    auto* input_port = sink->get_port<input_port_base>("data_in");

    // Set depth
    input_port->depth(5);

    // Should have full capacity
    REQUIRE(input_port->available_capacity() == 5);

    // Send one packet
    source->m_size = 10;
    source->process();

    // Capacity should decrease
    REQUIRE(input_port->available_capacity() == 4);
}

TEST_CASE("overflow callback invocation", "[port][backpressure]") {
    // Clear metrics from previous tests
    metrics::registry::instance().clear();

    auto source = std::make_shared<TestMutableSource>();
    auto sink = std::make_shared<TestMutableSink>();

    REQUIRE(source->connect("data_out", sink, "data_in"));

    auto* input_port = sink->get_port<input_port_base>("data_in");

    // Track dropped packets
    std::atomic<std::size_t> total_dropped{0};
    input_port->set_overflow_callback([&total_dropped](std::size_t count) { total_dropped.fetch_add(count); });

    // Set small depth to trigger overflow
    input_port->depth(2);

    // Send multiple packets - reset m_sent flag each time
    source->m_size = 10;
    for (int i = 0; i < 5; ++i) {
        source->m_sent = false;
        source->process();
    }

    // Some packets should have been dropped (sent 5, depth is 2, so 3 dropped)
    REQUIRE(total_dropped.load() > 0);

    // Verify statistics match
    REQUIRE(input_port->stats().packets_dropped() == total_dropped.load());
}

TEST_CASE("overflow with drop rate calculation", "[port][backpressure]") {
    // Clear metrics from previous tests
    metrics::registry::instance().clear();

    auto source = std::make_shared<TestMutableSource>();
    auto sink = std::make_shared<TestMutableSink>();

    REQUIRE(source->connect("data_out", sink, "data_in"));

    auto* input_port = sink->get_port<input_port_base>("data_in");

    // Set depth to 0 to drop everything
    input_port->depth(0);

    // Send packets - reset m_sent flag each time
    source->m_size = 10;
    for (int i = 0; i < 10; ++i) {
        source->m_sent = false;
        source->process();
    }

    // All should be dropped
    REQUIRE(input_port->stats().packets_dropped() == 10);

    // Drop rate should be 100%
    auto drop_rate = input_port->stats().drop_rate();
    REQUIRE_THAT(drop_rate, Catch::Matchers::WithinAbs(1.0, 0.01));
}

TEST_CASE("can_send() backpressure check", "[port][backpressure]") {
    auto source = std::make_shared<TestMutableSource>();
    auto sink = std::make_shared<TestMutableSink>();

    REQUIRE(source->connect("data_out", sink, "data_in"));

    auto* output_port = source->get_port<output_port_base>("data_out");
    auto* input_port = sink->get_port<input_port_base>("data_in");

    // Can send initially
    REQUIRE(output_port->can_send());

    // Fill input queue
    input_port->depth(1);
    source->m_size = 10;
    source->process();

    // Now cannot send (queue full)
    REQUIRE(!output_port->can_send());
}

// ============================================================================
// Blocking Variants Tests
// ============================================================================

// Note: the deprecated get_data(timeout) / get_data(blocking) no-op overloads were removed;
// try_get() is the canonical non-blocking read and get_data() remains for the size-0 idiom.

// ============================================================================
// Stress Tests
// ============================================================================

// TEST_CASE("rapid buffer creation and destruction", "[stress]") {
//     constexpr std::size_t iterations = 10000;

//     for (std::size_t i = 0; i < iterations; ++i) {
//         auto buffer = make_mutable<float>(100);
//         buffer[0] = static_cast<float>(i);

//         // Immediately destroy
//     }

//     REQUIRE(true);  // If we get here without crash, test passes
// }

// TEST_CASE("deep copy chain", "[stress]") {
//     auto buffer = make_mutable<float>(100);
//     for (std::size_t i = 0; i < buffer.size(); ++i) {
//         buffer[i] = static_cast<float>(i);
//     }

//     // Create chain of copies
//     std::vector<mutable_buffer<float>> copies;
//     for (int i = 0; i < 10; ++i) {
//         if (i == 0) {
//             copies.push_back(buffer.copy());
//         } else {
//             copies.push_back(copies.back().copy());
//         }
//     }

//     // Verify all copies are independent
//     for (auto& copy : copies) {
//         REQUIRE(copy.size() == 100);
//         REQUIRE_THAT(copy[50], Catch::Matchers::WithinAbs(50.0f, 0.001f));
//     }
// }

// TEST_CASE("wide fan-out stress test", "[stress][fanout]") {
//     auto source = std::make_shared<TestImmutableSource>();
//     source->m_size = 1000;

//     // Create many sinks
//     std::vector<std::shared_ptr<TestImmutableSink>> sinks;
//     for (int i = 0; i < 50; ++i) {
//         auto sink = std::make_shared<TestImmutableSink>();
//         sinks.push_back(sink);
//         REQUIRE(source->connect("data_out", sink, "data_in"));
//     }

//     // Send data
//     REQUIRE(source->process() == retval::NORMAL);
//     std::this_thread::sleep_for(std::chrono::milliseconds(100));

//     // All sinks should receive
//     for (auto& sink : sinks) {
//         REQUIRE(sink->process() == retval::FINISH);
//         REQUIRE(sink->m_received);
//         REQUIRE(sink->m_size == 1000);
//     }
// }

// ============================================================================
// Benchmark Tests (Informational)
// ============================================================================

// TEST_CASE("benchmark: mutable chain vs copies", "[.benchmark]") {
//     // This test is tagged with [.benchmark] so it won't run by default
//     // Run with: ./test_ports "[.benchmark]"

//     constexpr std::size_t buffer_size = 1000000;
//     constexpr std::size_t iterations = 100;

//     SECTION("mutable chain (zero copy)") {
//         auto total_duration = std::chrono::nanoseconds{0};

//         for (std::size_t iter = 0; iter < iterations; ++iter) {
//             auto buffer = make_mutable<float>(buffer_size);

//             auto start = std::chrono::high_resolution_clock::now();

//             // Simulate 3-stage pipeline
//             auto buf1 = std::move(buffer);
//             auto buf2 = std::move(buf1);
//             auto buf3 = std::move(buf2);

//             auto end = std::chrono::high_resolution_clock::now();
//             total_duration += (end - start);

//             // Prevent optimization
//             volatile float val = buf3[0];
//             (void)val;
//         }

//         auto avg_ns = total_duration.count() / iterations;
//         INFO("Average time (mutable chain): " << avg_ns << " ns");
//         REQUIRE(avg_ns < 1000000);  // Should be very fast
//     }

//     SECTION("copy chain") {
//         auto total_duration = std::chrono::nanoseconds{0};

//         for (std::size_t iter = 0; iter < iterations; ++iter) {
//             auto buffer = make_mutable<float>(buffer_size);

//             auto start = std::chrono::high_resolution_clock::now();

//             // Simulate 3-stage pipeline with copies
//             auto buf1 = buffer.copy();
//             auto buf2 = buf1.copy();
//             auto buf3 = buf2.copy();

//             auto end = std::chrono::high_resolution_clock::now();
//             total_duration += (end - start);

//             // Prevent optimization
//             volatile float val = buf3[0];
//             (void)val;
//         }

//         auto avg_ns = total_duration.count() / iterations;
//         INFO("Average time (copy chain): " << avg_ns << " ns");
//         // Copies should be slower
//     }
// }

// TEST_CASE("benchmark: immutable sharing vs copying", "[.benchmark]") {
//     constexpr std::size_t buffer_size = 1000000;
//     constexpr std::size_t num_outputs = 10;
//     constexpr std::size_t iterations = 100;

//     SECTION("immutable sharing (zero copy)") {
//         auto total_duration = std::chrono::nanoseconds{0};

//         for (std::size_t iter = 0; iter < iterations; ++iter) {
//             auto buffer = make_immutable<float>(buffer_size);

//             auto start = std::chrono::high_resolution_clock::now();

//             std::vector<immutable_buffer<float>> outputs;
//             for (std::size_t i = 0; i < num_outputs; ++i) {
//                 outputs.push_back(buffer.share());
//             }

//             auto end = std::chrono::high_resolution_clock::now();
//             total_duration += (end - start);
//         }

//         auto avg_ns = total_duration.count() / iterations;
//         INFO("Average time (immutable sharing): " << avg_ns << " ns");
//         REQUIRE(avg_ns < 1000000);
//     }

//     SECTION("mutable copying") {
//         auto total_duration = std::chrono::nanoseconds{0};

//         for (std::size_t iter = 0; iter < iterations; ++iter) {
//             auto buffer = make_mutable<float>(buffer_size);

//             auto start = std::chrono::high_resolution_clock::now();

//             std::vector<mutable_buffer<float>> outputs;
//             for (std::size_t i = 0; i < num_outputs; ++i) {
//                 outputs.push_back(buffer.copy());
//             }

//             auto end = std::chrono::high_resolution_clock::now();
//             total_duration += (end - start);
//         }

//         auto avg_ns = total_duration.count() / iterations;
//         INFO("Average time (mutable copying): " << avg_ns << " ns");
//         // Copying should be much slower
//     }
// }

// ============================================================================
// Timestamp Tests
// ============================================================================

TEST_CASE("timestamp comparison operators", "[timestamp][comparison]") {
    SECTION("equality") {
        timestamp ts1{100, 500'000'000'000};
        timestamp ts2{100, 500'000'000'000};
        timestamp ts3{100, 600'000'000'000};
        timestamp ts4{101, 500'000'000'000};

        REQUIRE(ts1 == ts2);
        REQUIRE_FALSE(ts1 == ts3);
        REQUIRE_FALSE(ts1 == ts4);
    }

    SECTION("less than") {
        timestamp ts1{100, 500'000'000'000};
        timestamp ts2{100, 600'000'000'000};
        timestamp ts3{101, 0};

        REQUIRE(ts1 < ts2);
        REQUIRE(ts1 < ts3);
        REQUIRE(ts2 < ts3);
        REQUIRE_FALSE(ts2 < ts1);
    }

    SECTION("greater than") {
        timestamp ts1{100, 500'000'000'000};
        timestamp ts2{100, 400'000'000'000};
        timestamp ts3{99, 900'000'000'000};

        REQUIRE(ts1 > ts2);
        REQUIRE(ts1 > ts3);
        REQUIRE_FALSE(ts2 > ts1);
    }

    SECTION("less than or equal") {
        timestamp ts1{100, 500'000'000'000};
        timestamp ts2{100, 500'000'000'000};
        timestamp ts3{100, 600'000'000'000};

        REQUIRE(ts1 <= ts2);
        REQUIRE(ts1 <= ts3);
        REQUIRE_FALSE(ts3 <= ts1);
    }

    SECTION("greater than or equal") {
        timestamp ts1{100, 500'000'000'000};
        timestamp ts2{100, 500'000'000'000};
        timestamp ts3{100, 400'000'000'000};

        REQUIRE(ts1 >= ts2);
        REQUIRE(ts1 >= ts3);
        REQUIRE_FALSE(ts3 >= ts1);
    }
}

TEST_CASE("timestamp arithmetic operations", "[timestamp][arithmetic]") {
    SECTION("timestamp difference") {
        timestamp ts1{100, 500'000'000'000}; // 100.5 seconds
        timestamp ts2{100, 0};               // 100.0 seconds

        auto diff = ts1 - ts2;
        REQUIRE(diff.count() == 500'000'000); // 0.5 seconds = 500M nanoseconds
    }

    SECTION("timestamp difference (negative)") {
        timestamp ts1{100, 0};
        timestamp ts2{100, 500'000'000'000};

        auto diff = ts1 - ts2;
        REQUIRE(diff.count() == -500'000'000); // -0.5 seconds
    }

    SECTION("timestamp difference (cross second boundary)") {
        timestamp ts1{101, 200'000'000'000}; // 101.2 seconds
        timestamp ts2{100, 800'000'000'000}; // 100.8 seconds

        auto diff = ts1 - ts2;
        REQUIRE(diff.count() == 400'000'000); // 0.4 seconds = 400M nanoseconds
    }

    SECTION("add positive duration") {
        timestamp ts{100, 0};
        auto dur = std::chrono::nanoseconds{500'000'000}; // 0.5 seconds

        auto result = ts + dur;
        REQUIRE(result.seconds == 100);
        REQUIRE(result.picoseconds == 500'000'000'000);
        REQUIRE(result.is_valid());
    }

    SECTION("add duration causing second rollover") {
        timestamp ts{100, 700'000'000'000};               // 100.7 seconds
        auto dur = std::chrono::nanoseconds{500'000'000}; // 0.5 seconds

        auto result = ts + dur;
        REQUIRE(result.seconds == 101);
        REQUIRE(result.picoseconds == 200'000'000'000); // 101.2 seconds total
        REQUIRE(result.is_valid());
    }

    SECTION("subtract duration") {
        timestamp ts{100, 500'000'000'000};
        auto dur = std::chrono::nanoseconds{200'000'000}; // 0.2 seconds

        auto result = ts - dur;
        REQUIRE(result.seconds == 100);
        REQUIRE(result.picoseconds == 300'000'000'000);
        REQUIRE(result.is_valid());
    }

    SECTION("subtract duration causing underflow throws") {
        timestamp ts{0, 100'000'000'000};                 // 0.1 seconds
        auto dur = std::chrono::nanoseconds{200'000'000}; // 0.2 seconds

        REQUIRE_THROWS_AS(ts - dur, std::underflow_error);
    }
}

TEST_CASE("timestamp chrono conversions", "[timestamp][chrono]") {
    SECTION("from_chrono basic") {
        auto tp =
            std::chrono::system_clock::time_point{std::chrono::seconds{100} + std::chrono::nanoseconds{500'000'000}};

        auto ts = timestamp::from_chrono(tp);
        REQUIRE(ts.seconds == 100);
        REQUIRE(ts.picoseconds == 500'000'000'000); // 500M ns = 500B ps
    }

    SECTION("to_chrono basic") {
        timestamp ts{100, 500'000'000'000};

        auto tp = ts.to_chrono();
        auto duration = tp.time_since_epoch();
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(duration);
        auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(duration - secs);

        REQUIRE(secs.count() == 100);
        REQUIRE(nanos.count() == 500'000'000);
    }

    SECTION("round-trip conversion") {
        auto original_tp = std::chrono::system_clock::now();

        auto ts = timestamp::from_chrono(original_tp);
        auto converted_tp = ts.to_chrono();

        // Should be equal within nanosecond precision
        auto diff = converted_tp - original_tp;
        auto ns_diff = std::chrono::duration_cast<std::chrono::nanoseconds>(diff);
        REQUIRE(std::abs(ns_diff.count()) < 1000); // Within 1 microsecond
    }

    SECTION("now returns valid timestamp") {
        auto ts = timestamp::now();
        REQUIRE(ts.is_valid());
        REQUIRE(ts.seconds > 0); // Should be well past epoch
    }
}

TEST_CASE("timestamp validation and normalization", "[timestamp][validation]") {
    SECTION("valid timestamp") {
        timestamp ts{100, 999'999'999'999}; // Just under 1 second
        REQUIRE(ts.is_valid());
    }

    SECTION("invalid timestamp (picoseconds >= 1 second)") {
        timestamp ts{100, 1'000'000'000'000}; // Exactly 1 second in picoseconds
        REQUIRE_FALSE(ts.is_valid());
    }

    SECTION("normalize removes excess picoseconds") {
        timestamp ts{100, 1'500'000'000'000}; // 1.5 seconds too many picoseconds
        REQUIRE_FALSE(ts.is_valid());

        ts.normalize();
        REQUIRE(ts.is_valid());
        REQUIRE(ts.seconds == 101);
        REQUIRE(ts.picoseconds == 500'000'000'000);
    }

    SECTION("normalize with multiple seconds") {
        timestamp ts{100, 3'200'000'000'000}; // 3.2 seconds too many
        ts.normalize();

        REQUIRE(ts.is_valid());
        REQUIRE(ts.seconds == 103);
        REQUIRE(ts.picoseconds == 200'000'000'000);
    }

    SECTION("normalize already valid timestamp does nothing") {
        timestamp ts{100, 500'000'000'000};
        auto orig_seconds = ts.seconds;
        auto orig_picos = ts.picoseconds;

        ts.normalize();
        REQUIRE(ts.seconds == orig_seconds);
        REQUIRE(ts.picoseconds == orig_picos);
    }
}

TEST_CASE("timestamp formatting", "[timestamp][formatting]") {
    SECTION("basic formatting") {
        timestamp ts{100, 500'000'000'000};
        auto str = ts.to_string();
        REQUIRE(str == "100.500000000000");
    }

    SECTION("format with leading zeros in picoseconds") {
        timestamp ts{42, 123'456'789}; // Small picosecond value
        auto str = ts.to_string();
        REQUIRE(str == "42.000123456789");
    }

    SECTION("format zero timestamp") {
        timestamp ts{0, 0};
        auto str = ts.to_string();
        REQUIRE(str == "0.000000000000");
    }

    SECTION("format large timestamp") {
        timestamp ts{999'999, 999'999'999'999};
        auto str = ts.to_string();
        REQUIRE(str == "999999.999999999999");
    }
}

TEST_CASE("timestamp edge cases", "[timestamp][edge_case]") {
    SECTION("zero timestamp") {
        timestamp ts{0, 0};
        REQUIRE(ts.is_valid());
        REQUIRE(ts == timestamp{0, 0});
    }

    SECTION("max picoseconds (just under 1 second)") {
        timestamp ts{100, 999'999'999'999};
        REQUIRE(ts.is_valid());
    }

    SECTION("arithmetic preserves normalization") {
        timestamp ts{100, 0};
        auto dur = std::chrono::nanoseconds{1'500'000'000}; // 1.5 seconds

        auto result = ts + dur;
        REQUIRE(result.is_valid());
        REQUIRE(result.seconds == 101);
        REQUIRE(result.picoseconds == 500'000'000'000);
    }

    SECTION("comparison works with unnormalized timestamps after normalize") {
        timestamp ts1{100, 1'500'000'000'000}; // Unnormalized
        timestamp ts2{101, 500'000'000'000};   // Normalized equivalent

        ts1.normalize();
        REQUIRE(ts1 == ts2);
    }
}

// ============================================================================
// Documentation Examples (Ensure they compile and work)
// ============================================================================

// TEST_CASE("README example: linear processing chain", "[examples]") {
//     // Verify the example from documentation actually works
//     auto source = std::make_shared<TestMutableSource>();
//     auto proc1 = std::make_shared<TestAmplifier>();
//     auto proc2 = std::make_shared<TestAmplifier>();
//     auto sink = std::make_shared<TestMutableSink>();

//     source->connect("data_out", proc1, "data_in");
//     proc1->connect("data_out", proc2, "data_in");
//     proc2->connect("data_out", sink, "data_in");

//     source->process();
//     std::this_thread::sleep_for(std::chrono::milliseconds(50));
//     proc1->process();
//     std::this_thread::sleep_for(std::chrono::milliseconds(50));
//     proc2->process();
//     std::this_thread::sleep_for(std::chrono::milliseconds(50));
//     sink->process();

//     REQUIRE(sink->m_received);
// }

// TEST_CASE("README example: broadcast pattern", "[examples]") {
//     auto source = std::make_shared<TestMutableSource>();
//     auto broadcaster = std::make_shared<TestBroadcaster>();
//     auto sink1 = std::make_shared<TestImmutableSink>();
//     auto sink2 = std::make_shared<TestImmutableSink>();

//     source->connect("data_out", broadcaster, "data_in");
//     broadcaster->connect("data_out1", sink1, "data_in");
//     broadcaster->connect("data_out2", sink2, "data_in");

//     source->process();
//     std::this_thread::sleep_for(std::chrono::milliseconds(50));
//     broadcaster->process();
//     std::this_thread::sleep_for(std::chrono::milliseconds(50));
//     sink1->process();
//     sink2->process();

//     REQUIRE(sink1->m_received);
//     REQUIRE(sink2->m_received);
// }
// ============================================================================
// Component Lifecycle and Port Management Tests
// ============================================================================

/**
 * @brief Simple sink with configurable input port depth for lifecycle testing
 */
class TestLifecycleSink : public component {
public:
    TestLifecycleSink() : component("TestLifecycleSink") {
        add_port(m_input);
        m_input.depth(100); // Default depth
    }

    auto process() -> retval override {
        auto [buffer, ts, metadata] = m_input.get_data();
        if (buffer.size() > 0) {
            m_packets_received.fetch_add(1, std::memory_order_relaxed);
            m_last_size = buffer.size();
        }
        return retval::NORMAL;
    }

    auto get_input_depth() const -> std::size_t { return m_input.depth(); }

    // Written by the worker (process()), read by the test's main thread while the worker
    // runs -> must be atomic (this counter races the main-thread reads below otherwise; the
    // doorbell's scheduling shift surfaced the latent race under TSan).
    std::atomic<std::size_t> m_packets_received{0};
    std::size_t m_last_size{0}; // worker-only (written, never read cross-thread)

private:
    input_port<immutable_buffer<float>> m_input{"data_in"};
    component::auto_stop m_auto_stop{*this}; // MUST be last
};

/**
 * @brief Continuous source for lifecycle testing
 */
class TestContinuousSource : public component {
public:
    TestContinuousSource() : component("TestContinuousSource") { add_port(m_output); }

    auto process() -> retval override {
        if (m_packets_sent >= m_max_packets) {
            return retval::NOOP;
        }

        auto buffer = make_immutable<float>(10);
        m_output.send_data(std::move(buffer), timestamp::now());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        m_packets_sent++;

        return retval::NORMAL;
    }

    std::size_t m_packets_sent{0};
    std::size_t m_max_packets{100};

private:
    output_port<immutable_buffer<float>> m_output{"data_out"};
    component::auto_stop m_auto_stop{*this}; // MUST be last
};

TEST_CASE("Component enabled property lifecycle", "[lifecycle][enabled]") {
    SECTION("Disabling component stops thread and pauses input ports") {
        auto sink = std::make_shared<TestLifecycleSink>();

        // Verify initial state
        REQUIRE(sink->get_property<bool>("enabled") == true);
        REQUIRE(sink->get_input_depth() == 100);

        // Start component
        sink->start();
        REQUIRE(wait_until([&] { return sink->is_running(); }));

        // Disable component via property (synchronous stop + pause)
        sink->set_properties({{"enabled", false}});
        sink->apply_lifecycle_changes();
        REQUIRE(wait_until([&] { return sink->get_input_depth() == 0; }));

        // Verify component stopped and input port paused
        REQUIRE(sink->get_property<bool>("enabled") == false);
        REQUIRE(sink->get_input_depth() == 0); // Should be paused
    }

    SECTION("Re-enabling component restores input port depths") {
        auto sink = std::make_shared<TestLifecycleSink>();

        // Set initial depth
        REQUIRE(sink->get_input_depth() == 100);

        // Start and then disable
        sink->start();
        REQUIRE(wait_until([&] { return sink->is_running(); }));
        sink->set_properties({{"enabled", false}});
        sink->apply_lifecycle_changes();
        REQUIRE(wait_until([&] { return sink->get_input_depth() == 0; }));

        // Verify paused
        REQUIRE(sink->get_input_depth() == 0);

        // Re-enable
        sink->set_properties({{"enabled", true}});
        sink->apply_lifecycle_changes();
        REQUIRE(wait_until([&] { return sink->get_input_depth() == 100; }));

        // Verify depth restored
        REQUIRE(sink->get_property<bool>("enabled") == true);
        REQUIRE(sink->get_input_depth() == 100); // Should be restored
    }

    SECTION("Paused input ports drop incoming data") {
        auto source = std::make_shared<TestContinuousSource>();
        auto sink = std::make_shared<TestLifecycleSink>();
        source->m_max_packets = 100000; // Ensure source keeps sending throughout test

        // Connect components
        source->connect("data_out", sink, "data_in");

        // Start both, then wait until the sink's worker has actually received some packets
        // (async processing — this is a genuine wait, not a synchronous lifecycle op).
        source->start();
        sink->start();
        REQUIRE(wait_until([&] { return sink->m_packets_received.load() > 0; }));

        auto initial_received = sink->m_packets_received.load();
        REQUIRE(initial_received > 0); // Should have received some packets

        // Disable sink (synchronous stop + pause to depth 0)
        sink->set_properties({{"enabled", false}});
        sink->apply_lifecycle_changes();
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // settle: let any in-flight packet land

        // Source continues sending, but sink's input port should drop everything
        auto packets_after_disable = sink->m_packets_received.load();
        // Allow a small number of packets that were in-flight during disable
        REQUIRE(packets_after_disable - initial_received <= 2);

        // Re-enable sink — it should start receiving again (async, so poll).
        sink->set_properties({{"enabled", true}});
        sink->apply_lifecycle_changes();
        REQUIRE(wait_until([&] { return sink->m_packets_received.load() > packets_after_disable; }));

        // Cleanup
        source->stop();
        sink->stop();
    }

    SECTION("Multiple enable/disable cycles preserve depths") {
        auto sink = std::make_shared<TestLifecycleSink>();
        sink->start();

        const std::size_t original_depth = 100;
        REQUIRE(sink->get_input_depth() == original_depth);

        // Cycle 1
        sink->set_properties({{"enabled", false}});
        sink->apply_lifecycle_changes();
        REQUIRE(wait_until([&] { return sink->get_input_depth() == 0; }));

        sink->set_properties({{"enabled", true}});
        sink->apply_lifecycle_changes();
        REQUIRE(wait_until([&] { return sink->get_input_depth() == original_depth; }));

        // Cycle 2
        sink->set_properties({{"enabled", false}});
        sink->apply_lifecycle_changes();
        REQUIRE(wait_until([&] { return sink->get_input_depth() == 0; }));

        sink->set_properties({{"enabled", true}});
        sink->apply_lifecycle_changes();
        REQUIRE(wait_until([&] { return sink->get_input_depth() == original_depth; }));

        sink->stop();
    }
}

TEST_CASE("Component lifecycle memory management", "[lifecycle][memory]") {
    SECTION("Disabled component prevents queue growth") {
        auto source = std::make_shared<TestContinuousSource>();
        auto sink = std::make_shared<TestLifecycleSink>();

        source->m_max_packets = 1000; // Lots of packets
        source->connect("data_out", sink, "data_in");

        // Start source and sink, then wait until the sink is actually receiving.
        source->start();
        sink->start();
        REQUIRE(wait_until([&] { return sink->m_packets_received.load() > 0; }));

        // Now disable it - synchronous stop + pause of the input ports.
        sink->set_properties({{"enabled", false}});
        sink->apply_lifecycle_changes();
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // settle: let any in-flight packet land

        auto packets_at_disable = sink->m_packets_received.load();

        // Let the source keep sending into the paused (depth-0) port for a window...
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // ...the sink must NOT receive any more (asserting absence over the window).
        REQUIRE(sink->m_packets_received.load() == packets_at_disable);

        source->stop();
        sink->stop();
    }
}
