/*
 * Copyright (C) 2024-2026 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <cassert>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <utility>

namespace composite {

/**
 * @brief Shared-ownership wrapper for externally managed buffer memory.
 *
 * Lets composite interoperate with memory owned elsewhere (a pool slab, a DMA
 * region, a foreign runtime) with shared lifetime semantics: a user-supplied
 * release callback runs exactly once when the last reference is destroyed.
 *
 * The buffer is a single `std::shared_ptr<T>` carrying the data pointer and a
 * custom deleter (the release callback), plus a size. The deleter lives inline in
 * the shared_ptr control block, so acquiring a handle is **one** allocation — not
 * the two (control block + a separate std::function capture) the previous
 * std::function-based design incurred. This matters on the pooled hot path.
 *
 * @tparam T Element type stored in the buffer.
 */
template <typename T>
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
     * @brief Construct a buffer over @p data with a custom release callback.
     * @tparam Release callable invoked as `release(T*)` when the last copy is
     *         destroyed (must be noexcept-safe — exceptions are swallowed).
     * @param data Pointer to the buffer memory.
     * @param size Number of elements (not bytes).
     * @param release Cleanup callback (e.g. return the slot to its pool).
     */
    template <typename Release>
    external_buffer(T* data, std::size_t size, Release&& release)
        : m_data(data, make_guard(std::forward<Release>(release))), m_size(size) {}

    external_buffer(const external_buffer&) = default;
    external_buffer& operator=(const external_buffer&) = default;
    external_buffer(external_buffer&&) noexcept = default;
    external_buffer& operator=(external_buffer&&) noexcept = default;

    auto data() noexcept -> T* { return m_data.get(); }
    auto data() const noexcept -> const T* { return m_data.get(); }
    auto size() const noexcept -> std::size_t { return m_data ? m_size : 0; }
    auto begin() noexcept -> iterator { return data(); }
    auto begin() const noexcept -> const_iterator { return data(); }
    auto cbegin() const noexcept -> const_iterator { return begin(); }
    auto end() noexcept -> iterator {
        auto* p = data();
        return (p == nullptr || size() == 0) ? p : p + size();
    }
    auto end() const noexcept -> const_iterator {
        auto* p = data();
        return (p == nullptr || size() == 0) ? p : p + size();
    }
    auto cend() const noexcept -> const_iterator { return end(); }
    T& operator[](std::size_t index) const {
        assert(m_data && index < m_size);
        return m_data.get()[index];
    }
    explicit operator bool() const noexcept { return m_data != nullptr; }
    auto empty() const noexcept -> bool { return size() == 0; }

    /**
     * @brief A shared_ptr that shares ownership with this buffer (zero-alloc
     * aliasing pattern used by immutable_buffer to adopt pooled memory).
     */
    auto ownership_handle() const& noexcept -> std::shared_ptr<const void> {
        return std::static_pointer_cast<const void>(m_data);
    }

    /// Move the existing ownership handle out without an atomic refcount
    /// increment/decrement pair. The buffer becomes empty.
    auto ownership_handle() && noexcept -> std::shared_ptr<const void> {
        return std::static_pointer_cast<const void>(std::move(m_data));
    }

    /**
     * @brief View the first @p n elements while preserving the release callback.
     *
     * Moves this buffer's ownership into the returned view with a reduced size; the
     * release fires when the view (and any copies) are destroyed. Zero-allocation
     * (no new control block — the same shared_ptr is moved).
     *
     * @throws std::invalid_argument if n > size()
     */
    [[nodiscard]]
    auto take(std::size_t n) && -> external_buffer<T> {
        if (n > size()) {
            throw std::invalid_argument("external_buffer::take: n exceeds buffer size");
        }
        external_buffer<T> view;
        view.m_data = std::move(m_data); // same data pointer + release, smaller logical size
        view.m_size = n;
        return view;
    }

private:
    /// Wrap a user release(T*) callback in a noexcept deleter for the shared_ptr.
    template <typename Release>
    static auto make_guard(Release&& release) {
        return [rel = std::forward<Release>(release)](T* p) noexcept {
            try {
                rel(p);
            } catch (...) { /* release must not throw; swallow */
            }
        };
    }

    std::shared_ptr<T> m_data; ///< data pointer + inline custom deleter (the release)
    std::size_t m_size{0};     ///< element count

}; // class external_buffer

} // namespace composite
