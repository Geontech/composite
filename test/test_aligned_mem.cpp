/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "composite/buffers/aligned_mem.hpp"
#include "composite/buffers/buffer.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace composite;

// ============================================================================
// Basic Construction and Properties
// ============================================================================

TEST_CASE("aligned_mem construction and basic properties", "[aligned_mem][basic]") {
    SECTION("construct with valid alignment") {
        aligned_mem<float> mem(32, 100);

        REQUIRE(mem.alignment() == 32);
        REQUIRE(mem.size() == 100);
        REQUIRE(mem.capacity() == 100);
        REQUIRE_FALSE(mem.empty());
        REQUIRE(mem.data() != nullptr);
        REQUIRE(mem.size_bytes() == 100 * sizeof(float));

        // Verify actual alignment
        auto addr = reinterpret_cast<std::uintptr_t>(mem.data());
        REQUIRE((addr % 32) == 0);
    }

    SECTION("construct with zero count") {
        aligned_mem<int> mem(64, 0);

        REQUIRE(mem.alignment() == 64);
        REQUIRE(mem.size() == 0);
        REQUIRE(mem.capacity() == 0);
        REQUIRE(mem.empty());
    }

    SECTION("invalid alignment throws") {
        // Not a power of 2
        REQUIRE_THROWS_AS(aligned_mem<float>(31, 10), std::invalid_argument);

        // Too small (less than alignof(std::max_align_t))
        if (alignof(std::max_align_t) > 1) {
            REQUIRE_THROWS_AS(aligned_mem<float>(1, 10), std::invalid_argument);
        }
    }

    SECTION("common SIMD alignments") {
        // SSE: 16 bytes
        aligned_mem<float> sse(16, 4);
        REQUIRE(reinterpret_cast<std::uintptr_t>(sse.data()) % 16 == 0);

        // AVX: 32 bytes
        aligned_mem<double> avx(32, 4);
        REQUIRE(reinterpret_cast<std::uintptr_t>(avx.data()) % 32 == 0);

        // AVX-512: 64 bytes
        aligned_mem<int> avx512(64, 16);
        REQUIRE(reinterpret_cast<std::uintptr_t>(avx512.data()) % 64 == 0);
    }
}

// ============================================================================
// Element Access
// ============================================================================

TEST_CASE("aligned_mem element access", "[aligned_mem][access]") {
    SECTION("unchecked access via operator[]") {
        aligned_mem<int> mem(32, 10);

        for (std::size_t i = 0; i < mem.size(); ++i) {
            mem[i] = static_cast<int>(i * 10);
        }

        for (std::size_t i = 0; i < mem.size(); ++i) {
            REQUIRE(mem[i] == static_cast<int>(i * 10));
        }
    }

    SECTION("checked access via at()") {
        aligned_mem<float> mem(32, 5);

        mem.at(0) = 1.0f;
        mem.at(4) = 5.0f;

        REQUIRE(mem.at(0) == 1.0f);
        REQUIRE(mem.at(4) == 5.0f);

        // Out of bounds throws
        REQUIRE_THROWS_AS(mem.at(5), std::out_of_range);
        REQUIRE_THROWS_AS(mem.at(100), std::out_of_range);
    }

    SECTION("const element access") {
        aligned_mem<int> mem(32, 3);
        mem[0] = 10;
        mem[1] = 20;
        mem[2] = 30;

        const auto& const_mem = mem;
        REQUIRE(const_mem[0] == 10);
        REQUIRE(const_mem.at(1) == 20);
        REQUIRE(const_mem.data()[2] == 30);
    }
}

// ============================================================================
// Iterators
// ============================================================================

TEST_CASE("aligned_mem iterators", "[aligned_mem][iterators]") {
    SECTION("mutable iterators") {
        aligned_mem<int> mem(32, 5);

        int value = 0;
        for (auto& elem : mem) {
            elem = value++;
        }

        REQUIRE(mem[0] == 0);
        REQUIRE(mem[4] == 4);
    }

    SECTION("const iterators") {
        aligned_mem<float> mem(32, 4);
        for (std::size_t i = 0; i < 4; ++i) {
            mem[i] = static_cast<float>(i * 2.5f);
        }

        const auto& const_mem = mem;
        float sum = 0.0f;
        for (const auto& elem : const_mem) {
            sum += elem;
        }

        REQUIRE(sum == (0.0f + 2.5f + 5.0f + 7.5f));
    }

    SECTION("iterator arithmetic") {
        aligned_mem<int> mem(32, 10);

        REQUIRE(mem.end() - mem.begin() == 10);
        REQUIRE(mem.cend() - mem.cbegin() == 10);
    }
}

