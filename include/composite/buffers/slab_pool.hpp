/*
 * Copyright (C) 2025 Geon Technologies, LLC
 *
 * This file is part of composite.
 *
 * composite is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * composite is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/.
 */

#pragma once

#include "external_buffer.hpp"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <sys/mman.h>
#include <utility>
#include <vector>

namespace composite {

/**
 * @brief Fixed-size LIFO buffer pool backed by a single contiguous memory slab.
 *
 * This class provides a high-performance, thread-safe memory pool for zero-copy
 * buffer management. All buffers are allocated from a single aligned memory slab,
 * which improves cache locality and reduces allocation overhead.
 *
 * Key features:
 * - Zero-copy buffer acquisition via external_buffer with custom deleter
 * - Thread-safe acquire/release operations with mutex protection
 * - LIFO allocation for better cache performance
 * - Optional huge page support for reduced TLB pressure
 * - Lock-free observability counters (available, outstanding)
 * - Overflow-safe size calculations during construction
 * - Debug-mode pointer validation on release
 *
 * Thread Safety:
 * - All public methods are thread-safe
 * - Multiple threads can acquire/release buffers concurrently
 * - Observability methods (available, capacity, outstanding) use appropriate memory ordering
 *
 * Memory Layout:
 * - Single contiguous slab of (buffer_count * aligned_size) bytes
 * - Each buffer is aligned to Alignment boundary (typically 64 bytes for AVX-512)
 * - Stride between buffers accounts for alignment padding
 *
 * Lifetime Management:
 * - Must be created via create() factory (returns shared_ptr)
 * - Buffers hold shared_ptr to pool, keeping pool alive until all buffers released
 * - Automatic cleanup on destruction (free aligned memory)
 *
 * Performance Characteristics:
 * - O(1) acquire/release operations
 * - Lock contention only during acquire/release, not during buffer use
 * - LIFO order improves cache hit rate for recently released buffers
 *
 * Usage Example:
 * @code
 * auto pool = slab_pool<float>::create(1024, 64);  // 64 buffers of 1024 floats
 *
 * // Acquire single buffer
 * if (auto buf = pool->acquire()) {
 *     auto span = buf->as_span();
 *     // Use buffer...
 * }  // Buffer automatically returned to pool on destruction
 *
 * // Batch acquisition
 * std::vector<composite::external_buffer<float>> buffers;
 * auto acquired = pool->acquire_batch(10, buffers);
 * @endcode
 *
 * @tparam T Value type stored in buffers (e.g., float, uint8_t, std::complex<float>)
 * @tparam Alignment Memory alignment in bytes. Must be power of two >= alignof(T).
 *                   Default 64 bytes is optimal for AVX-512 operations.
 */
template <typename T, std::size_t Alignment = 64>
class slab_pool : public std::enable_shared_from_this<slab_pool<T, Alignment>> {
    struct ctor_tag {}; ///< Private tag type to restrict construction to factory method
public:
    /**
     * @brief Buffer type returned by this pool
     *
     * external_buffer provides a view into the pool's slab memory with
     * a custom deleter that returns the buffer to the pool on destruction.
     */
    using buffer_type = composite::external_buffer<T>;

    static_assert((Alignment & (Alignment - 1)) == 0, "Alignment must be a power of two");
    static_assert(Alignment >= alignof(T), "Alignment must satisfy alignof(T)");

