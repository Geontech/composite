/*
 * Copyright (C) 2025 Geon Technologies, LLC
 *
 * This file is part of composite.
 *
 * composite is free software: you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * composite is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/.
 */

#pragma once

#include "aligned_mem.hpp"
#include "composite/core/metadata.hpp"
#include "composite/core/timestamp.hpp"

#include <concepts>
#include <condition_variable>
#include <deque>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>

namespace composite {

// ============================================================================
// Container Validation Concept
// ============================================================================

/**
 * @brief Concept validating that a container is suitable for use with buffers
 *
 * Requirements:
 * - Must be a contiguous range (std::vector, std::array, etc.)
 * - Must have value_type matching the buffer element type T
 * - Must have data() method returning T* or convertible to T*
 * - Must have size() method returning std::size_t or convertible to std::size_t
 * - Must be copyable (required for mutable_buffer::copy())
 */
template<typename Container, typename T>
concept ValidBufferContainer =
    std::ranges::contiguous_range<Container> &&
    std::same_as<typename Container::value_type, T> &&
    std::copyable<Container> &&
    requires(Container c) {
        { c.data() } -> std::convertible_to<T*>;
        { c.size() } -> std::convertible_to<std::size_t>;
    };

/**
 * @brief Concept for containers supporting dynamic resizing operations
 *
 * Extends ValidBufferContainer with dynamic operations for runtime size management.
 * Containers satisfying this concept (like std::vector and aligned_mem) enable
 * mutable_buffer's resize(), reserve(), capacity(), shrink_to_fit(), and clear()
 * operations.
 *
 * Requirements:
 * - resize(size_t): Change container size
 * - reserve(size_t): Pre-allocate capacity
 * - capacity(): Query allocated capacity
 * - shrink_to_fit(): Reduce capacity to size
 * - clear(): Remove all elements
 */
template<typename Container>
concept DynamicBufferContainer =
    requires(Container c) {
        { c.resize(std::size_t{}) } -> std::same_as<void>;
        { c.reserve(std::size_t{}) } -> std::same_as<void>;
        { c.capacity() } -> std::convertible_to<std::size_t>;
        { c.shrink_to_fit() } -> std::same_as<void>;
        { c.clear() } -> std::same_as<void>;
    };

template <typename T> class mutable_buffer;

// ============================================================================
// Buffer Ownership Wrappers
// ============================================================================

/**
 * @brief Immutable buffer - shared read-only access to contiguous data
 *
 * Uses shared_ptr internally for reference-counted ownership. Perfect for fan-out
 * scenarios where multiple components need read-only access to the same data with
 * zero-copy semantics. Copying is cheap (only increments reference count).
 *
 * Immutable buffers guarantee:
 * - Data cannot be modified through this interface
 * - Multiple readers can share the same data safely
 * - Zero-copy slicing for efficient windowing/chunking
 * - Thread-safe copying and destruction (via std::shared_ptr)
 */
template <typename T>
class immutable_buffer {
public:
    using value_type = T;

    /**
     * @brief Default constructor - creates an empty buffer
     */
    immutable_buffer() = default;

    /**
     * @brief Construct from shared_ptr to any contiguous container
     * @tparam Container Any type satisfying ValidBufferContainer<T>
     * @param data Shared pointer to container holding the data
     *
     * Takes shared ownership of the container. Multiple immutable_buffers can
     * reference the same underlying container safely.
     */
    template<ValidBufferContainer<T> Container>
    explicit immutable_buffer(std::shared_ptr<Container> data) :
      m_data(std::static_pointer_cast<const void>(data)),
      m_span(std::as_bytes(std::span{data->data(), data->size()})),
      m_size(data->size()) {}

    /**
     * @brief Construct from unique_ptr (promotes to shared ownership)
     * @tparam Container Any type satisfying ValidBufferContainer<T>
     * @param data Unique pointer to container holding the data
     *
     * Takes ownership and promotes to shared ownership. This enables creating
     * an immutable buffer from exclusively-owned data.
     */
    template<ValidBufferContainer<T> Container>
    explicit immutable_buffer(std::unique_ptr<Container> data) :
      immutable_buffer(std::shared_ptr<Container>(std::move(data))) {}

