/*
 * Copyright (C) 2024-2026 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include <atomic>
#include <memory>
#include <utility>

/**
 * @file snapshot.hpp
 * @brief snapshot<T> — atomically published, immutable config for threads the park never quiesces.
 */

namespace composite {

/**
 * @brief An atomically published, immutable value readable from threads the park never parks.
 *
 * The park coordinator synchronizes property writes with the component's MAIN worker only.
 * Threads it does not quiesce — a pipeline_component's pool workers, a source's receiver
 * threads — must not read live config members directly: a concurrent commit tears scalar reads
 * and frees owned storage under them (a std::string or vector member read becomes a
 * use-after-free). snapshot<T> closes that gap: the park-synchronized side (the constructor,
 * property_change_handler(), a config<T> on_apply) publish()es a fresh immutable value, and any
 * thread load()s a shared_ptr<const T> whose refcount keeps that value alive for as long as the
 * reader holds it — however many times the publisher has since moved on.
 *
 * A default-constructed snapshot load()s nullptr: publish an initial value from the component's
 * constructor (or construct the snapshot with one) so readers never need a null check.
 *
 * Cost: load() is one atomic shared_ptr load plus a refcount, which is negligible per FRAME but
 * too heavy per SAMPLE — for a single trivially-copyable scalar read on such a path, a plain
 * std::atomic<T> remains the right tool.
 */
template <typename T>
class snapshot {
public:
    snapshot() = default;
    explicit snapshot(T initial) : m_value(std::make_shared<const T>(std::move(initial))) {}

    /// Publish a new value (park-synchronized side). Readers still holding a previously loaded
    /// value keep it alive; new load()s see this one.
    auto publish(T value) -> void {
        m_value.store(std::make_shared<const T>(std::move(value)), std::memory_order_release);
    }

    /// Publish an already-built value (e.g. the result of a make_*_config() helper).
    auto publish(std::shared_ptr<const T> value) -> void { m_value.store(std::move(value), std::memory_order_release); }

    /// Load the current value (any thread, any time). The returned shared_ptr keeps it alive.
    [[nodiscard]] auto load() const -> std::shared_ptr<const T> { return m_value.load(std::memory_order_acquire); }

private:
    std::atomic<std::shared_ptr<const T>> m_value{};
};

} // namespace composite
