/*
 * Copyright (C) 2024-2025 Geon Technologies, LLC
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#pragma once

#include "composite/core/component.hpp"
#include "composite/dpdk/config.hpp"
#include "composite/transports/transport.hpp"
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

auto close_func(void* p) -> void;

using component_handles_type = std::vector<std::unique_ptr<void, decltype(&close_func)>>;

auto generate_app_name() -> std::string;

auto make_component(const nlohmann::json& comp_json, component_handles_type& handles) -> std::shared_ptr<composite::component>;

auto validate_connection(const nlohmann::json& conn_json) -> std::tuple<std::string, std::string, std::string>;

/**
 * @brief Transport definition structure
 *
 * Stores configuration needed to create transport instances.
 * Multiple ports can reference the same definition and get their own instance.
 */
struct transport_definition {
    std::string id;
    transport_type type;
    nlohmann::json config;  // Type-specific configuration
};

/**
 * @brief Transport registry type
 *
 * Maps transport ID (string) to transport definition
 */
using transport_registry = std::map<std::string, transport_definition>;

/**
 * @brief Parse transport definitions from JSON configuration
 * @param transports_json JSON array of transport definitions
 * @return Map of transport ID to transport definition, and error message (empty if success)
 *
 * Expected JSON format:
 * [
 *   {
 *     "id": "nats_main",
 *     "type": "nats",
 *     "url": "nats://localhost:4222",
 *     "subject": "sensors.temp"
 *   }
 * ]
 */
auto parse_transports(const nlohmann::json& transports_json)
  -> std::tuple<transport_registry, std::string>;

/**
 * @brief Create a transport instance from a definition
 * @param def Transport definition
 * @return Transport instance, or nullptr with error message
 */
auto create_transport(const transport_definition& def)
  -> std::tuple<std::unique_ptr<transport_base>, std::string>;

/**
 * @brief Attach transports to component ports based on configuration
 * @param component Component to attach transports to
 * @param transports_json JSON object mapping port names to transport IDs
 * @param registry Transport registry to look up transports
 * @return Error message (empty if success)
 *
 * Expected JSON format:
 * {
 *   "data_out": ["nats_main", "nats_backup"],
 *   "commands_in": ["nats_commands"]
 * }
 */
auto attach_component_transports(
  std::shared_ptr<component> component,
  const nlohmann::json& transports_json,
  const transport_registry& registry
) -> std::string;

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