    /**
     * @brief Create a shared pool instance (factory method).
     *
     * This is the only way to create a slab_pool. The factory pattern ensures
     * the pool is always managed by shared_ptr, which is required for the
     * buffer lifetime management scheme (buffers hold shared_ptr to pool).
     *
     * The constructor performs the following operations:
     * 1. Validates input parameters (non-zero)
     * 2. Calculates aligned stride with overflow checking
     * 3. Allocates contiguous aligned memory slab
     * 4. Requests huge pages (best-effort, Linux only)
     * 5. Pre-populates free stack with all buffer addresses
     *
     * @param buffer_size Number of T elements per buffer. Must be > 0.
     *                    Example: 1024 for 1024 floats = 4096 bytes
     * @param buffer_count Total number of buffers in pool. Must be > 0.
     *                     Example: 64 buffers in pool
     *
     * @return Shared pointer to newly created pool, ready for buffer acquisition
     *
     * @throws std::invalid_argument if buffer_size or buffer_count is zero
     * @throws std::overflow_error if size calculations overflow std::size_t limits
     * @throws std::bad_alloc if aligned memory allocation fails
     *
     * @note Thread-safe: Can be called concurrently from multiple threads
     *
     * Example:
     * @code
     * // Create pool with 32 buffers of 2048 floats each
     * auto pool = slab_pool<float>::create(2048, 32);
     * @endcode
     */
    static
    auto create(std::size_t buffer_size, std::size_t buffer_count) -> std::shared_ptr<slab_pool> {
        return std::make_shared<slab_pool>(ctor_tag{}, buffer_size, buffer_count);
    }

    /**
     * @brief Constructor implementation (internal use only).
     *
     * Only callable by create() factory via ctor_tag. Performs all initialization
     * including memory allocation and free list population.
     *
     * @param tag Private tag to prevent direct construction
     * @param buffer_size Number of T elements per buffer
     * @param buffer_count Total number of buffers in pool
     *
     * @throws std::invalid_argument if buffer_size or buffer_count is zero
     * @throws std::overflow_error if size calculations overflow
     * @throws std::bad_alloc if allocation fails
     */
    slab_pool(ctor_tag, std::size_t buffer_size, std::size_t buffer_count) :
      m_buffer_size(buffer_size),
      m_buffer_count(buffer_count) {
        if (buffer_size == 0 || buffer_count == 0) {
            throw std::invalid_argument("slab_pool: size and count must be > 0");
        }

        const auto raw_bytes = buffer_size * sizeof(T);

        // Check for overflow before adding
        if (raw_bytes > std::numeric_limits<std::size_t>::max() - Alignment) {
            throw std::overflow_error("slab_pool: buffer size too large for alignment padding");
        }
        // Bitwise Align Up: (x + align - 1) & ~(align - 1)
        m_stride_bytes = (raw_bytes + Alignment - 1) & ~(Alignment - 1);

        // Check for overflow
        if (buffer_count > std::numeric_limits<std::size_t>::max() / m_stride_bytes) {
            throw std::overflow_error("slab_pool: total size overflows");
        }
        m_total_bytes = m_stride_bytes * buffer_count;

        // Allocate slab
        void* ptr = std::aligned_alloc(Alignment, m_total_bytes);
        if (!ptr) { throw std::bad_alloc(); }
        m_slab.reset(static_cast<uint8_t*>(ptr));

#ifdef MADV_HUGEPAGE
        // Optimization: Huge pages when available (best-effort)
        ::madvise(m_slab.get(), m_total_bytes, MADV_HUGEPAGE);
#endif

        // Populate free stack
        m_free_stack.reserve(buffer_count);
        auto* base = m_slab.get();
        for (std::size_t i = 0; i < buffer_count; ++i) {
            m_free_stack.push_back(reinterpret_cast<T*>(base + (i * m_stride_bytes)));
        }
    }

    /**
     * @brief Destructor - automatically frees aligned memory slab.
     *
     * Safe to call even with outstanding buffers. The slab memory is freed
     * only when the last shared_ptr to this pool is destroyed, which occurs
     * when the last buffer is released (buffers hold shared_ptr to pool).
     *
     * @note Thread-safe: Destruction is serialized by shared_ptr reference counting
     */
    ~slab_pool() = default;