// ============================================================================
// Copy and Move Semantics
// ============================================================================

TEST_CASE("aligned_mem copy semantics", "[aligned_mem][copy]") {
    SECTION("copy construction") {
        aligned_mem<float> mem1(32, 5);
        for (std::size_t i = 0; i < 5; ++i) {
            mem1[i] = static_cast<float>(i);
        }

        aligned_mem<float> mem2(mem1);

        REQUIRE(mem2.alignment() == mem1.alignment());
        REQUIRE(mem2.size() == mem1.size());
        REQUIRE(mem2.capacity() == mem1.capacity());
        REQUIRE(mem2.data() != mem1.data()); // Different memory

        for (std::size_t i = 0; i < 5; ++i) {
            REQUIRE(mem2[i] == mem1[i]);
        }

        // Modify copy doesn't affect original
        mem2[0] = 999.0f;
        REQUIRE(mem1[0] == 0.0f);
    }

    SECTION("copy assignment") {
        aligned_mem<int> mem1(32, 3);
        mem1[0] = 10;
        mem1[1] = 20;
        mem1[2] = 30;

        aligned_mem<int> mem2(64, 1);
        mem2 = mem1;

        REQUIRE(mem2.alignment() == 32);
        REQUIRE(mem2.size() == 3);
        REQUIRE(mem2[0] == 10);
        REQUIRE(mem2[1] == 20);
        REQUIRE(mem2[2] == 30);
    }
}

TEST_CASE("aligned_mem move semantics", "[aligned_mem][move]") {
    SECTION("move construction") {
        aligned_mem<float> mem1(32, 5);
        auto* original_ptr = mem1.data();
        mem1[0] = 42.0f;

        aligned_mem<float> mem2(std::move(mem1));

        REQUIRE(mem2.data() == original_ptr); // Same memory
        REQUIRE(mem2.alignment() == 32);
        REQUIRE(mem2.size() == 5);
        REQUIRE(mem2[0] == 42.0f);

        // Moved-from state
        REQUIRE(mem1.size() == 0);
        REQUIRE(mem1.capacity() == 0);
    }

    SECTION("move assignment") {
        aligned_mem<int> mem1(32, 3);
        auto* original_ptr = mem1.data();
        mem1[0] = 100;

        aligned_mem<int> mem2(64, 10);
        mem2 = std::move(mem1);

        REQUIRE(mem2.data() == original_ptr);
        REQUIRE(mem2.alignment() == 32);
        REQUIRE(mem2.size() == 3);
        REQUIRE(mem2[0] == 100);
    }
}

// ============================================================================
// Dynamic Operations
// ============================================================================

TEST_CASE("aligned_mem resize operations", "[aligned_mem][resize]") {
    SECTION("resize within capacity (shrink)") {
        aligned_mem<int> mem(32, 10);
        for (std::size_t i = 0; i < 10; ++i) {
            mem[i] = static_cast<int>(i);
        }

        auto* original_ptr = mem.data();
        mem.resize(5);

        REQUIRE(mem.size() == 5);
        REQUIRE(mem.capacity() == 10);
        REQUIRE(mem.data() == original_ptr); // No reallocation
        REQUIRE(mem[4] == 4);
    }

    SECTION("resize within capacity (grow)") {
        aligned_mem<float> mem(32, 10);
        mem[0] = 1.0f;
        mem.resize(5);
        mem.resize(8);

        REQUIRE(mem.size() == 8);
        REQUIRE(mem.capacity() == 10);
        REQUIRE(mem[0] == 1.0f);

        // New elements default-initialized
        REQUIRE(mem[7] == 0.0f);
    }

    SECTION("resize beyond capacity (reallocation)") {
        aligned_mem<int> mem(32, 5);
        for (std::size_t i = 0; i < 5; ++i) {
            mem[i] = static_cast<int>(i * 10);
        }

        mem.resize(20);

        REQUIRE(mem.size() == 20);
        REQUIRE(mem.capacity() == 20);

        // Old data preserved
        for (std::size_t i = 0; i < 5; ++i) {
            REQUIRE(mem[i] == static_cast<int>(i * 10));
        }

        // New elements default-initialized
        for (std::size_t i = 5; i < 20; ++i) {
            REQUIRE(mem[i] == 0);
        }

        // Still aligned
        auto addr = reinterpret_cast<std::uintptr_t>(mem.data());
        REQUIRE((addr % 32) == 0);
    }
}