    /**
     * @brief Copy constructor - shares ownership (cheap, only increments refcount)
     */
    immutable_buffer(const immutable_buffer&) = default;

    /**
     * @brief Copy assignment - shares ownership (cheap, only increments refcount)
     */
    immutable_buffer& operator=(const immutable_buffer&) = default;

    /**
     * @brief Move constructor - transfers ownership
     */
    immutable_buffer(immutable_buffer&&) = default;

    /**
     * @brief Move assignment - transfers ownership
     */
    immutable_buffer& operator=(immutable_buffer&&) = default;

    /**
     * @brief Get number of elements in the buffer
     * @return Number of elements (not bytes)
     */
    auto size() const -> std::size_t { return m_size; }

    /**
     * @brief Get const pointer to underlying data
     * @return Const pointer to first element
     */
    auto data() const -> const T* {
        return reinterpret_cast<const T*>(m_span.data());
    }

    /**
     * @brief Get a std::span view of the buffer
     * @return Read-only span over all elements
     */
    auto as_span() const -> std::span<const T> {
        return std::span<const T>{data(), size()};
    }

    /**
     * @brief Unchecked element access
     * @param idx Element index
     * @return Const reference to element at index
     */
    auto operator[](std::size_t idx) const -> const T& {
        return data()[idx];
    }

    /**
     * @brief Bounds-checked element access
     * @param idx Element index
     * @return Const reference to element at index
     * @throws std::out_of_range if idx >= size()
     */
    auto at(std::size_t idx) const -> const T& {
        if (idx >= m_size) {
            throw std::out_of_range(
              std::format("buffer index out of range: {} >= {}", idx, m_size)
            );
        }
        return data()[idx];
    }

    /**
     * @brief Get iterator to beginning
     * @return Const pointer to first element
     */
    auto begin() const { return data(); }

    /**
     * @brief Get iterator to end
     * @return Const pointer to one past the last element
     */
    auto end() const { return data() + size(); }

    /**
     * @brief Share buffer with another reader (zero-copy)
     * @return A new immutable_buffer sharing the same underlying data
     *
     * Creates a copy of this buffer that shares the same data. Very cheap
     * operation (only increments shared_ptr reference count). Equivalent to
     * copy construction but more explicit in intent.
     */
    auto share() const -> immutable_buffer { return *this; }

    /**
     * @brief Check if this is the only reference to the underlying data
     * @return true if this is the only buffer referencing this data
     *
     * Useful for optimization - if true, the data could potentially be
     * modified without affecting other readers (because there are none).
     */
    auto is_unique() const -> bool { return m_data.use_count() == 1; }

    /**
     * @brief Create a zero-copy view of a subset of this buffer
     *
     * Creates a new immutable_buffer that shares the same underlying data
     * but views only a subset of elements. Perfect for windowing, chunking,
     * and header/payload separation.
     *
     * @param offset Starting index (must be < size())
     * @param count Number of elements (use npos for "to end")
     * @return New immutable_buffer viewing the specified range
     * @throws std::out_of_range if offset or offset+count exceed buffer size
     *
     * @example
     * auto samples = make_immutable<float>(1024);
     * auto first_half = samples.slice(0, 512);    // Elements 0-511
     * auto second_half = samples.slice(512, 512); // Elements 512-1023
     * auto tail = samples.slice(100);             // Elements 100-1023
     */
    auto slice(std::size_t offset, std::size_t count = npos) const -> immutable_buffer {
        // Handle empty buffer case
        if (!has_data() || empty()) {
            if (offset != 0) {
                throw std::out_of_range("cannot slice empty buffer with non-zero offset");
            }
            return immutable_buffer{};
        }

        // Validate offset
        if (offset > m_size) {
            throw std::out_of_range(
                std::format("slice offset {} exceeds buffer size {}", offset, m_size)
            );
        }

        // Calculate actual count (handle npos = "to end")
        auto actual_count = (count == npos) ? (m_size - offset) : count;

        // Validate count
        if (offset + actual_count > m_size) {
            throw std::out_of_range(
                std::format("slice range [{}, {}) exceeds buffer size {}",
                           offset, offset + actual_count, m_size)
            );
        }

        // Create slice sharing same data
        immutable_buffer result;
        result.m_data = m_data;  // Share ownership
        result.m_size = actual_count;

        // Create subspan of the byte span
        auto byte_offset = offset * sizeof(T);
        auto byte_count = actual_count * sizeof(T);
        result.m_span = m_span.subspan(byte_offset, byte_count);

        return result;
    }

