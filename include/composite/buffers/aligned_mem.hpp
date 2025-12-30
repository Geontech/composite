/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdlib>
#include <format>
#include <memory>
#include <stdexcept>

namespace composite {

/**
 * @class aligned_mem
 * @brief A template class for managing aligned memory allocations with dynamic sizing
 * @tparam T The data type stored in the aligned memory buffer
 *
 * Provides memory allocation with user-specified alignment for SIMD operations (AVX/AVX2/AVX-512).
 * Supports deep-copying, moving, iterators, and safe element access.
 * Satisfies `std::ranges::contiguous_range` and `DynamicBufferContainer` concepts.
 *
 * Compatible with composite's buffer system for high-performance streaming applications.
 */
template <typename T>
class aligned_mem {
public:
    using value_type = T;                       ///< Element type stored in the buffer
    using size_type = std::size_t;              ///< Type used for sizes and indices
    using reference = value_type&;              ///< Mutable reference to element
    using const_reference = const value_type&;  ///< Const reference to element
    using iterator = value_type*;               ///< Iterator type (raw pointer)
    using const_iterator = const value_type*;   ///< Const iterator type (raw const pointer)

    /**
     * @brief Constructs an aligned memory buffer
     * @param alignment Memory alignment requirement (must be a power of 2)
     * @param count Number of elements of type T to allocate
     * @throws std::invalid_argument if alignment is not valid
     * @throws std::runtime_error if memory allocation fails
     */
    explicit aligned_mem(size_type alignment, size_type count) :
      m_data(),
      m_alignment(alignment),
      m_count(count),
      m_capacity(count) {
        if (alignment < alignof(std::max_align_t) || !std::has_single_bit(alignment)) {
            auto err = std::format(
              "invalid alignment: {}: must be a power of 2 and at least alignof(std::max_align_t)",
              alignment
            );
            throw std::invalid_argument(err);
        }
        if (count > 0) {
            m_data = static_cast<T*>(std::aligned_alloc(alignment, count * sizeof(T)));
            if (m_data == nullptr) {
                throw std::runtime_error("memory allocation failed");
            }
            // Default-initialize all elements
            for (size_type i = 0; i < count; ++i) {
                m_data[i] = T{};
            }
        }
    }

    /**
     * @brief Destructor that frees allocated memory
     */
    ~aligned_mem() {
        std::free(m_data);
    }

    /**
     * @brief Copy constructor (deep copy)
     * @param other The aligned_mem instance to copy
     * @throws std::runtime_error if memory allocation fails
     */
    aligned_mem(const aligned_mem& other) :
      m_alignment(other.m_alignment),
      m_count(other.m_count),
      m_capacity(other.m_capacity) {
        if (m_capacity > 0) {
            m_data = static_cast<T*>(std::aligned_alloc(m_alignment, m_capacity * sizeof(T)));
            if (m_data == nullptr) {
                throw std::runtime_error("memory allocation failed during copy construction");
            }
            std::copy(other.m_data, other.m_data + m_count, m_data);
        }
    }

    /**
     * @brief Copy assignment operator (deep copy)
     * @param other The aligned_mem instance to copy from
     * @return Reference to this instance
     * @throws std::runtime_error if memory allocation fails
     */
    auto operator=(const aligned_mem& other) -> aligned_mem& {
        if (this != &other) {
            T* new_data = nullptr;
            if (other.m_capacity > 0) {
                new_data = static_cast<T*>(
                  std::aligned_alloc(other.m_alignment, other.m_capacity * sizeof(T))
                );
                if (new_data == nullptr) {
                    throw std::runtime_error("memory allocation failed during copy assignment");
                }
                std::copy(other.m_data, other.m_data + other.m_count, new_data);
            }
            std::free(m_data);
            m_data = new_data;
            m_alignment = other.m_alignment;
            m_count = other.m_count;
            m_capacity = other.m_capacity;
        }
        return *this;
    }

    /**
     * @brief Move constructor (transfers ownership)
     * @param other The aligned_mem instance to move
     */
    aligned_mem(aligned_mem&& other) noexcept {
        *this = std::move(other);
    }

    /**
     * @brief Move assignment operator (transfers ownership)
     * @param other The aligned_mem instance to move from
     * @return Reference to this instance
     */
    auto operator=(aligned_mem<T>&& other) noexcept -> aligned_mem& {
        if (this != &other) {
            std::free(m_data);
            m_data = other.m_data;
            m_alignment = other.m_alignment;
            m_count = other.m_count;
            m_capacity = other.m_capacity;
            other.m_data = nullptr;
            other.m_count = 0;
            other.m_capacity = 0;
        }
        return *this;
    }

    /**
     * @brief Returns a pointer to the allocated memory
     * @return A pointer to the memory buffer
     *
     * The returned pointer is guaranteed to have the alignment specified
     * during construction. Safe to use with SIMD load/store intrinsics
     * that require aligned memory.
     */
    [[nodiscard]]
    auto data() noexcept -> T* {
        return m_data;
    }

