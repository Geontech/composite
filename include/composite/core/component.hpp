/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "composite/ports/input_port.hpp"
#include "composite/ports/output_port.hpp"
#include "composite/ports/port_set.hpp"
#include "composite/properties/property_set.hpp"
#include "lifecycle.hpp"

#include <concepts>
#include <mutex>
#include <optional>
#include <span>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>

namespace composite {

enum class retval : int {
    NORMAL,
    NOOP,
    FINISH,
    NO_YIELD
}; // enum class retval

class component : public lifecycle {
    static constexpr uint32_t DEFAULT_DELAY{1000000};

public:
    struct connection {
        std::pair<std::string, std::string> output;
        std::pair<std::string, std::string> input;
    };

    virtual ~component() override {
        stop();
        if (m_logger) { m_logger->flush(); }
    }

    auto id() const noexcept -> const std::string& {
        return m_id;
    }

    auto initialize() -> void override {
        // To be implemented by subclasses
    }

    auto start() -> void override {
        m_thread.reset();
        m_enabled = true;
        m_thread.emplace(&component::thread_func, this);
        pthread_setname_np(m_thread->native_handle(), m_id.c_str());

        // Apply CPU affinity if configured
        if (m_cpu_affinity.has_value()) {
            if (pthread_setaffinity_np(m_thread->native_handle(), sizeof(*m_cpu_affinity), &(*m_cpu_affinity)) != 0) {
                m_logger->warn("Failed to set thread CPU affinity: {}", strerror(errno));
            }
        }
    }

    auto stop() -> void override {
        m_enabled = false;
        m_thread.reset();
    }

    virtual auto process() -> retval = 0;

    auto add_port(port_base& port) -> void {
        m_port_set.add_port(port);
    }

    auto add_port(port_base* port) -> void {
        m_port_set.add_port(port);
    }

    template <typename T>
    auto get_port(std::string_view name) -> T* {
        return m_port_set.get_port<T>(name);
    }

    auto ports() const -> const port_set::port_map_type& {
        return m_port_set.ports();
    }

    /**
     * @brief Connect this component's output port to another's input port
     *
     * @param output_port_name Name of output port on this component
     * @param other Target component
     * @param input_port_name Name of input port on target component
     * @return true if connection successful, false otherwise
     */
    auto connect(
      std::string_view output_port_name,
      std::shared_ptr<component> other,
      std::string_view input_port_name
    ) -> bool {
        // Get output port from this component
        auto* out_port = get_port<output_port_base>(output_port_name);
        if (out_port == nullptr) {
            m_logger->error("output port '{}' not found", output_port_name);
            return false;
        }

        // Get input port from target component
        if (other == nullptr) {
            m_logger->error("invalid input component pointer");
            return false;
        }
        auto* in_port = other->get_port<input_port_base>(input_port_name);
        if (in_port == nullptr) {
            m_logger->error("input port '{}' not found", input_port_name);
            return false;
        }

        // Check element type compatibility
        if (out_port->element_type_id() != in_port->element_type_id()) {
            m_logger->error(
                "type mismatch connecting {}:{} ({}) to {}:{} ({})",
                id(), output_port_name, out_port->element_type().name(),
                other->id(), input_port_name, in_port->element_type().name()
            );
            return false;
        }

        // Log mutability information for transfer optimization transparency
        m_logger->trace(
            "connecting {}:{} (mutability: {}) -> {}:{} (mutability: {})",
            id(), output_port_name, out_port->is_mutable() ? "mutable" : "immutable",
            other->id(), input_port_name, in_port->is_mutable() ? "mutable" : "immutable"
        );

        // Make the connection
        out_port->connect(in_port);

        // Record connection for tracking
        m_connections.push_back({
          .output = std::make_pair(id(), std::string{output_port_name}),
          .input = std::make_pair(other->id(), std::string{input_port_name})
        });
        m_logger->debug(
          "connected {}:{} -> {}:{}",
          id(), output_port_name,
          other->id(), input_port_name
        );

        return true;
    }

    auto connections() const -> const std::vector<connection>& {
        return m_connections;
    }

    // ========================================================================
    // Property Registration API
    // ========================================================================

    /**
     * @brief Register a scalar, optional, or scalar-list property
     * Excludes struct types and struct list types which use separate overloads.
     */
    template <typename T>
    requires (!properties::has_property_traits_v<T>) &&
             (!properties::is_vector_v<std::remove_cvref_t<T>> ||
              properties::is_scalar_type_v<typename std::remove_cvref_t<T>::value_type>)
    auto add_property(
      std::string_view name,
      T& ref,
      properties::config_type config = properties::config_type::INITIALIZE,
      std::string_view units = "") -> properties::property& {
        return m_prop_set.add(name, ref, config, units);
    }

    /**
     * @brief Register a struct property (no units parameter)
     * Requires property_traits<T> specialization.
     */
    template <typename T>
    requires properties::has_property_traits_v<T>
    auto add_property(
      std::string_view name,
      T& ref,
      properties::config_type config = properties::config_type::INITIALIZE) -> properties::property& {
        return m_prop_set.add(name, ref, config);
    }

