#pragma once

#include <cstdint>

namespace composite {

struct timestamp {
    uint32_t sec{};
    uint64_t psec{};
}; // struct timestamp

} // namespace composite