    /**
     * @brief Create a zero-copy view from offset to end of buffer
     *
     * Equivalent to slice(offset, npos).
     *
     * @param offset Starting index
     * @return New immutable_buffer viewing from offset to end
     * @throws std::out_of_range if offset exceeds buffer size
     */
    auto slice_from(std::size_t offset) const -> immutable_buffer {
        return slice(offset, npos);
    }

    /**
     * @brief Sentinel value for slice() meaning "to end of buffer"
     *
     * Use this value for the count parameter in slice() to indicate
     * "from offset to the end of the buffer".
     */
    static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

    /**
     * @brief Check if buffer contains zero elements
     * @return true if size() == 0
     */
    auto empty() const -> bool { return m_size == 0; }

    /**
     * @brief Check if buffer has allocated storage (may still be empty)
     * @return true if underlying data pointer is not null
     *
     * A buffer can have storage but be empty (size() == 0). This checks
     * only whether storage exists, not whether it contains elements.
     */
    auto has_data() const -> bool { return m_data != nullptr; }

    /**
     * @brief Check if buffer is usable (has storage and contains elements)
     * @return true if has_data() && !empty()
     *
     * Allows buffers to be used in boolean contexts: if (buffer) { ... }
     */
    explicit operator bool() const { return has_data() && !empty(); }

private:
    friend class mutable_buffer<T>;

    std::shared_ptr<const void> m_data;     ///< Type-erased immutable container
    std::span<const std::byte> m_span;      ///< View into raw bytes for type-safe access
    std::size_t m_size{0};                  ///< Number of elements (not bytes)
};

/**
 * @brief Mutable buffer - exclusive ownership of contiguous data
 *
 * Uses unique_ptr internally for exclusive ownership. Only one component can
 * modify the data at a time. When sent to multiple outputs, copies are made
 * for all but the last (which receives the moved buffer).
 *
 * Mutable buffers guarantee:
 * - Exclusive write access (move-only semantics)
 * - Can be converted to immutable for zero-copy sharing
 * - Supports in-place modifications
 * - Dynamic resizing for containers that support it (std::vector, aligned_mem)
 * - Type-erased storage allowing any ValidBufferContainer
 */
template <typename T>
class mutable_buffer {
    using deleter_type = void(*)(void*);
    using copier_type = std::function<std::unique_ptr<void, deleter_type>(const void*)>;
    using data_accessor_type = std::function<T*(void*)>;
    using resize_op_type = std::function<void(void*, std::size_t)>;
    using reserve_op_type = std::function<void(void*, std::size_t)>;
    using capacity_getter_type = std::function<std::size_t(const void*)>;
    using shrink_op_type = std::function<void(void*)>;
    using clear_op_type = std::function<void(void*)>;
public:
    using value_type = T;

    /**
     * @brief Default constructor - creates an empty buffer
     */
    mutable_buffer() = default;