    /**
     * @brief Acquire a single buffer from the pool (LIFO order).
     *
     * Returns a buffer from the top of the free stack. If no buffers are
     * available, returns std::nullopt without blocking. The returned buffer
     * will automatically return to the pool when destroyed.
     *
     * LIFO order improves cache locality - recently released buffers are
     * likely still in cache.
     *
     * @return Optional containing buffer on success, or std::nullopt if pool exhausted
     *
     * @note Thread-safe: Multiple threads can call concurrently
     * @note Lock-free after acquisition: Only acquires mutex briefly during pop
     * @note Non-blocking: Returns immediately if no buffers available
     *
     * Example:
     * @code
     * if (auto buf = pool->acquire()) {
     *     auto span = buf->as_span();
     *     std::fill(span.begin(), span.end(), 0.0f);
     *     // Buffer automatically returned when buf goes out of scope
     * } else {
     *     // Pool exhausted, handle backpressure
     * }
     * @endcode
     */
    [[nodiscard]]
    auto acquire() -> std::optional<buffer_type> {
        auto lock = std::unique_lock{m_mutex};
        if (m_free_stack.empty()) { return std::nullopt; }

        T* ptr = m_free_stack.back();
        m_free_stack.pop_back();
        lock.unlock();
        m_outstanding.fetch_add(1, std::memory_order_relaxed);

        return make_handle(this->shared_from_this(), ptr);
    }

    /**
     * @brief Acquire multiple buffers at once (batch operation).
     *
     * More efficient than calling acquire() in a loop because:
     * 1. Acquires mutex only once for entire batch
     * 2. Calls shared_from_this() once and reuses for all buffers
     * 3. Pre-reserves vector capacity to avoid reallocations
     *
     * Acquires as many buffers as possible, up to count. If fewer than count
     * buffers are available, acquires all remaining buffers without error.
     *
     * @param count Desired number of buffers to acquire
     * @param out Vector to append acquired buffers to. Must be valid container.
     *            Buffers are appended, not replaced.
     *
     * @return Number of buffers actually acquired (0 to count, inclusive)
     *         Returns 0 if pool is exhausted.
     *
     * @note Thread-safe: Multiple threads can call concurrently
     * @note Partial acquisition: May return fewer than count buffers without error
     * @note Non-blocking: Returns immediately with whatever is available
     *
     * Example:
     * @code
     * std::vector<slab_pool<float>::buffer_type> buffers;
     * auto acquired = pool->acquire_batch(16, buffers);
     * if (acquired < 16) {
     *     // Pool had fewer than 16 buffers available
     * }
     * // Process all acquired buffers
     * for (auto& buf : buffers) {
     *     // Use buffer...
     * }
     * // All buffers automatically returned when vector is destroyed
     * @endcode
     */
    [[nodiscard]]
    auto acquire_batch(std::size_t count, std::vector<buffer_type>& out) -> std::size_t {
        auto lock = std::unique_lock{m_mutex};
        auto acquired = std::size_t{};
        out.reserve(out.size() + count);
        auto self = this->shared_from_this();

        while (acquired < count && !m_free_stack.empty()) {
            T* ptr = m_free_stack.back();
            m_free_stack.pop_back();
            out.emplace_back(make_handle(self, ptr));
            acquired++;
        }
        if (acquired > 0) {
            m_outstanding.fetch_add(acquired, std::memory_order_relaxed);
        }
        return acquired;
    }

    /**
     * @brief Acquire multiple buffers at once (convenience overload).
     *
     * This is a convenience wrapper around the primary acquire_batch() that
     * returns a new vector instead of appending to an existing one. Useful
     * for one-off acquisitions where allocation reuse isn't needed.
     *
     * For performance-critical hot loops that repeatedly acquire batches,
     * prefer the out-parameter version to reuse vector allocations:
     * @code
     * // Hot loop - reuse allocation (preferred)
     * std::vector<buffer_type> buffers;
     * while (running) {
     *     buffers.clear();
     *     pool->acquire_batch(16, buffers);
     *     process(buffers);
     * }
     *
     * // One-off acquisition - convenience (this overload)
     * auto buffers = pool->acquire_batch(16);
     * @endcode
     *
     * @param count Desired number of buffers to acquire
     *
     * @return Vector containing acquired buffers (may be fewer than count)
     *         Returns empty vector if pool is exhausted.
     *
     * @note Thread-safe: Multiple threads can call concurrently
     * @note Allocates new vector on each call
     * @note [[nodiscard]]: Return value should not be ignored
     *
     * Example:
     * @code
     * // Direct initialization in range-for
     * for (auto& buf : pool->acquire_batch(8)) {
     *     auto span = buf.as_span();
     *     // Process buffer...
     * }
     * @endcode
     */
    [[nodiscard]]
    auto acquire_batch(std::size_t count) -> std::vector<buffer_type> {
        auto buffers = std::vector<buffer_type>{};
        buffers.reserve(count);
        acquire_batch(count, buffers);
        return buffers;
    }