    /**
     * @brief Register a struct list property (no units parameter)
     * Requires property_traits<T::value_type> specialization.
     */
    template <typename T>
    requires properties::is_vector_v<std::remove_cvref_t<T>> &&
             properties::has_property_traits_v<typename std::remove_cvref_t<T>::value_type>
    auto add_property(
      std::string_view name, T& ref,
      properties::config_type config = properties::config_type::INITIALIZE) -> properties::property& {
        return m_prop_set.add(name, ref, config);
    }

    // ========================================================================
    // Property Access
    // ========================================================================

    template <typename T>
    auto get_property(std::string_view name) const -> T {
        return m_prop_set.get<T>(name);
    }

    // ========================================================================
    // Property Setting
    // ========================================================================

    auto set_properties(
      const std::vector<std::pair<std::string, std::string>>& values,
      properties::config_type config = properties::config_type::INITIALIZE,
      bool allow_unknown_key = false) -> void {
        m_prop_change_requested = true;
        auto lk = std::scoped_lock{m_prop_mtx};
        m_prop_change_requested = false;

        try {
            m_prop_set.set_batch(values, config, allow_unknown_key);
        } catch (const properties::key_error& err) {
            logger()->error("{}: unknown property '{}'", m_id, err.name);
            throw;
        } catch (const properties::config_error& err) {
            logger()->error("{}: property '{}' is not runtime configurable", m_id, err.name);
            throw;
        } catch (const properties::value_error& err) {
            logger()->error("{}: invalid value for property '{}'", m_id, err.name);
            throw;
        } catch (const properties::listener_rejected& err) {
            logger()->error("{}: change listener rejected update to '{}'", m_id, err.name);
            throw;
        } catch (const std::exception& ex) {
            logger()->error("{}: unexpected property error: {}", m_id, ex.what());
            throw;
        }
    }

    auto append_struct_list(
      std::string_view name,
      std::span<const std::pair<std::string, std::string>> fields,
      properties::config_type config = properties::config_type::INITIALIZE) -> std::size_t {
        m_prop_change_requested = true;
        auto lk = std::scoped_lock{m_prop_mtx};
        m_prop_change_requested = false;

        try {
            return m_prop_set.append_struct_list(name, fields, config);
        } catch (const properties::key_error& err) {
            logger()->error("{}: unknown property '{}'", m_id, err.name);
            throw;
        } catch (const properties::config_error& err) {
            logger()->error("{}: property '{}' is not runtime configurable", m_id, err.name);
            throw;
        } catch (const properties::value_error& err) {
            logger()->error("{}: invalid value for property '{}'", m_id, err.name);
            throw;
        } catch (const properties::listener_rejected& err) {
            logger()->error("{}: change listener rejected update to '{}'", m_id, err.name);
            throw;
        } catch (const std::exception& ex) {
            logger()->error("{}: unexpected property error: {}", m_id, ex.what());
            throw;
        }
    }

    auto update_struct_list_element(
      std::string_view name,
      std::size_t index,
      std::span<const std::pair<std::string, std::string>> fields,
      properties::config_type config = properties::config_type::INITIALIZE) -> void {
        m_prop_change_requested = true;
        auto lk = std::scoped_lock{m_prop_mtx};
        m_prop_change_requested = false;

        try {
            m_prop_set.update_struct_list_element(name, index, fields, config);
        } catch (const properties::key_error& err) {
            logger()->error("{}: unknown property '{}'", m_id, err.name);
            throw;
        } catch (const properties::config_error& err) {
            logger()->error("{}: property '{}' is not runtime configurable", m_id, err.name);
            throw;
        } catch (const properties::value_error& err) {
            logger()->error("{}: invalid value for property '{}'", m_id, err.name);
            throw;
        } catch (const properties::listener_rejected& err) {
            logger()->error("{}: change listener rejected update to '{}'", m_id, err.name);
            throw;
        } catch (const std::exception& ex) {
            logger()->error("{}: unexpected property error: {}", m_id, ex.what());
            throw;
        }
    }

    // ========================================================================
    // Change Listener Registration
    // ========================================================================

    auto add_property_change_listener(std::string_view name, properties::property::change_listener_fn func) -> void {
        m_prop_set.add_change_listener(name, std::move(func));
    }

    auto add_property_change_listener(std::string_view name, properties::property::indexed_change_listener_fn func) -> void {
        m_prop_set.add_change_listener(name, std::move(func));
    }

    virtual auto property_change_handler() -> void {
        // To be implemented by subclasses
        // Gets executed at the end of set_properties function
    }

    auto properties() const -> const properties::property_set::property_map& {
        return m_prop_set.properties();
    }

    auto property_set() const -> const properties::property_set& {
        return m_prop_set;
    }

    auto log_level(spdlog::level::level_enum level) const -> void {
        m_logger->set_level(level);
    }