TEST_CASE("aligned_mem reserve operations", "[aligned_mem][reserve]") {
    SECTION("reserve increases capacity") {
        aligned_mem<float> mem(32, 5);
        mem[0] = 42.0f;

        mem.reserve(100);

        REQUIRE(mem.size() == 5); // Size unchanged
        REQUIRE(mem.capacity() >= 100);
        REQUIRE(mem[0] == 42.0f); // Data preserved

        // Alignment maintained
        auto addr = reinterpret_cast<std::uintptr_t>(mem.data());
        REQUIRE((addr % 32) == 0);
    }

    SECTION("reserve with smaller capacity is no-op") {
        aligned_mem<int> mem(32, 20);
        auto* original_ptr = mem.data();

        mem.reserve(10);

        REQUIRE(mem.capacity() == 20);
        REQUIRE(mem.data() == original_ptr);
    }

    SECTION("resize after reserve doesn't reallocate") {
        aligned_mem<double> mem(64, 10);
        mem.reserve(1000);

        auto* ptr_after_reserve = mem.data();

        mem.resize(500);

        REQUIRE(mem.data() == ptr_after_reserve); // No reallocation
        REQUIRE(mem.size() == 500);
        REQUIRE(mem.capacity() == 1000);
    }
}

TEST_CASE("aligned_mem capacity operations", "[aligned_mem][capacity]") {
    SECTION("shrink_to_fit reduces capacity") {
        aligned_mem<float> mem(32, 10);
        mem.reserve(1000);
        REQUIRE(mem.capacity() == 1000);

        mem.shrink_to_fit();

        REQUIRE(mem.capacity() == mem.size());
        REQUIRE(mem.capacity() == 10);

        // Alignment maintained
        auto addr = reinterpret_cast<std::uintptr_t>(mem.data());
        REQUIRE((addr % 32) == 0);
    }

    SECTION("shrink_to_fit on empty buffer") {
        aligned_mem<int> mem(32, 10);
        mem.clear();
        mem.shrink_to_fit();

        REQUIRE(mem.size() == 0);
        REQUIRE(mem.capacity() == 0);
        REQUIRE(mem.data() == nullptr);
    }

    SECTION("clear preserves capacity") {
        aligned_mem<double> mem(64, 100);
        auto* original_ptr = mem.data();

        mem.clear();

        REQUIRE(mem.size() == 0);
        REQUIRE(mem.empty());
        REQUIRE(mem.capacity() == 100);
        REQUIRE(mem.data() == original_ptr);
    }
}

// ============================================================================
// Integration with composite buffers
// ============================================================================

TEST_CASE("aligned_mem with composite buffers", "[aligned_mem][buffer][integration]") {
    SECTION("use with mutable_buffer") {
        auto aligned = make_aligned<float>(32, 100);

        // Fill with data
        for (std::size_t i = 0; i < 100; ++i) {
            (*aligned)[i] = static_cast<float>(i * 0.5f);
        }

        // Create mutable buffer
        mutable_buffer<float> buffer(std::move(aligned));

        REQUIRE(buffer.size() == 100);
        REQUIRE(buffer[0] == 0.0f);
        REQUIRE(buffer[99] == 49.5f);

        // Alignment is preserved
        auto addr = reinterpret_cast<std::uintptr_t>(buffer.data());
        REQUIRE((addr % 32) == 0);
    }

    SECTION("use with immutable_buffer") {
        auto aligned = make_aligned<int>(64, 50);
        for (std::size_t i = 0; i < 50; ++i) {
            (*aligned)[i] = static_cast<int>(i * 2);
        }

        auto mutable_buf = mutable_buffer<int>(std::move(aligned));
        auto immutable_buf = std::move(mutable_buf).to_immutable();

        REQUIRE(immutable_buf.size() == 50);
        REQUIRE(immutable_buf[0] == 0);
        REQUIRE(immutable_buf[49] == 98);
    }

    SECTION("factory function") {
        auto aligned = make_aligned<double>(32, 256);

        REQUIRE(aligned != nullptr);
        REQUIRE(aligned->size() == 256);
        REQUIRE(aligned->alignment() == 32);

        auto addr = reinterpret_cast<std::uintptr_t>(aligned->data());
        REQUIRE((addr % 32) == 0);
    }
}