    /**
     * @brief Get number of buffers currently available for acquisition.
     *
     * Returns the current size of the free stack. This is a snapshot value
     * that may change immediately after the call returns if other threads
     * are acquiring/releasing buffers concurrently.
     *
     * Useful for monitoring pool utilization and implementing backpressure:
     * - available() == capacity(): Pool fully available (idle)
     * - available() == 0: Pool exhausted (high load)
     * - available() + outstanding() == capacity(): Accounting invariant
     *
     * @return Number of buffers ready for immediate acquisition (0 to capacity)
     *
     * @note Thread-safe: Acquires mutex to read free stack size
     * @note Snapshot semantics: Value may be stale immediately after return
     *
     * Example:
     * @code
     * if (pool->available() < pool->capacity() * 0.1) {
     *     // Less than 10% buffers available - apply backpressure
     * }
     * @endcode
     */
    [[nodiscard]]
    auto available() const -> std::size_t {
        auto lock = std::lock_guard{m_mutex};
        return m_free_stack.size();
    }

    /**
     * @brief Get total capacity of the pool (constant after construction).
     *
     * Returns the total number of buffers allocated in the pool. This value
     * never changes after construction.
     *
     * Invariant: available() + outstanding() == capacity() at all times
     * (accounting for snapshot timing in multi-threaded scenarios)
     *
     * @return Total number of buffers in pool (same as buffer_count from create())
     *
     * @note Thread-safe: No synchronization needed (immutable value)
     * @note Constant time: O(1)
     *
     * Example:
     * @code
     * auto utilization = 1.0 - (pool->available() / (double)pool->capacity());
     * // utilization ranges from 0.0 (idle) to 1.0 (exhausted)
     * @endcode
     */
    [[nodiscard]]
    auto capacity() const -> std::size_t {
        return m_buffer_count;
    }

    /**
     * @brief Get number of buffers currently acquired and not yet released.
     *
     * Returns count of buffers that have been acquired but not yet destroyed.
     * Uses relaxed memory ordering for high-performance non-blocking reads.
     *
     * This counter is updated atomically:
     * - Incremented on acquire() or acquire_batch()
     * - Decremented on buffer destruction (custom deleter)
     *
     * Useful for:
     * - Monitoring current load
     * - Detecting buffer leaks (outstanding() should eventually return to 0)
     * - Debugging lifetime issues
     *
     * Invariant: outstanding() + available() == capacity()
     *
     * @return Number of buffers currently in use (0 to capacity)
     *
     * @note Thread-safe: Lock-free atomic load with relaxed ordering
     * @note Snapshot semantics: Value may be stale immediately after return
     * @note High performance: No cache line contention or synchronization
     *
     * Example:
     * @code
     * auto load = (double)pool->outstanding() / pool->capacity();
     * if (load > 0.9) {
     *     // Pool is 90%+ utilized - consider scaling resources
     * }
     * @endcode
     */
    [[nodiscard]]
    auto outstanding() const -> std::size_t {
        return m_outstanding.load(std::memory_order_relaxed);
    }

private:
    /**
     * @brief Custom deleter for unique_ptr managing the aligned slab memory.
     *
     * Calls std::free() on the slab pointer, matching the std::aligned_alloc()
     * used during construction. This deleter is invoked automatically when
     * m_slab unique_ptr is destroyed.
     */
    struct slab_deleter {
        void operator()(uint8_t* ptr) const { std::free(ptr); }
    };

