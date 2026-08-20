/*
 * Copyright (C) 2026 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 *
 * Thread naming for `top -H`, `perf`, and gdb.
 */

#pragma once

#include <pthread.h>

#include <string>
#include <string_view>

namespace composite {

/// Longest name Linux accepts, excluding the NUL.
inline constexpr std::size_t k_max_thread_name = 15;

/// Name an arbitrary thread, truncating to the platform limit.
/// @return true if the name was applied.
inline auto set_thread_name(pthread_t handle, std::string_view name) -> bool {
    const std::string truncated{name.substr(0, k_max_thread_name)};
    return pthread_setname_np(handle, truncated.c_str()) == 0;
}

/// Name the calling thread.
inline auto set_thread_name(std::string_view name) -> bool {
    return set_thread_name(pthread_self(), name);
}

/// Compose "<stem>.<suffix>" to fit the limit, trimming the stem so the suffix survives:
/// "channelizer_bank" + "w7" -> "channelize.w7".
inline auto thread_name_with_suffix(std::string_view stem, std::string_view suffix) -> std::string {
    const std::size_t reserved = suffix.size() + 1;
    if (reserved >= k_max_thread_name) {
        return std::string{suffix.substr(0, k_max_thread_name)};
    }
    const std::size_t stem_room = k_max_thread_name - reserved;
    std::string out{stem.substr(0, stem_room)};
    out += '.';
    out += suffix;
    return out;
}

} // namespace composite