// ============================================================================
// SIMD Use Cases
// ============================================================================

TEST_CASE("aligned_mem SIMD readiness", "[aligned_mem][simd]") {
    SECTION("AVX alignment for float vector") {
        // AVX processes 8 floats (32 bytes) at a time
        constexpr std::size_t AVX_ALIGNMENT = 32;
        constexpr std::size_t VECTOR_SIZE = 1024;

        aligned_mem<float> mem(AVX_ALIGNMENT, VECTOR_SIZE);

        // Verify alignment for AVX _mm256_load_ps
        auto addr = reinterpret_cast<std::uintptr_t>(mem.data());
        REQUIRE((addr % AVX_ALIGNMENT) == 0);

        // Can safely use AVX aligned loads on this memory
        REQUIRE(mem.size() % 8 == 0); // Multiple of AVX vector width
    }

    SECTION("AVX-512 alignment") {
        // AVX-512 processes 16 floats (64 bytes) at a time
        constexpr std::size_t AVX512_ALIGNMENT = 64;
        constexpr std::size_t VECTOR_SIZE = 2048;

        aligned_mem<float> mem(AVX512_ALIGNMENT, VECTOR_SIZE);

        auto addr = reinterpret_cast<std::uintptr_t>(mem.data());
        REQUIRE((addr % AVX512_ALIGNMENT) == 0);

        REQUIRE(mem.size() % 16 == 0); // Multiple of AVX-512 vector width
    }
}

// ============================================================================
// Buffer Factory Functions
// ============================================================================