    /**
     * @brief Construct from unique_ptr to any contiguous container
     * @tparam Container Any type satisfying ValidBufferContainer<T>
     * @param data Unique pointer to container holding the data
     *
     * Takes exclusive ownership of the container. Sets up type-erased
     * deleter, copier, and data accessor functions. If Container satisfies
     * DynamicBufferContainer, also enables resize/reserve/capacity operations.
     */
    template<ValidBufferContainer<T> Container>
    explicit mutable_buffer(std::unique_ptr<Container> data) :
        m_data(data.release(), [](void* p) {
            delete static_cast<Container*>(p);
        }),
        m_size(static_cast<Container*>(m_data.get())->size()),
        m_span_mut(std::as_writable_bytes(std::span{
            static_cast<Container*>(m_data.get())->data(),
            m_size
        })),
        m_copier([](const void* src) -> std::unique_ptr<void, void(*)(void*)> {
            auto* src_container = static_cast<const Container*>(src);
            auto new_container = std::make_unique<Container>(*src_container);
            return {new_container.release(), [](void* p) {
                delete static_cast<Container*>(p);
            }};
        }),
        m_data_accessor([](void* container_ptr) -> T* {
            return static_cast<Container*>(container_ptr)->data();
        })
    {
        // Only initialize capacity management operations for dynamic containers
        if constexpr (DynamicBufferContainer<Container>) {
            m_resize_op = [](void* container_ptr, std::size_t new_size) {
                static_cast<Container*>(container_ptr)->resize(new_size);
            };
            m_reserve_op = [](void* container_ptr, std::size_t new_capacity) {
                static_cast<Container*>(container_ptr)->reserve(new_capacity);
            };
            m_capacity_getter = [](const void* container_ptr) -> std::size_t {
                return static_cast<const Container*>(container_ptr)->capacity();
            };
            m_shrink_op = [](void* container_ptr) {
                static_cast<Container*>(container_ptr)->shrink_to_fit();
            };
            m_clear_op = [](void* container_ptr) {
                static_cast<Container*>(container_ptr)->clear();
            };
        }
    }

    /**
     * @brief Copy constructor - deleted (move-only, use copy() for deep copy)
     */
    mutable_buffer(const mutable_buffer&) = delete;

    /**
     * @brief Copy assignment - deleted (move-only, use copy() for deep copy)
     */
    mutable_buffer& operator=(const mutable_buffer&) = delete;

    /**
     * @brief Move constructor - transfers exclusive ownership
     * @param other Buffer to move from (will be left in empty state)
     */
    mutable_buffer(mutable_buffer&& other) noexcept
        : m_data(std::move(other.m_data)),
          m_size(other.m_size),
          m_span_mut(other.m_span_mut),
          m_copier(std::move(other.m_copier)),
          m_data_accessor(std::move(other.m_data_accessor)),
          m_resize_op(std::move(other.m_resize_op)),
          m_reserve_op(std::move(other.m_reserve_op)),
          m_capacity_getter(std::move(other.m_capacity_getter)),
          m_shrink_op(std::move(other.m_shrink_op)),
          m_clear_op(std::move(other.m_clear_op)) {
        // Reset moved-from object to empty state
        other.m_size = 0;
        other.m_span_mut = {};
    }

    /**
     * @brief Move assignment - transfers exclusive ownership
     * @param other Buffer to move from (will be left in empty state)
     * @return Reference to this buffer
     */
    mutable_buffer& operator=(mutable_buffer&& other) noexcept {
        if (this != &other) {
            m_data = std::move(other.m_data);
            m_size = other.m_size;
            m_span_mut = other.m_span_mut;
            m_copier = std::move(other.m_copier);
            m_data_accessor = std::move(other.m_data_accessor);
            m_resize_op = std::move(other.m_resize_op);
            m_reserve_op = std::move(other.m_reserve_op);
            m_capacity_getter = std::move(other.m_capacity_getter);
            m_shrink_op = std::move(other.m_shrink_op);
            m_clear_op = std::move(other.m_clear_op);

            // Reset moved-from object to empty state
            other.m_size = 0;
            other.m_span_mut = {};
        }
        return *this;
    }

    /**
     * @brief Get number of elements in the buffer
     * @return Number of elements (not bytes)
     */
    auto size() const -> std::size_t { return m_size; }

    /**
     * @brief Get mutable pointer to underlying data
     * @return Pointer to first element, or nullptr if buffer is empty
     */
    auto data() -> T* {
        return m_data ? reinterpret_cast<T*>(m_span_mut.data()) : nullptr;
    }

    /**
     * @brief Get const pointer to underlying data
     * @return Const pointer to first element, or nullptr if buffer is empty
     */
    auto data() const -> const T* {
        return m_data ? reinterpret_cast<const T*>(m_span_mut.data()) : nullptr;
    }

    /**
     * @brief Get a std::span view of the buffer (mutable)
     * @return Mutable span over all elements
     */
    auto as_span() -> std::span<T> {
        return std::span<T>{data(), size()};
    }