    /**
     * @brief Set the CPU affinity for this component's thread
     * @param cpuset CPU set to apply when thread starts
     *
     * This must be called before start(). The affinity will be applied
     * when the component thread is created.
     */
    auto set_cpu_affinity(const cpu_set_t& cpuset) -> void {
        m_cpu_affinity = cpuset;
    }

    /**
     * @brief Apply pending lifecycle changes based on enabled property
     *
     * Must be called after set_properties() completes to avoid deadlock.
     * Checks if enabled property changed and starts/stops component accordingly,
     * managing input port depths to prevent memory bloat.
     */
    auto apply_lifecycle_changes() -> void {
        if (!m_lifecycle_change_pending) {
            return;
        }
        m_lifecycle_change_pending = false;

        bool is_running = m_thread.has_value();

        if (m_enabled && !is_running) {
            // Need to start
            logger()->debug("Enabling component '{}'", m_id);
            resume_input_ports();
            start();
        } else if (!m_enabled && is_running) {
            // Need to stop
            logger()->debug("Disabling component '{}'", m_id);
            pause_input_ports();  // Pause ports first to prevent new data
            stop();  // Then stop the thread
        }
    }

protected:
    explicit component(std::string_view id) :
      m_id(id),
      m_sink(std::make_shared<spdlog::sinks::stdout_color_sink_mt>()),
      m_logger(std::make_shared<spdlog::logger>(m_id, m_sink)) {
        if (m_id.empty()) {
            throw std::invalid_argument("component id cannot be empty");
        }
        auto pattern = std::format("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [{}] %v", m_id);
        m_logger->set_pattern(pattern);
        add_property("noop_thread_delay", m_delay).units("ns");
        add_property("enabled", m_enabled)
            .configurability(properties::config_type::RUNTIME)
            .change_listener([this]() -> bool {
                // Mark that lifecycle change is pending
                m_lifecycle_change_pending = true;
                return true;
            });
    }

    auto logger() const -> std::shared_ptr<spdlog::logger> {
        return m_logger;
    }

private:
    std::string m_id;
    std::shared_ptr<spdlog::sinks::stdout_color_sink_mt> m_sink;
    std::shared_ptr<spdlog::logger> m_logger;
    std::optional<std::jthread> m_thread;
    uint32_t m_delay{DEFAULT_DELAY};
    bool m_enabled{true};
    std::optional<cpu_set_t> m_cpu_affinity;
    port_set m_port_set;
    properties::property_set m_prop_set;
    std::mutex m_prop_mtx;
    std::atomic_bool m_prop_change_requested{};
    std::atomic_bool m_lifecycle_change_pending{false};
    std::vector<connection> m_connections;
    std::map<std::string, std::size_t> m_saved_input_depths;

    /**
     * @brief Pause all input ports by setting their queue depth to 0
     *
     * Saves current depth values and sets all input port depths to 0,
     * preventing queue growth and memory bloat when component is stopped.
     * Depths can be restored via resume_input_ports().
     */
    auto pause_input_ports() -> void {
        m_saved_input_depths.clear();
        for (const auto& [name, port] : m_port_set.ports()) {
            if (auto* input_port = dynamic_cast<input_port_base*>(port)) {
                // Save current depth
                m_saved_input_depths[name] = input_port->depth();
                // Set to 0 to drop all incoming data
                input_port->depth(0);
                logger()->debug("Paused input port '{}' (saved depth: {})", name, m_saved_input_depths[name]);
            }
        }
    }

    /**
     * @brief Resume all input ports by restoring their saved queue depths
     *
     * Restores depths that were saved by pause_input_ports(). If no saved
     * depth exists for a port, it remains at its current depth.
     */
    auto resume_input_ports() -> void {
        for (const auto& [name, saved_depth] : m_saved_input_depths) {
            if (auto* input_port = dynamic_cast<input_port_base*>(m_port_set.get_port<input_port_base>(name))) {
                input_port->depth(saved_depth);
                logger()->debug("Resumed input port '{}' (restored depth: {})", name, saved_depth);
            }
        }
        m_saved_input_depths.clear();
    }

    auto thread_func(std::stop_token token) -> void {
        using enum retval;
        while (!token.stop_requested()) {
            if (m_prop_change_requested) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            auto lk = std::scoped_lock{m_prop_mtx};
            retval res;
            try {
                res = process();
            } catch (const std::exception& e) {
                logger()->error("component '{}' process() threw an exception: {}; pausing input ports and stopping thread.", m_id, e.what());
                pause_input_ports();
                res = FINISH;
            } catch (...) {
                logger()->error("component '{}' process() threw an unknown exception; pausing input ports and stopping thread.", m_id);
                pause_input_ports();
                res = FINISH;
            }
            if (res == NOOP) {
                std::this_thread::sleep_for(std::chrono::nanoseconds{m_delay});
            } else if (res == FINISH) {
                break;
            } else if (res == NORMAL) {
                std::this_thread::yield();
            }
        }
    }

}; // class component

} // namespace composite