TEST_CASE("aligned buffer factory functions", "[aligned_mem][buffer][factory]") {
    SECTION("make_aligned_buffer creates mutable buffer") {
        auto buffer = make_aligned_buffer<float>(32, 100);

        REQUIRE(buffer.size() == 100);
        REQUIRE_FALSE(buffer.empty());

        // Verify alignment
        auto addr = reinterpret_cast<std::uintptr_t>(buffer.data());
        REQUIRE((addr % 32) == 0);

        // Can modify elements
        buffer[0] = 42.0f;
        buffer[99] = 99.0f;
        REQUIRE(buffer[0] == 42.0f);
        REQUIRE(buffer[99] == 99.0f);

        // Can use dynamic operations
        buffer.resize(200);
        REQUIRE(buffer.size() == 200);
        REQUIRE(buffer[0] == 42.0f); // Data preserved

        // Alignment maintained after resize
        addr = reinterpret_cast<std::uintptr_t>(buffer.data());
        REQUIRE((addr % 32) == 0);
    }

    SECTION("make_aligned_immutable_buffer creates immutable buffer") {
        auto buffer = make_aligned_immutable_buffer<int>(64, 50);

        REQUIRE(buffer.size() == 50);
        REQUIRE_FALSE(buffer.empty());

        // Verify alignment
        auto addr = reinterpret_cast<std::uintptr_t>(buffer.data());
        REQUIRE((addr % 64) == 0);

        // Can read elements
        REQUIRE(buffer[0] == 0); // Default-initialized

        // Can share (zero-copy)
        auto shared1 = buffer.share();
        auto shared2 = buffer.share();

        REQUIRE(shared1.size() == 50);
        REQUIRE(shared2.size() == 50);

        // All share the same underlying data
        REQUIRE(shared1.data() == buffer.data());
        REQUIRE(shared2.data() == buffer.data());
    }

    SECTION("make_aligned_buffer with AVX alignment") {
        constexpr std::size_t AVX_ALIGNMENT = 32;
        constexpr std::size_t VECTOR_SIZE = 1024;

        auto buffer = make_aligned_buffer<float>(AVX_ALIGNMENT, VECTOR_SIZE);

        // Fill with test data
        for (std::size_t i = 0; i < buffer.size(); ++i) {
            buffer[i] = static_cast<float>(i);
        }

        // Verify alignment for AVX intrinsics
        auto addr = reinterpret_cast<std::uintptr_t>(buffer.data());
        REQUIRE((addr % AVX_ALIGNMENT) == 0);
        REQUIRE(buffer.size() % 8 == 0); // Multiple of AVX vector width

        // Data is correct
        REQUIRE(buffer[0] == 0.0f);
        REQUIRE(buffer[1023] == 1023.0f);
    }

    SECTION("make_aligned_buffer with AVX-512 alignment") {
        constexpr std::size_t AVX512_ALIGNMENT = 64;

        auto buffer = make_aligned_buffer<double>(AVX512_ALIGNMENT, 256);

        auto addr = reinterpret_cast<std::uintptr_t>(buffer.data());
        REQUIRE((addr % AVX512_ALIGNMENT) == 0);
    }

    SECTION("convert between mutable and immutable aligned buffers") {
        // Create mutable aligned buffer
        auto mutable_buf = make_aligned_buffer<float>(32, 100);
        mutable_buf[0] = 1.0f;
        mutable_buf[99] = 99.0f;

        // Convert to immutable
        auto immutable_buf = std::move(mutable_buf).to_immutable();

        REQUIRE(immutable_buf.size() == 100);
        REQUIRE(immutable_buf[0] == 1.0f);
        REQUIRE(immutable_buf[99] == 99.0f);

        // Alignment preserved
        auto addr = reinterpret_cast<std::uintptr_t>(immutable_buf.data());
        REQUIRE((addr % 32) == 0);
    }

    SECTION("factory functions with invalid alignment throw") {
        REQUIRE_THROWS_AS(make_aligned_buffer<float>(31, 10), std::invalid_argument);
        REQUIRE_THROWS_AS(make_aligned_immutable_buffer<int>(15, 10), std::invalid_argument);
    }

    SECTION("factory convenience vs manual creation") {
        // Manual way
        auto aligned = make_aligned<float>(32, 100);
        mutable_buffer<float> manual_buffer(std::move(aligned));

        // Factory way
        auto factory_buffer = make_aligned_buffer<float>(32, 100);

        // Both achieve the same result
        REQUIRE(manual_buffer.size() == factory_buffer.size());

        auto addr1 = reinterpret_cast<std::uintptr_t>(manual_buffer.data());
        auto addr2 = reinterpret_cast<std::uintptr_t>(factory_buffer.data());
        REQUIRE((addr1 % 32) == 0);
        REQUIRE((addr2 % 32) == 0);
    }
}

// ============================================================================
// Edge Cases
// ============================================================================

TEST_CASE("aligned_mem edge cases", "[aligned_mem][edge]") {
    SECTION("self-assignment") {
        aligned_mem<int> mem(32, 5);
        mem[0] = 42;

        mem = mem; // Self-assignment

        REQUIRE(mem.size() == 5);
        REQUIRE(mem[0] == 42);
    }

    SECTION("multiple resize operations") {
        aligned_mem<float> mem(32, 10);
        mem[0] = 1.0f;

        mem.resize(20);
        mem.resize(5);
        mem.resize(15);

        REQUIRE(mem.size() == 15);
        REQUIRE(mem[0] == 1.0f);
    }

    SECTION("reserve-resize-shrink cycle") {
        aligned_mem<double> mem(64, 10);

        mem.reserve(1000);
        REQUIRE(mem.capacity() == 1000);

        mem.resize(500);
        REQUIRE(mem.size() == 500);

        mem.shrink_to_fit();
        REQUIRE(mem.capacity() == 500);

        // Alignment preserved through all operations
        auto addr = reinterpret_cast<std::uintptr_t>(mem.data());
        REQUIRE((addr % 64) == 0);
    }
}
