/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "composite/core/component.hpp"
#include "composite/dpdk/config.hpp"
#ifdef COMPOSITE_USE_OPENTELEMETRY
#include "composite/telemetry/config.hpp"
#endif

#include <dlfcn.h>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <tuple>
#include <vector>

namespace composite {

auto generate_app_name() -> std::string;

/// Load a component from its shared library and return it as a self-owning
/// shared_ptr: the dlopen handle is captured in the returned shared_ptr's
/// deleter, so the library is dlclose()d only after the component is destroyed.
/// This removes the previous separate handles vector (and its concurrent-POST
/// data race) and the main.cpp declaration-order dependency, and makes safe
/// per-component unload possible. Returns nullptr on any load/create failure
/// (logged); never throws out of create() (consumed while the mapping is live).
auto make_component(const nlohmann::json& comp_json) -> std::shared_ptr<composite::component>;

auto validate_connection(const nlohmann::json& conn_json) -> std::tuple<std::string, std::string, std::string>;

/// Post-construction setup shared by the config loader (main.cpp) and the REST create path
/// (POST /app/components). The two paths MUST stay behaviorally identical — they diverged
/// once (the v0.5.0 known issue: REST-created components silently skipped the log level and
/// cpu_affinity), and a shared helper is what keeps that from recurring.
///
/// Applies, from @p comp_json and process-wide state:
///  - the process-wide component log level (global_log_level());
///  - "cpu_affinity" (logical indices resolved against process_available_cores()).
///
/// Deliberately NOT here: property application (the two paths differ on globals merging)
/// and initialize() (the config path defers it to application::initialize(); the REST path
/// calls it per component because the application has already initialized).
///
/// @throws std::runtime_error on an unparsable "cpu_affinity" value.
auto setup_component(composite::component& comp, const nlohmann::json& comp_json) -> void;

/**
 * @brief Parse DPDK configuration from JSON
 * @param dpdk_json JSON object containing DPDK configuration
 * @return DPDK configuration structure
 *
 * Expected JSON format:
 * {
 *   "enabled": true,
 *   "eal_args": ["-l", "0-3", "-n", "4"],
 *   "ports": [
 *     {
 *       "port_id": 0,
 *       "interface": "eth0",
 *       "rx_queues": 4,
 *       ...
 *     }
 *   ]
 * }
 */
auto parse_dpdk_config(const nlohmann::json& dpdk_json) -> dpdk::config;

#ifdef COMPOSITE_USE_OPENTELEMETRY
/**
 * @brief Parse telemetry configuration from JSON
 * @param telemetry_json JSON object containing telemetry configuration
 * @return Telemetry configuration structure
 *
 * Expected JSON format:
 * {
 *   "enabled": true,
 *   "service_name": "my_app",
 *   "service_version": "1.0.0",
 *   "export_interval": 10000,
 *   "exporter": {
 *     "endpoint": "http://otel-collector:4318",
 *     "timeout": 10000,
 *     "headers": "Authorization=Bearer token"
 *   }
 * }
 */
auto parse_telemetry_config(const nlohmann::json& telemetry_json) -> telemetry::config;
#endif

} // namespace composite