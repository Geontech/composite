/*
 * Copyright (C) 2024-2026 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#include "composite/core/logger.hpp"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace composite {

namespace {
auto to_spdlog(log_level level) -> spdlog::level::level_enum {
    switch (level) {
    case log_level::trace:
        return spdlog::level::trace;
    case log_level::debug:
        return spdlog::level::debug;
    case log_level::info:
        return spdlog::level::info;
    case log_level::warn:
        return spdlog::level::warn;
    case log_level::error:
        return spdlog::level::err;
    case log_level::critical:
        return spdlog::level::critical;
    case log_level::off:
        return spdlog::level::off;
    }
    return spdlog::level::info;
}
} // namespace

// The spdlog logger/sink live here only — never in a public header.
struct logger::impl {
    std::shared_ptr<spdlog::logger> sp;
};

logger::logger() = default; // null logger: m_impl == nullptr (should_log() false, log() a no-op)

logger::logger(std::string name) : m_impl(std::make_shared<impl>()) {
    auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    m_impl->sp = std::make_shared<spdlog::logger>(name, std::move(sink));
    // Same per-component pattern as before: timestamp, colored level, [id], message.
    m_impl->sp->set_pattern(std::format("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [{}] %v", name));
}

logger::~logger() = default;

auto logger::log(log_level level, std::string_view msg) -> void {
    if (m_impl && m_impl->sp) {
        m_impl->sp->log(to_spdlog(level), msg); // spdlog logs a string_view verbatim (no fmt)
    }
}

auto logger::should_log(log_level level) const noexcept -> bool {
    return m_impl && m_impl->sp && m_impl->sp->should_log(to_spdlog(level));
}

auto logger::set_level(log_level level) -> void {
    if (m_impl && m_impl->sp) {
        m_impl->sp->set_level(to_spdlog(level));
    }
}

auto logger::flush() -> void {
    if (m_impl && m_impl->sp) {
        m_impl->sp->flush();
    }
}

auto logger::name() const -> const std::string& {
    static const std::string empty;
    return (m_impl && m_impl->sp) ? m_impl->sp->name() : empty;
}

auto set_global_log_level(log_level level) -> void {
    spdlog::set_level(to_spdlog(level));
}

auto log_level_from_string(std::string_view name) -> log_level {
    if (name == "trace") {
        return log_level::trace;
    }
    if (name == "debug") {
        return log_level::debug;
    }
    if (name == "info") {
        return log_level::info;
    }
    if (name == "warn" || name == "warning") {
        return log_level::warn;
    }
    if (name == "error" || name == "err") {
        return log_level::error;
    }
    if (name == "critical") {
        return log_level::critical;
    }
    if (name == "off") {
        return log_level::off;
    }
    return log_level::info;
}

} // namespace composite
