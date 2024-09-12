#pragma once

#include <algorithm>
#include <memory>

namespace caddie {

template <typename T>
class aligned_mem {
public:
    using value_type = T;
    using reference_type = T&;
    using const_reference_type = const T&;

    explicit aligned_mem(std::size_t alignment, std::size_t count) :
      m_data(static_cast<value_type*>(std::aligned_alloc(alignment, count * sizeof(value_type)))),
      m_alignment(alignment),
      m_count(count) {}
    
    ~aligned_mem() {
        std::free(m_data);
    }

    aligned_mem(const aligned_mem<T>& other) : 
      m_data(static_cast<value_type*>(std::aligned_alloc(other.m_alignment, other.size_bytes()))),
      m_alignment(other.m_alignment),
      m_count(other.m_count) {
        std::copy(other.m_data, other.m_data + size(), m_data);
    }

    aligned_mem<T>& operator=(const aligned_mem<T>&) = delete;
    aligned_mem(aligned_mem<T>&&) = delete;
    aligned_mem<T>& operator=(aligned_mem<T>&&) = delete;

    auto data() -> T* {
        return m_data;
    }

    auto data() const -> const T* {
        return m_data;
    }

    auto at(std::size_t pos) -> reference_type {
        if (pos >= size()) {
            throw std::out_of_range("aligned_mem: index out of range");
        }
        return *(m_data + pos);
    }

    auto at(std::size_t pos) const -> const_reference_type {
        if (pos >= size()) {
            throw std::out_of_range("aligned_mem: index out of range");
        }
        return *(m_data + pos);
    }

    auto alignment() const -> std::size_t {
        return m_alignment;
    }

    auto size() const -> std::size_t {
        return m_count;
    }

    auto size_bytes() const -> std::size_t {
        return size() * sizeof(value_type);
    }

private:
    value_type* m_data{nullptr};
    std::size_t m_alignment{};
    std::size_t m_count{};

}; // class aligned_mem

template <typename T>
auto make_aligned(std::size_t alignment, std::size_t count) -> std::unique_ptr<aligned_mem<T>> {
    return std::make_unique<aligned_mem<T>>(alignment, count);
}

} // namespace caddie