    /**
     * @brief Returns a const pointer to the allocated memory
     * @return A const pointer to the memory buffer
     *
     * The returned pointer is guaranteed to have the alignment specified
     * during construction. Safe to use with SIMD load/store intrinsics
     * that require aligned memory.
     */
    [[nodiscard]]
    auto data() const noexcept -> const T* {
        return m_data;
    }

    /**
     * @brief Provides bounds-checked access to an element
     * @param pos The index of the element
     * @return Reference to the element at specified location @param pos
     * @throws std::out_of_range if @param pos is not within range
     */
    auto at(size_type pos) -> reference {
        if (pos >= m_count) {
            auto err = std::format(
                "aligned_mem range check: pos (which is {}) >= this->size() (which is {})",
                pos, m_count
            );
            throw std::out_of_range(err);
        }
        return m_data[pos];
    }

    /**
     * @brief Provides bounds-checked access to an element
     * @param pos The index of the element
     * @return Const reference to the element at specified location @param pos
     * @throws std::out_of_range if @param pos is not within range
     */
    auto at(size_type pos) const -> const_reference {
        if (pos >= m_count) {
            auto err = std::format(
                "aligned_mem range check: pos (which is {}) >= this->size() (which is {})",
                pos, m_count
            );
            throw std::out_of_range(err);
        }
        return m_data[pos];
    }

    /**
     * @brief Unchecked element access
     * @param pos The index of the element
     * @return Reference to the element at specified location
     */
    auto operator[](size_type pos) -> reference {
        return m_data[pos];
    }

    /**
     * @brief Unchecked element access
     * @param pos The index of the element
     * @return Const reference to the element at specified location
     */
    auto operator[](size_type pos) const -> const_reference {
        return m_data[pos];
    }

    /**
     * @brief Returns the memory alignment of the allocated buffer
     * @return The memory alignment value in bytes
     *
     * Common SIMD alignments:
     * - 16 bytes: SSE (128-bit)
     * - 32 bytes: AVX/AVX2 (256-bit)
     * - 64 bytes: AVX-512 (512-bit), cache line alignment
     */
    [[nodiscard]]
    auto alignment() const noexcept -> size_type {
        return m_alignment;
    }

    /**
     * @brief Returns the number of elements stored
     * @return The number of elements (not bytes)
     *
     * This is the logical size of the container. May be less than capacity()
     * if memory has been reserved but not used.
     */
    [[nodiscard]]
    auto size() const noexcept -> size_type {
        return m_count;
    }

    /**
     * @brief Returns the number of elements that can be stored without reallocation
     * @return The current capacity in elements
     *
     * Capacity is always >= size(). When size() < capacity(), operations like
     * resize() can grow the buffer efficiently without reallocating memory.
     */
    [[nodiscard]]
    auto capacity() const noexcept -> size_type {
        return m_capacity;
    }

    /**
     * @brief Checks if the buffer is empty
     * @return true if size() == 0, false otherwise
     */
    [[nodiscard]]
    auto empty() const noexcept -> bool {
        return m_count == 0;
    }

    /**
     * @brief Returns the number of bytes stored
     * @return The number of bytes (size() * sizeof(T))
     *
     * Useful for determining memory usage or for interfacing with APIs
     * that work with byte counts rather than element counts.
     */
    [[nodiscard]]
    auto size_bytes() const noexcept -> size_type {
        return m_count * sizeof(value_type);
    }

    /**
     * @brief Resizes the buffer
     *
     * Efficient in-place resize when new_size <= capacity (no reallocation).
     * When new_size > capacity, allocates new aligned memory and copies data.
     * New elements beyond old size are default-initialized.
     *
     * **Alignment Guarantee**: If reallocation occurs, the new memory maintains
     * the same alignment as the original buffer.
     *
     * @param new_size The new size in elements
     * @throws std::runtime_error if allocation fails
     */
    auto resize(size_type new_size) -> void {
        if (new_size <= m_capacity) {
            // Simple case: just update size
            // Only default-initialize new elements if growing
            if (new_size > m_count) {
                for (size_type i = m_count; i < new_size; ++i) {
                    m_data[i] = T{};
                }
            }
            m_count = new_size;
        } else {
            // Need to reallocate
            auto new_data = static_cast<T*>(std::aligned_alloc(m_alignment, new_size * sizeof(T)));
            if (new_data == nullptr) {
                throw std::runtime_error("memory allocation failed during resize");
            }

            // Copy existing elements
            if (m_data != nullptr) {
                std::copy(m_data, m_data + m_count, new_data);
            }

            // Default-initialize new elements
            for (size_type i = m_count; i < new_size; ++i) {
                new_data[i] = T{};
            }

            std::free(m_data);
            m_data = new_data;
            m_count = new_size;
            m_capacity = new_size;
        }
    }

