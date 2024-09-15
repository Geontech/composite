#pragma once

#include <cstdint>

namespace caddie {

struct timestamp {
    uint32_t sec{};
    uint64_t psec{};
}; // struct timestamp

} // namespace caddie
