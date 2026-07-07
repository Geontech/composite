/*
 * Copyright (C) 2024-2026 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "composite/util/export.hpp"

#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

/**
 * @file logger.hpp
 * @brief composite::logger — a thin logging facade that keeps the logging backend (spdlog) out
 *        of the public API.
 *
 * The framework logs through spdlog, but spdlog must NOT appear in any public header: it would
 * force every consumer of the installed package to have spdlog (and its bundled fmt) on their
 * include path, and it would pin the framework's public ABI to spdlog's. This facade exposes only
 * std::format-based logging; the spdlog logger/sink live entirely in logger.cpp (pimpl). The
 * backend is therefore swappable without touching the public API or any component.
 *
 * Per-level overloads come in two forms:
 *   - logger->warn("gain = {}", g);   // 1+ args: compile-time-checked std::format
 *   - logger->warn(some_runtime_msg); // 0 args: logged verbatim (no format parsing)
 * The 1-arg-minimum on the variadic form disambiguates a bare string from format_string<>.
 */
namespace composite {

enum class log_level { trace, debug, info, warn, error, critical, off };

class COMPOSITE_API logger {
public:
    /// Construct a named logger (colored stdout sink + the framework's standard pattern).
    explicit logger(std::string name);
    /// A no-op logger (used as a safe default before a real one is installed).
    logger();
    ~logger();
    logger(const logger&) = delete;
    logger& operator=(const logger&) = delete;

#define COMPOSITE_LOG_LEVEL_METHODS(LVL, ENUM)                                                                         \
    template <class Arg0, class... Args>                                                                               \
    void LVL(std::format_string<Arg0, Args...> fmt, Arg0&& a0, Args&&... args) {                                       \
        if (should_log(ENUM)) {                                                                                        \
            log(ENUM, std::format(fmt, std::forward<Arg0>(a0), std::forward<Args>(args)...));                          \
        }                                                                                                              \
    }                                                                                                                  \
    void LVL(std::string_view msg) { log(ENUM, msg); }

    COMPOSITE_LOG_LEVEL_METHODS(trace, log_level::trace)
    COMPOSITE_LOG_LEVEL_METHODS(debug, log_level::debug)
    COMPOSITE_LOG_LEVEL_METHODS(info, log_level::info)
    COMPOSITE_LOG_LEVEL_METHODS(warn, log_level::warn)
    COMPOSITE_LOG_LEVEL_METHODS(error, log_level::error)
    COMPOSITE_LOG_LEVEL_METHODS(critical, log_level::critical)
#undef COMPOSITE_LOG_LEVEL_METHODS

    [[nodiscard]] auto should_log(log_level level) const noexcept -> bool;
    auto set_level(log_level level) -> void;
    auto flush() -> void;
    [[nodiscard]] auto name() const -> const std::string&;

private:
    auto log(log_level level, std::string_view msg) -> void; // defined in logger.cpp (backend)

    struct impl;
    std::shared_ptr<impl> m_impl; // pimpl: the spdlog logger/sink live here, never in this header
};

/// Set the process-wide default log level (CLI --log-level). Mirrors the prior global behavior.
COMPOSITE_API auto set_global_log_level(log_level level) -> void;

/// Parse a level name ("trace"/"debug"/"info"/"warn"/"error"/"critical"/"off"); unknown -> info.
[[nodiscard]] COMPOSITE_API auto log_level_from_string(std::string_view name) -> log_level;

} // namespace composite