    /**
     * @brief Get a std::span view of the buffer (const)
     * @return Read-only span over all elements
     */
    auto as_span() const -> std::span<const T> {
        return std::span<const T>{data(), size()};
    }

    /**
     * @brief Unchecked mutable element access
     * @param idx Element index
     * @return Mutable reference to element at index
     */
    auto operator[](std::size_t idx) -> T& { return data()[idx]; }

    /**
     * @brief Unchecked const element access
     * @param idx Element index
     * @return Const reference to element at index
     */
    auto operator[](std::size_t idx) const -> const T& { return data()[idx]; }

    /**
     * @brief Bounds-checked mutable element access
     * @param idx Element index
     * @return Mutable reference to element at index
     * @throws std::out_of_range if idx >= size()
     */
    auto at(std::size_t idx) -> T& {
        if (idx >= m_size) {
            throw std::out_of_range(
              std::format("buffer index out of range: {} >= {}", idx, m_size)
            );
        }
        return data()[idx];
    }

    /**
     * @brief Bounds-checked const element access
     * @param idx Element index
     * @return Const reference to element at index
     * @throws std::out_of_range if idx >= size()
     */
    auto at(std::size_t idx) const -> const T& {
        if (idx >= m_size) {
            throw std::out_of_range(
              std::format("buffer index out of range: {} >= {}", idx, m_size)
            );
        }
        return data()[idx];
    }

    /**
     * @brief Get mutable iterator to beginning
     * @return Pointer to first element
     */
    auto begin() { return data(); }

    /**
     * @brief Get mutable iterator to end
     * @return Pointer to one past the last element
     */
    auto end() { return data() + size(); }

    /**
     * @brief Get const iterator to beginning
     * @return Const pointer to first element
     */
    auto begin() const { return data(); }

    /**
     * @brief Get const iterator to end
     * @return Const pointer to one past the last element
     */
    auto end() const { return data() + size(); }

    /**
     * @brief Create a deep copy of this buffer (new allocation)
     * @return A new mutable_buffer with a copy of the data
     *
     * Since mutable_buffer is move-only, this is the explicit way to
     * duplicate a buffer's data. Uses the type-erased copier function
     * to create a new container instance with copied contents.
     */
    auto copy() const -> mutable_buffer {
        mutable_buffer result;
        if (empty()) { return result; }

        // Use the copier to create a new instance
        result.m_data = m_copier(m_data.get());
        result.m_size = m_size;
        result.m_copier = m_copier;
        result.m_data_accessor = m_data_accessor;
        result.m_resize_op = m_resize_op;
        result.m_reserve_op = m_reserve_op;
        result.m_capacity_getter = m_capacity_getter;
        result.m_shrink_op = m_shrink_op;
        result.m_clear_op = m_clear_op;

        // Use the data accessor to get the actual data pointer from the container
        result.m_span_mut = std::as_writable_bytes(std::span{
            m_data_accessor(result.m_data.get()),
            m_size
        });
        return result;
    }

    /**
     * @brief Create an immutable zero-copy view of a subset of this buffer
     *
     * Returns an immutable_buffer viewing a subset of this mutable buffer's data.
     * This is safe because the returned buffer is read-only and shares ownership
     * via shared_ptr after conversion.
     *
     * Note: First converts to immutable (promoting to shared ownership), then slices.
     * This is a convenience method; for multiple slices, convert once then slice.
     *
     * @param offset Starting index (must be < size())
     * @param count Number of elements (use immutable_buffer<T>::npos for "to end")
     * @return New immutable_buffer viewing the specified range
     * @throws std::out_of_range if offset or offset+count exceed buffer size
     *
     * @example
     * auto mut_buf = make_mutable<float>(1024);
     * // ... populate data ...
     * auto window = mut_buf.slice(0, 512);  // Immutable view of first 512 elements
     */
    auto slice(std::size_t offset, std::size_t count = immutable_buffer<T>::npos) const -> immutable_buffer<T> {
        // Convert a copy to immutable, then slice
        // We need to copy because we can't move from const method
        auto immut = copy().to_immutable();
        return immut.slice(offset, count);
    }