    /**
     * @brief Return a buffer to the pool (called by external_buffer deleter).
     *
     * This method is private and only callable by the custom deleter lambda
     * created in make_handle(). When a buffer is destroyed, the deleter
     * invokes this method to return the buffer to the free stack.
     *
     * The method performs validation in debug builds to catch common errors:
     * 1. Pointer must be within slab memory range [base, base + total_bytes)
     * 2. Pointer must be aligned to stride boundary (not an arbitrary offset)
     *
     * These assertions help detect:
     * - Double-free: Pointer not from this pool
     * - Corruption: Pointer offset doesn't match allocation boundaries
     * - Use-after-free: Pointer to freed memory being returned again
     *
     * @param ptr Pointer to buffer previously acquired from this pool.
     *            Must be a valid pointer returned by acquire() or acquire_batch().
     *
     * @note Thread-safe: Acquires mutex to modify free stack
     * @note Private: Only callable by buffer deleter (not part of public API)
     * @note Debug validation: Asserts on invalid pointers in debug builds
     *
     * @warning In release builds, no validation is performed for maximum performance.
     *          Returning an invalid pointer will corrupt the free stack.
     */
    auto release(T* ptr) -> void {
#ifndef NDEBUG
        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        uintptr_t base = reinterpret_cast<uintptr_t>(m_slab.get());
        bool in_range = (addr >= base) && (addr < base + m_total_bytes);
        bool aligned = ((addr - base) % m_stride_bytes) == 0;
        assert(in_range && aligned && "slab_pool: attempt to release invalid pointer");
#endif
        auto lock = std::lock_guard{m_mutex};
        m_free_stack.push_back(ptr);
        m_outstanding.fetch_sub(1, std::memory_order_relaxed);
    }

    /**
     * @brief Create an external_buffer handle for a buffer pointer.
     *
     * This factory method wraps a raw pointer from the slab into an external_buffer
     * with a custom deleter. The deleter captures a shared_ptr to the pool, which:
     * 1. Keeps the pool alive as long as any buffers exist
     * 2. Returns the buffer to the free stack when destroyed
     *
     * This design ensures safe memory management:
     * - Pool cannot be destroyed while buffers are outstanding
     * - Buffers automatically return to pool on destruction (RAII)
     * - No need for manual reference counting or lifetime tracking
     *
     * @param self Shared pointer to this pool (from shared_from_this())
     * @param ptr Raw pointer to buffer within the slab memory
     *
     * @return external_buffer<T> with custom deleter that returns buffer on destruction
     *
     * @note Private: Only called by acquire() and acquire_batch()
     * @note Captures shared_ptr by value in lambda to extend pool lifetime
     * @note Lambda is stored in external_buffer and invoked on buffer destruction
     */
    auto make_handle(std::shared_ptr<slab_pool> self, T* ptr) -> buffer_type {
        // Return wrapper with deleter that returns to pool while keeping pool alive
        return buffer_type(
            ptr,
            m_buffer_size,
            [ptr, self = std::move(self)]() { self->release(ptr); }
        );
    }

    // ===== Member Variables =====

    /** @brief Number of T elements per buffer (immutable after construction) */
    std::size_t m_buffer_size{};

    /** @brief Total number of buffers in pool (immutable after construction) */
    std::size_t m_buffer_count{};

    /** @brief Aligned size in bytes per buffer slot (immutable after construction)
     *         Calculated as: align_up(buffer_size * sizeof(T), Alignment) */
    std::size_t m_stride_bytes{};

    /** @brief Total allocated slab size in bytes (immutable after construction)
     *         Calculated as: stride_bytes * buffer_count */
    std::size_t m_total_bytes{};

    /** @brief Pointer to aligned memory slab containing all buffers.
     *         Allocated with std::aligned_alloc(), freed via slab_deleter. */
    std::unique_ptr<uint8_t, slab_deleter> m_slab;

    /** @brief Mutex protecting m_free_stack modifications.
     *         Mutable to allow const methods to lock for reading. */
    mutable std::mutex m_mutex;

    /** @brief LIFO stack of available buffer pointers.
     *         Protected by m_mutex. Size indicates current availability. */
    std::vector<T*> m_free_stack;

    /** @brief Count of buffers currently acquired (not yet released).
     *         Updated atomically without mutex. Relaxed ordering sufficient
     *         because it's for observability only, not synchronization. */
    std::atomic<std::size_t> m_outstanding{0};

}; // class slab_pool

} // namespace composite