    /**
     * @brief Reserves capacity without changing size
     *
     * Pre-allocates aligned memory for at least new_capacity elements.
     * Does not affect size() or existing data.
     * If new_capacity <= current capacity, this is a no-op.
     *
     * **Alignment Guarantee**: New memory maintains the same alignment as
     * the original buffer.
     *
     * @param new_capacity Target capacity in elements
     * @throws std::runtime_error if allocation fails
     */
    auto reserve(size_type new_capacity) -> void {
        if (new_capacity <= m_capacity) {
            return; // Already have enough capacity
        }

        auto new_data = static_cast<T*>(std::aligned_alloc(m_alignment, new_capacity * sizeof(T)));
        if (new_data == nullptr) {
            throw std::runtime_error("memory allocation failed during reserve");
        }

        // Copy existing data
        if (m_data != nullptr) {
            std::copy(m_data, m_data + m_count, new_data);
        }

        std::free(m_data);
        m_data = new_data;
        m_capacity = new_capacity;
    }

    /**
     * @brief Reduces capacity to match size
     *
     * Reallocates aligned memory to exactly fit current size.
     * This is a binding request.
     *
     * **Alignment Guarantee**: New memory maintains the same alignment as
     * the original buffer.
     *
     * @throws std::runtime_error if allocation fails
     */
    auto shrink_to_fit() -> void {
        if (m_capacity == m_count) {
            return; // Already tight
        }

        if (m_count == 0) {
            std::free(m_data);
            m_data = nullptr;
            m_capacity = 0;
            return;
        }

        auto new_data = static_cast<T*>(std::aligned_alloc(m_alignment, m_count * sizeof(T)));
        if (new_data == nullptr) {
            throw std::runtime_error("memory allocation failed during shrink_to_fit");
        }

        std::copy(m_data, m_data + m_count, new_data);
        std::free(m_data);
        m_data = new_data;
        m_capacity = m_count;
    }

    /**
     * @brief Clears all elements
     *
     * Sets size to 0 but preserves capacity and alignment.
     * After clear(), capacity() is unchanged but size() is 0.
     * No memory is deallocated or reallocated.
     */
    auto clear() -> void {
        m_count = 0;
    }

    /**
     * @brief Returns an iterator to the beginning of the buffer
     * @return Mutable iterator (pointer) to the first element
     *
     * The returned pointer is properly aligned and can be used with
     * range-based for loops or standard algorithms.
     */
    [[nodiscard]]
    auto begin() noexcept -> iterator {
        return m_data;
    }

    /**
     * @brief Returns an iterator past the end of the buffer
     * @return Mutable iterator (pointer) past the last element
     *
     * Standard C++ end() semantics - points one past the last element.
     * Do not dereference this iterator.
     */
    [[nodiscard]]
    auto end() noexcept -> iterator {
        return m_data + m_count;
    }

    /**
     * @brief Returns a const iterator to the beginning of the buffer
     * @return Const iterator (pointer) to the first element
     *
     * The returned pointer is properly aligned and can be used with
     * range-based for loops or standard algorithms.
     */
    [[nodiscard]]
    auto begin() const noexcept -> const_iterator {
        return m_data;
    }

    /**
     * @brief Returns a const iterator past the end of the buffer
     * @return Const iterator (pointer) past the last element
     *
     * Standard C++ end() semantics - points one past the last element.
     * Do not dereference this iterator.
     */
    [[nodiscard]]
    auto end() const noexcept -> const_iterator {
        return m_data + m_count;
    }

    /**
     * @brief Returns a const iterator to the beginning of the buffer
     * @return Const iterator (pointer) to the first element
     *
     * Explicitly const version for when you want to ensure const-correctness.
     */
    [[nodiscard]]
    auto cbegin() const noexcept -> const_iterator {
        return begin();
    }

    /**
     * @brief Returns a const iterator past the end of the buffer
     * @return Const iterator (pointer) past the last element
     *
     * Explicitly const version for when you want to ensure const-correctness.
     */
    [[nodiscard]]
    auto cend() const noexcept -> const_iterator {
        return end();
    };

private:
    value_type* m_data{nullptr}; ///< Pointer to allocated memory
    size_type m_alignment{alignof(std::max_align_t)}; ///< Memory alignment
    size_type m_count{0}; ///< Number of elements stored
    size_type m_capacity{0}; ///< Number of elements allocated

}; // class aligned_mem

/**
 * @brief Creates a `std::unique_ptr` to an aligned_mem instance
 * @tparam T The type stored in the aligned memory
 * @param alignment The memory alignment (must be a power of 2)
 * @param count Number of elements to allocate
 * @return A unique pointer to an aligned_mem<T> instance
 * @throws std::invalid_argument if alignment is invalid
 * @throws std::runtime_error if memory allocation fails
 *
 * Example usage:
 * @code
 * // Allocate 1024 floats with 32-byte alignment (for AVX/AVX2)
 * auto mem = composite::make_aligned<float>(32, 1024);
 *
 * // Use with composite buffers
 * mutable_buffer<float> buffer(std::move(mem));
 * @endcode
 */
template <typename T>
auto make_aligned(std::size_t alignment, std::size_t count) -> std::unique_ptr<aligned_mem<T>> {
    return std::make_unique<aligned_mem<T>>(alignment, count);
}

} // namespace composite