    /**
     * @brief Create an immutable zero-copy view from offset to end
     *
     * @param offset Starting index
     * @return New immutable_buffer viewing from offset to end
     */
    auto slice_from(std::size_t offset) const -> immutable_buffer<T> {
        return slice(offset, immutable_buffer<T>::npos);
    }

    /**
     * @brief Convert to immutable buffer (promotes to shared ownership)
     * @return An immutable_buffer sharing ownership of this buffer's data
     *
     * Consumes this mutable_buffer (rvalue-qualified) and converts it to
     * an immutable_buffer with shared ownership semantics. This is a zero-cost
     * operation (no data copy). After conversion, this buffer is left empty.
     *
     * The resulting immutable_buffer can be cheaply copied and shared with
     * multiple readers.
     */
    auto to_immutable() && -> immutable_buffer<T> {
        // Early return for empty buffers
        if (empty() || !m_data) {
            return immutable_buffer<T>{};
        }

        // Validate container pointer
        auto* container_ptr = m_data.get();
        if (!container_ptr) {
            throw std::runtime_error("mutable_buffer::to_immutable: null container pointer");
        }

        // Get the actual data pointer using the accessor
        auto* data_ptr = m_data_accessor(container_ptr);
        if (!data_ptr) {
            throw std::runtime_error("mutable_buffer::to_immutable: data accessor returned null");
        }

        // Validate no overflow in byte size calculation
        auto size = m_size;
        constexpr auto max_size = std::numeric_limits<std::size_t>::max() / sizeof(T);
        if (size > max_size) {
            throw std::overflow_error(
                std::format("mutable_buffer::to_immutable: size {} exceeds maximum {} for type",
                           size, max_size)
            );
        }

        auto byte_size = size * sizeof(T);
        auto deleter = m_data.get_deleter();

        // Release ownership and transfer to shared_ptr
        auto* released_ptr = m_data.release();
        if (!released_ptr) {
            throw std::runtime_error("mutable_buffer::to_immutable: release returned null");
        }

        auto shared = std::shared_ptr<const void>(released_ptr, deleter);

        // Construct result immutable buffer
        immutable_buffer<T> result;
        result.m_data = shared;
        result.m_size = size;
        result.m_span = std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(data_ptr),
            byte_size
        };

        // Reset this buffer to empty state (we've consumed it)
        m_size = 0;
        m_span_mut = {};

        return result;
    }

    /**
     * @brief Check if buffer contains zero elements
     * @return true if size() == 0
     */
    auto empty() const -> bool { return m_size == 0; }

    /**
     * @brief Check if buffer has allocated storage (may still be empty)
     * @return true if underlying data pointer is not null
     *
     * A buffer can have storage but be empty (size() == 0). This checks
     * only whether storage exists, not whether it contains elements.
     */
    auto has_data() const -> bool { return m_data != nullptr; }

    /**
     * @brief Check if buffer is usable (has storage and contains elements)
     * @return true if has_data() && !empty()
     *
     * Allows buffers to be used in boolean contexts: if (buffer) { ... }
     */
    explicit operator bool() const { return has_data() && !empty(); }

    /**
     * @brief Resize buffer to new size
     *
     * If new_size > size(), buffer is expanded (new elements default-initialized).
     * If new_size < size(), buffer is truncated (excess elements discarded).
     * If new_size == size(), this is a no-op.
     *
     * @param new_size Target size in elements
     */
    auto resize(std::size_t new_size) -> void {
        if (!m_data || !m_resize_op) {
            throw std::runtime_error("cannot resize empty or uninitialized buffer");
        }

        m_resize_op(m_data.get(), new_size);
        m_size = new_size;

        // Update span to reflect new size
        m_span_mut = std::as_writable_bytes(std::span{
            m_data_accessor(m_data.get()),
            m_size
        });
    }

    /**
     * @brief Reserve capacity without changing size
     *
     * Pre-allocates storage for at least new_capacity elements. Does not
     * affect size() or existing data. If new_capacity <= capacity(), this is a no-op.
     *
     * @param new_capacity Target capacity in elements
     */
    auto reserve(std::size_t new_capacity) -> void {
        if (!m_data || !m_reserve_op) {
            throw std::runtime_error("cannot reserve on empty or uninitialized buffer");
        }

        m_reserve_op(m_data.get(), new_capacity);

        // Update span in case reallocation occurred
        m_span_mut = std::as_writable_bytes(std::span{
            m_data_accessor(m_data.get()),
            m_size
        });
    }

