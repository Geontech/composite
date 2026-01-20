/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <cassert>
#include <cstddef>
#include <functional>
#include <memory>
#include <utility>

namespace composite {

/**
 * @brief Shared ownership wrapper for externally managed buffers.
 *
 * external_buffer lets composite interoperate with memory allocated and owned
 * elsewhere (e.g., DMA regions or foreign runtimes) while providing shared
 * lifetime semantics. A user-supplied release callback is invoked exactly once
 * when the last reference goes out of scope, enabling custom cleanup strategies
 * without requiring composite to own allocation.
 *
 * @tparam T Element type stored in the buffer.
 */
template<typename T>
class external_buffer {
public:
    using value_type = T;
    using reference = T&;
    using pointer = T*;
    using iterator = T*;
    using const_iterator = const T*;

    /// @brief Construct an empty/null buffer.
    external_buffer() = default;

    /**
     * @brief Construct a buffer with a custom release callback.
     * @tparam Release Callable with signature void() that must be noexcept.
     * @param data Pointer to the buffer memory.
     * @param size Number of elements (not bytes).
     * @param release Callback invoked when the last copy of this buffer is destroyed.
     */
    template <typename Release>
    external_buffer(T* data, std::size_t size, Release&& release) {
        m_state = std::allocate_shared<state>(
            std::allocator<state>{},
            data,
            size,
            std::forward<Release>(release)
        );
    }

    // Standard Copy/Move semantics
    external_buffer(const external_buffer&) = default;
    external_buffer& operator=(const external_buffer&) = default;
    external_buffer(external_buffer&&) noexcept = default;
    external_buffer& operator=(external_buffer&&) noexcept = default;

    /// @brief Mutable pointer to the underlying data, or nullptr if empty.
    auto data() noexcept -> T* { return m_state ? m_state->data : nullptr; }
    /// @brief Const pointer to the underlying data, or nullptr if empty.
    auto data() const noexcept -> const T* { return m_state ? m_state->data : nullptr; }
    /// @brief Number of elements in the buffer.
    auto size() const noexcept -> std::size_t { return m_state ? m_state->size : 0; }
    /// @brief Iterator to the first element (nullptr for empty buffers).
    auto begin() noexcept -> iterator { return data(); }
    /// @brief Iterator to the first element (nullptr for empty buffers).
    auto begin() const noexcept -> const_iterator { return data(); }
    /// @brief Iterator to the first element (nullptr for empty buffers).
    auto cbegin() const noexcept -> const_iterator { return begin(); }
    /// @brief Iterator one-past-the-last element (safe for empty buffers).
    auto end() noexcept -> iterator {
        auto ptr = data();
        return (!ptr || size() == 0) ? ptr : ptr + size();
    }
    /// @brief Iterator one-past-the-last element (safe for empty buffers).
    auto end() const noexcept -> const_iterator {
        auto ptr = data();
        return (!ptr || size() == 0) ? ptr : ptr + size();
    }
    /// @brief Iterator one-past-the-last element (safe for empty buffers).
    auto cend() const noexcept -> const_iterator { return end(); }
    /// @brief Bounds-checked element access when assertions are enabled.
    T& operator[](std::size_t index) const {
        assert(m_state && index < m_state->size);
        return m_state->data[index];
    }
    /// @brief True when the buffer holds data.
    explicit operator bool() const noexcept { return m_state != nullptr; }
    /// @brief True when the buffer is empty or null.
    auto empty() const noexcept -> bool { return size() == 0; }

    /**
     * @brief Get a shared_ptr that shares ownership with this buffer.
     *
     * Enables zero-allocation construction of immutable_buffer by using
     * the shared_ptr aliasing pattern. The returned pointer keeps this
     * buffer's internal state alive.
     */
    auto ownership_handle() const noexcept -> std::shared_ptr<const void> {
        return std::static_pointer_cast<const void>(m_state);
    }

    /**
     * @brief Create a view of the first n elements (similar to std::views::take).
     *
     * Returns a new external_buffer viewing only the first n elements while
     * preserving the lifetime management of the original buffer. The original
     * buffer's release callback will be invoked when both the original and
     * the returned view are destroyed.
     *
     * This is useful when the actual data size is smaller than the allocated
     * buffer size (e.g., variable-length network packets in fixed-size buffers).
     *
     * @param n Number of elements to take (must be <= size())
     * @return New external_buffer viewing the first n elements
     * @throws std::invalid_argument if n > size()
     *
     * Example:
     * @code
     * auto pool_buffer = pool->acquire();  // 2048 bytes
     * // ... receive actual_length bytes ...
     * auto sized = std::move(pool_buffer).take(actual_length);  // e.g., 1080 bytes
     * // sized keeps pool_buffer alive, returns to pool when destroyed
     * @endcode
     */
    [[nodiscard]]
    auto take(std::size_t n) && -> external_buffer<T> {
        if (n > size()) {
            throw std::invalid_argument("external_buffer::take: n exceeds buffer size");
        }
        // Create new buffer with same data pointer but smaller size
        // Capture original state to keep it alive
        auto original_state = std::move(m_state);
        return external_buffer<T>(
            original_state->data,
            n,
            [state = std::move(original_state)]() {
                // Original state's release callback invoked when this lambda is destroyed
            }
        );
    }

private:
    struct state {
        using Deleter = std::function<void()>;
        state(T* d, std::size_t s, Deleter rel) :
          data(d),
          size(s),
          release(std::move(rel)) {}

        ~state() noexcept {
            if (!release) { return; }
            try {
                release();
            } catch (...) {
                // release callbacks must not throw; swallow to keep destructor noexcept
            }
        }

        T* data{nullptr};
        std::size_t size{};
        Deleter release;
    }; // struct state

    std::shared_ptr<state> m_state;

}; // class external_buffer

} // namespace composite