    /**
     * @brief Query current capacity
     *
     * @return Number of elements that can be stored without reallocation
     */
    auto capacity() const -> std::size_t {
        if (!m_data || !m_capacity_getter) {
            return 0;
        }
        return m_capacity_getter(m_data.get());
    }

    /**
     * @brief Release excess capacity
     *
     * Requests that capacity be reduced to match size. This is a non-binding
     * request; implementations may choose not to shrink.
     */
    auto shrink_to_fit() -> void {
        if (!m_data || !m_shrink_op) {
            return;  // No-op for empty buffers
        }

        m_shrink_op(m_data.get());

        // Update span in case reallocation occurred
        m_span_mut = std::as_writable_bytes(std::span{
            m_data_accessor(m_data.get()),
            m_size
        });
    }

    /**
     * @brief Clear all elements
     *
     * Removes all elements from the buffer (size becomes 0), but does not
     * release capacity. After clear(), capacity() is unchanged but size() is 0.
     */
    auto clear() -> void {
        if (!m_data || !m_clear_op) {
            return;  // No-op for empty buffers
        }

        m_clear_op(m_data.get());
        m_size = 0;
        m_span_mut = {};
    }

private:
    friend class immutable_buffer<T>;

    std::unique_ptr<void, deleter_type> m_data{nullptr, [](void*){}};   ///< Type-erased container with custom deleter
    std::size_t m_size{};                                               ///< Number of elements (not bytes)
    std::span<std::byte> m_span_mut;                                    ///< Mutable view into raw bytes
    copier_type m_copier;                                               ///< Type-erased copy operation
    data_accessor_type m_data_accessor;                                 ///< Type-erased data pointer accessor
    resize_op_type m_resize_op;                                         ///< Optional resize operation (dynamic containers only)
    reserve_op_type m_reserve_op;                                       ///< Optional reserve operation (dynamic containers only)
    capacity_getter_type m_capacity_getter;                             ///< Optional capacity query (dynamic containers only)
    shrink_op_type m_shrink_op;                                         ///< Optional shrink operation (dynamic containers only)
    clear_op_type m_clear_op;                                           ///< Optional clear operation (dynamic containers only)
};

// ============================================================================
// Helper Factory Functions
// ============================================================================

/**
 * @brief Create a mutable buffer with default container (std::vector)
 * @tparam T Element type
 * @param size Number of default-initialized elements
 * @return A mutable_buffer backed by std::vector<T>
 *
 * This is the most common way to create a mutable buffer. The underlying
 * std::vector is heap-allocated and supports dynamic resizing.
 */
template<typename T>
auto make_mutable(std::size_t size) -> mutable_buffer<T> {
    return mutable_buffer<T>(std::make_unique<std::vector<T>>(size));
}

/**
 * @brief Create a mutable buffer from existing data
 * @tparam T Element type
 * @param init Initializer list with element values
 * @return A mutable_buffer backed by std::vector<T>
 *
 * Convenient for creating buffers from literal values:
 * auto buf = make_mutable<float>({1.0f, 2.0f, 3.0f});
 */
template<typename T>
auto make_mutable(std::initializer_list<T> init) -> mutable_buffer<T> {
    return mutable_buffer<T>(std::make_unique<std::vector<T>>(init));
}

/**
 * @brief Create an immutable buffer with default container (std::vector)
 * @tparam T Element type
 * @param size Number of default-initialized elements
 * @return An immutable_buffer backed by std::vector<T>
 *
 * Creates an immutable buffer directly. Useful when you know the data
 * won't be modified and want to enable zero-copy sharing from the start.
 */
template<typename T>
auto make_immutable(std::size_t size) -> immutable_buffer<T> {
    return immutable_buffer<T>(std::make_shared<std::vector<T>>(size));
}

/**
 * @brief Create an immutable buffer from existing data
 * @tparam T Element type
 * @param init Initializer list with element values
 * @return An immutable_buffer backed by std::vector<T>
 *
 * Convenient for creating immutable buffers from literal values:
 * auto buf = make_immutable<int>({10, 20, 30, 40});
 */
template<typename T>
auto make_immutable(std::initializer_list<T> init) -> immutable_buffer<T> {
    return immutable_buffer<T>(std::make_shared<std::vector<T>>(init));
}

/**
 * @brief Create a mutable buffer from any compatible container
 * @tparam Container Any type satisfying ValidBufferContainer
 * @param container Unique pointer to the container
 * @return A mutable_buffer wrapping the container
 *
 * Generic wrapper for creating buffers from custom container types.
 * Useful when you have a specialized container (like std::array or
 * a custom SIMD-aligned container) and want buffer semantics.
 */
template<typename Container>
requires ValidBufferContainer<Container, typename Container::value_type>
auto wrap_mutable(std::unique_ptr<Container> container)
  -> mutable_buffer<typename Container::value_type> {
    return mutable_buffer<typename Container::value_type>(std::move(container));
}

/**
 * @brief Create an immutable buffer from any compatible container
 * @tparam Container Any type satisfying ValidBufferContainer
 * @param container Shared pointer to the container
 * @return An immutable_buffer wrapping the container
 *
 * Generic wrapper for creating immutable buffers from custom container types.
 * Since immutable buffers use shared ownership, the container must already
 * be in a shared_ptr.
 */
template<typename Container>
requires ValidBufferContainer<Container, typename Container::value_type>
auto wrap_immutable(std::shared_ptr<Container> container)
  -> immutable_buffer<typename Container::value_type> {
    return immutable_buffer<typename Container::value_type>(container);
}

/**
 * @brief Create a mutable buffer backed by aligned memory
 * @tparam T The element type
 * @param alignment Memory alignment requirement (must be a power of 2)
 * @param count Number of elements to allocate
 * @return A mutable buffer with guaranteed alignment for SIMD operations
 * @throws std::invalid_argument if alignment is not valid
 * @throws std::runtime_error if memory allocation fails
 *
 * This is a convenience function that combines aligned_mem allocation with
 * mutable_buffer creation. The resulting buffer is suitable for use with
 * SIMD intrinsics (SSE, AVX, AVX-512) that require aligned memory.
 *
 * Example usage:
 * @code
 * // Create buffer with 32-byte alignment for AVX
 * auto buffer = make_aligned_buffer<float>(32, 1024);
 *
 * // Use with AVX intrinsics
 * __m256 vec = _mm256_load_ps(&buffer[0]);  // Guaranteed aligned load
 *
 * // All buffer operations work normally
 * buffer.resize(2048);  // Maintains alignment
 * buffer.reserve(4096); // Pre-allocate aligned memory
 * @endcode
 */
template<typename T>
auto make_aligned_buffer(std::size_t alignment, std::size_t count) -> mutable_buffer<T> {
    return mutable_buffer<T>(make_aligned<T>(alignment, count));
}

/**
 * @brief Create an immutable buffer backed by aligned memory
 * @tparam T The element type
 * @param alignment Memory alignment requirement (must be a power of 2)
 * @param count Number of elements to allocate
 * @return An immutable buffer with guaranteed alignment for SIMD operations
 * @throws std::invalid_argument if alignment is not valid
 * @throws std::runtime_error if memory allocation fails
 *
 * Creates an immutable (read-only) buffer backed by aligned memory.
 * Useful when you need aligned memory for zero-copy sharing across multiple
 * consumers via the immutable buffer's share() semantics.
 *
 * Example usage:
 * @code
 * // Create aligned immutable buffer for broadcast scenarios
 * auto buffer = make_aligned_immutable_buffer<double>(64, 512);
 *
 * // Share with multiple consumers (zero-copy)
 * auto copy1 = buffer.share();
 * auto copy2 = buffer.share();
 * @endcode
 */
template<typename T>
auto make_aligned_immutable_buffer(std::size_t alignment, std::size_t count) -> immutable_buffer<T> {
    // Create mutable first, then convert to immutable
    return make_aligned_buffer<T>(alignment, count).to_immutable();
}

} // namespace composite